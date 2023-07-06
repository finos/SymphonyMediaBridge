#pragma once
#include "memory/MemMap.h"
#include <concurrency/WaitFreeStack.h>
#include <cstddef>
#include <cstdint>

namespace transport
{
class SocketAddress;
}
namespace iouring
{

struct CompletionInfo
{
    enum Operation
    {
        READ = 0,
        WRITE
    };

    Operation operation;
    uint64_t cookie;
    void* buffer;
    size_t length;
    int error = 0;
};

// threading is a potential issue. The CQE and SQE rings are not made for wait free operations.
// We use the same SQE for read and write, so read and write operations must be issued from same thread.
// Once we have event from socket there is something to read we must issue a read operation for that socket. The
// operation is then async. We must test if a single thread on IoUring is enough to support desired packet rates.
// Perhaps we need different IoUrings per socket but that may cause multiple kernel threads.
class IoUring
{
public:
    IoUring();
    ~IoUring();

    bool createForUdp(size_t queueDepth);

    void send(int fd, const void* buffer, size_t length, const transport::SocketAddress& target, uint64_t cookie);
    void receive(void* buffer, size_t length, uint64_t cookie);

    bool registerCompletionEvent(int eventFd);
    bool unRegisterCompletionEvent(int eventFd);

    bool getCompletedItem(CompletionInfo& item);
    bool hasCompletedItems() const;

    bool registerBuffers(void* buffer, size_t count, size_t itemSize, size_t itemPadding = 0);

private:
    struct IoSubmitRing;
    struct IoCompletionRing;

    int _ringFd;
    IoSubmitRing* _submitRing;
    IoCompletionRing* _completionRing;

    memory::MemMap _submitQueueMemory;
    memory::MemMap _completionQueueMemory;
    memory::MemMap _submitItems;
    memory::MemMap _messageHeadersMemory;

    concurrency::WaitFreeStack _messageHeaders;
    concurrency::WaitFreeStack _sqeItems;
};
} // namespace iouring
