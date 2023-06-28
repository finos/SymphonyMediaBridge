#include "transport/iouring/IoUring.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <linux/io_uring.h>
#include <sys/mman.h>
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

struct IoUring::IoSubmitRing
{
    unsigned* head;
    unsigned* tail;
    unsigned* ring_mask;
    unsigned* ring_entries;
    unsigned* flags;
    unsigned* array;
    io_uring_sqe* items;
};

struct IoUring::IoCompletionRing
{
    unsigned* head;
    unsigned* tail;
    unsigned* ring_mask;
    unsigned* ring_entries;
    io_uring_cqe* items;
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

bool IoUring::createForUdp(size_t queueDepth)
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

    int submitRingSize = ring.sq_off.array + ring.sq_entries * sizeof(unsigned);
    int completionRingSize = ring.cq_off.cqes + ring.cq_entries * sizeof(io_uring_cqe);

    unsigned int* completionQueue = nullptr;
    if (ring.features & IORING_FEAT_SINGLE_MMAP)
    {
        submitRingSize = std::max(submitRingSize, completionRingSize);
        completionRingSize = submitRingSize;
    }

    _submitQueueMemory.allocate(submitRingSize,
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE,
        _ringFd,
        IORING_OFF_SQ_RING);

    _submitItems.allocate(ring.sq_entries * sizeof(io_uring_sqe),
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE,
        _ringFd,
        IORING_OFF_SQES);

    if (!_submitQueueMemory.isGood() || !_submitItems.isGood())
    {
        return false;
    }

    _submitRing->head = _submitQueueMemory.get<unsigned int>() + ring.sq_off.head;
    _submitRing->tail = _submitQueueMemory.get<unsigned int>() + ring.sq_off.tail;
    _submitRing->ring_mask = _submitQueueMemory.get<unsigned int>() + ring.sq_off.ring_mask;
    _submitRing->ring_entries = _submitQueueMemory.get<unsigned int>() + ring.sq_off.ring_entries;
    _submitRing->flags = _submitQueueMemory.get<unsigned int>() + ring.sq_off.flags;
    _submitRing->array = _submitQueueMemory.get<unsigned int>() + ring.sq_off.array;

    if (ring.features & IORING_FEAT_SINGLE_MMAP)
    {
        completionQueue = _submitQueueMemory.get<unsigned int>();
    }
    else
    {
        if (!_completionQueueMemory.allocate(completionRingSize,
                PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_POPULATE,
                _ringFd,
                IORING_OFF_CQ_RING))
        {
            return false;
        }
        completionQueue = _completionQueueMemory.get<unsigned int>();
    }

    _completionRing->head = completionQueue + ring.cq_off.head;
    _completionRing->tail = completionQueue + ring.cq_off.tail;
    _completionRing->ring_mask = completionQueue + ring.cq_off.ring_mask;
    _completionRing->ring_entries = completionQueue + ring.cq_off.ring_entries;
    _completionRing->items = reinterpret_cast<io_uring_cqe*>(completionQueue + ring.cq_off.cqes);
    return true;
}

// single threaded. Must post jobs on serial queue here. Good perhaps as we want packets to be sent in order most of the
// time. buffer should be in a mmap memory with properties: MAP_ANONYMOUS
// https://manpages.ubuntu.com/manpages/kinetic/en/man2/sendmsg.2.html
void IoUring::send(void* buffer, size_t length, uint64_t cookie) {}

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

bool IoUring::hasCompletedItem() const
{
    return false;
}

} // namespace iouring
