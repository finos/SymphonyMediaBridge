#pragma once
#include "utils/SocketAddress.h"
#include <sys/socket.h>

namespace transport
{

class RtcSocket
{
public:
    struct Message
    {
        void add(const void* buffer, size_t len);
        size_t getLength() const
        {
            size_t length = 0;
            for (int i = 0; i < fragmentCount; ++i)
            {
                length += fragments[i].iov_len;
            }
            return length;
        }

        SocketAddress* target = nullptr;
        iovec fragments[5];
        int fragmentCount = 0; // int in msghdr
        int errorCode = 0;
    };

    RtcSocket();
    RtcSocket(int fd, const SocketAddress& localPort);

    ~RtcSocket();

    RtcSocket(const RtcSocket&) = delete;

    void detachHandle();
    void close();

    int open(const SocketAddress& address, uint16_t port, int socketType = SOCK_DGRAM);

    int listen(int backlog);
    int accept(RtcSocket& serverSocket, SocketAddress& peerAddress);
    static int accept(int serverFd, SocketAddress& peerAddress, SocketAddress& localAddress, int& fd);
    int connect(const SocketAddress& remotePort);

    bool isGood() const;
    int setSendBuffer(uint32_t size);
    int setReceiveBuffer(uint32_t size);

    int sendTo(const void* buffer, size_t length, const SocketAddress& target);
    int sendAggregate(const void* buf0,
        size_t length0,
        const void* buf1,
        size_t length1,
        const SocketAddress& target = SocketAddress());
    int sendAggregate(const void* buf0,
        size_t length0,
        const void* buf1,
        size_t length1,
        const void* buf2,
        size_t length2,
        const SocketAddress& target = SocketAddress());
    int sendMultiple(Message* messages, size_t count);

    SocketAddress getBoundPort() const { return _boundPort; }
    int fd() { return _fd; }

    int updateBoundPort();

    static const char* explain(int errorCode);

private:
    int sendAggregate(const struct iovec* messages, uint16_t messageCount, const SocketAddress& target);

    SocketAddress _boundPort;
    int _fd;
    int _type;
};

} // namespace transport
