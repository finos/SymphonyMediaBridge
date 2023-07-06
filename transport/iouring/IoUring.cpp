#include "transport/iouring/IoUring.h"
#include "utils/SocketAddress.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <linux/io_uring.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

namespace iouring
{
// https://unixism.net/loti/low_level.html
// https://man7.org/linux/man-pages/man7/io_uring.7.html
// liburing with UDP https://patchwork.kernel.org/project/io-uring/patch/20220726121502.1958288-6-dylany@fb.com/

// https://kernel.googlesource.com/pub/scm/linux/kernel/git/daniel.lezcano/linux/+/refs/heads/master/tools/io_uring

// https://manpages.debian.org/unstable/liburing-dev/io_uring_setup.2.en.html

/**
 * @returns ring_fd
 */
int setup(unsigned entries, struct io_uring_params* p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}

// https://manpages.ubuntu.com/manpages/kinetic/en/man2/io_uring_enter.2.html
int enter(int ring_fd, unsigned int to_submit, unsigned int min_complete, unsigned int flags)
{
    return (int)syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete, flags, NULL, 0);
}

// https://manpages.ubuntu.com/manpages/kinetic/en/man2/io_uring_register.2.html

// buffer must be allocated using mmap MAP_ANONYMOUS
// unclear how these would be used by SQ kernel thread. If we read we issue a recv request that will be complete later.
int registerBuffers(int ring_fd, void* basePointer, size_t itemCount, size_t itemSize, size_t itemPadding)
{
    uint8_t* p = reinterpret_cast<uint8_t*>(basePointer);
    iovec buffers[itemCount];
    for (size_t i = 0; i < itemCount; ++i)
    {
        buffers[i].iov_base = p;
        buffers[i].iov_len = itemSize;
        p += (itemSize + itemPadding);
    }

    return (int)syscall(__NR_io_uring_register, ring_fd, IORING_REGISTER_BUFFERS, buffers, itemCount, NULL, 0);
}

int registerCompletionEvent(int ring_fd, int event_fd)
{
    return (int)syscall(__NR_io_uring_register, ring_fd, IORING_REGISTER_EVENTFD, event_fd, 1, NULL, 0);
}

int unRegisterCompletionEvent(int ring_fd, int event_fd)
{
    return (int)syscall(__NR_io_uring_register, ring_fd, IORING_UNREGISTER_EVENTFD, event_fd, 1, NULL, 0);
}

class MessageHeader : public concurrency::StackItem
{
public:
    void init()
    {
        std::memset(&header, 0, sizeof(header));
        header.msg_iov = vec;
        header.msg_iovlen = 0;
        std::memset(&target, 0, sizeof(target));
    }

    void addBuffer(const void* data, size_t s)
    {
        if (header.msg_iovlen >= 3)
        {
            return;
        }
        const auto i = header.msg_iovlen;
        header.msg_iov[i].iov_base = const_cast<void*>(data);
        header.msg_iov[i].iov_len = s;
        ++header.msg_iovlen;
    }

    void setTarget(const transport::SocketAddress& addr)
    {
        std::memcpy(&target, addr.getSockAddr(), addr.getSockAddrSize());
        header.msg_name = &target;
        header.msg_namelen = addr.getSockAddrSize();
    }

    msghdr header;
    iovec vec[3];
    transport::RawSockAddress target;
};

struct SqeItem : public concurrency::StackItem
{
    io_uring_sqe* sqe;
};

struct IoUring::IoSubmitRing
{
    __u32* head;
    _Atomic(__u32)* tail;
    __u32* ring_mask;
    __u32* ring_entries;
    _Atomic(__u32)* flags;
    __u32* array;
    io_uring_sqe* items;

    IoSubmitRing(io_uring_params& ring, memory::MemMap& m, memory::MemMap& itemsMemory)
    {
        head = m.get<__u32>(ring.sq_off.head);
        tail = m.get<_Atomic(__u32)>(ring.sq_off.tail);
        ring_mask = m.get<__u32>(ring.sq_off.ring_mask);
        ring_entries = m.get<__u32>(ring.sq_off.ring_entries);
        flags = m.get<_Atomic(__u32)>(ring.sq_off.flags);
        array = m.get<__u32>(ring.sq_off.array);
        items = itemsMemory.get<io_uring_sqe>();
    }
};

struct IoUring::IoCompletionRing
{
    _Atomic(__u32)* head;
    _Atomic(__u32)* tail;
    __u32* ring_mask;
    __u32* ring_entries;
    __u32* overflow;
    io_uring_cqe* items;

    IoCompletionRing(io_uring_params& ring, memory::MemMap& m)
    {
        head = m.get<_Atomic(__u32)>(ring.cq_off.head);
        tail = m.get<_Atomic(__u32)>(ring.cq_off.tail);
        ring_mask = m.get<__u32>(ring.cq_off.ring_mask);
        ring_entries = m.get<__u32>(ring.cq_off.ring_entries);
        items = m.get<io_uring_cqe>(ring.cq_off.cqes);
    }
};

IoUring::IoUring() : _ringFd(-1), _submitRing(nullptr), _completionRing(nullptr) {}

IoUring::~IoUring()
{
    if (_ringFd != -1)
    {
        auto result = ::close(_ringFd);
        assert(result == 0);
    }

    delete _submitRing;
    delete _completionRing;
}

bool IoUring::createForUdp(const size_t queueDepth)
{
    if (_ringFd != -1)
    {
        return false;
    }

    io_uring_params ring;
    memset(&ring, 0, sizeof(ring));
    ring.flags = IORING_SETUP_SQPOLL;
    // sq_thread_cpu,  sq_thread_idle may be configured too. It could be that it is beneficial to use same cpu for
    // recv and send as is used by SQ kernel thread
    _ringFd = iouring::setup(queueDepth, &ring);
    if (_ringFd == -1)
    {
        assert(false);
        return false;
    }

    const int submitRingSize = ring.sq_off.array + ring.sq_entries * sizeof(unsigned);
    const int completionRingSize = ring.cq_off.cqes + ring.cq_entries * sizeof(io_uring_cqe);

    if (!_submitItems.allocateAtLeast(ring.sq_entries * sizeof(io_uring_sqe),
            PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_POPULATE,
            _ringFd,
            IORING_OFF_SQES))
    {
        return false;
    }

    if (ring.features & IORING_FEAT_SINGLE_MMAP)
    {
        // kernel will associate these memory areas with the ring fd
        if (!_submitQueueMemory.allocateAtLeast(std::max(submitRingSize, completionRingSize),
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_POPULATE,
                _ringFd,
                IORING_OFF_SQ_RING))
        {
            return false;
        }
        _submitRing = new IoSubmitRing(ring, _submitQueueMemory, _submitItems);
        _completionRing = new IoCompletionRing(ring, _submitQueueMemory);
    }
    else
    {
        if (!_submitQueueMemory.allocateAtLeast(submitRingSize,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_POPULATE,
                _ringFd,
                IORING_OFF_SQ_RING))
        {
            return false;
        }
        _submitRing = new IoSubmitRing(ring, _submitQueueMemory, _submitItems);

        // kernel will associate this area with the ring fd as CQ
        if (!_completionQueueMemory.allocateAtLeast(completionRingSize,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_POPULATE,
                _ringFd,
                IORING_OFF_CQ_RING))
        {
            return false;
        }

        _completionRing = new IoCompletionRing(ring, _completionQueueMemory);
    }

    const size_t hdrAreaSize = memory::roundUpToPage(queueDepth * sizeof(MessageHeader));
    _messageHeadersMemory.allocate(hdrAreaSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS);
    auto* h = _messageHeadersMemory.get<MessageHeader>();
    for (size_t i = 0; i < queueDepth; ++i)
    {
        _messageHeaders.push(h);
        ++h;
    }

    // could be that we need to register _messageHeadersMemory with the ring fd

    return true;
}

// single threaded. Must post jobs on serial queue here. Good perhaps as we want packets to be sent in order most of
// the time. buffer should be in a mmap memory with properties: MAP_ANONYMOUS
// https://manpages.ubuntu.com/manpages/kinetic/en/man2/sendmsg.2.html
void IoUring::send(int fd, const void* buffer, size_t length, const transport::SocketAddress& target, uint64_t cookie)
{
    MessageHeader* msgHeader;
    concurrency::StackItem* item;
    if (!_messageHeaders.pop(item))
    {
        return;
    }
    msgHeader = reinterpret_cast<MessageHeader*>(item);

    msgHeader->init();

    msgHeader->addBuffer(buffer, length);
    msgHeader->setTarget(target);

    // auto sqe = allocSqe();
    io_uring_sqe* sqe;
    const auto tail = *_submitRing->tail;
    const auto index = tail & (*_submitRing->ring_mask);
    sqe = &_submitRing->items[index];
    std::memset(sqe, 0, sizeof(*sqe));
    sqe->addr = reinterpret_cast<unsigned long long>(msgHeader);
    sqe->fd = fd;
    sqe->user_data = cookie;

    // TODO fill sqe
    _submitRing->array[index] = index;
    ::atomic_store_explicit(_submitRing->tail, tail + 1, std::memory_order_release);

    const unsigned flags = ::atomic_load_explicit(_submitRing->flags, std::memory_order_relaxed);
    if (flags & IORING_SQ_NEED_WAKEUP)
        iouring::enter(_ringFd, 0, 0, IORING_ENTER_SQ_WAKEUP);
}

void IoUring::receive(void* buffer, size_t length, uint64_t cookie) {}

bool IoUring::registerCompletionEvent(int eventFd)
{
    return 0 == iouring::registerCompletionEvent(_ringFd, eventFd);
}

bool IoUring::unRegisterCompletionEvent(int eventFd)
{
    return 0 == iouring::unRegisterCompletionEvent(_ringFd, eventFd);
}

bool IoUring::getCompletedItem(CompletionInfo& item)
{
    return false;
}

bool IoUring::hasCompletedItems() const
{
    const auto head = ::atomic_load_explicit(_completionRing->head, memory_order_relaxed);
    const auto tail = atomic_load_explicit(_completionRing->tail, memory_order_relaxed);
    return head != tail;
}

// must be allocated with mmap, MAP_ANONYMOUS
bool IoUring::registerBuffers(void* buffer, size_t count, size_t itemSize, size_t itemPadding)
{
    return 0 == iouring::registerBuffers(_ringFd, buffer, count, itemSize, itemPadding);
}

} // namespace iouring
