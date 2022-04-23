#include "RtcSocket.h"

#include "utils/StdExtensions.h"
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
namespace transport
{
void RtcSocket::Message::add(const void* data, size_t len)
{
    assert(static_cast<size_t>(fragmentCount) < std::size(fragments));
    fragments[fragmentCount].iov_base = const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(data));
    fragments[fragmentCount].iov_len = len;
    ++fragmentCount;
}

RtcSocket::RtcSocket() : _boundPort(SocketAddress::parse("0.0.0.0")), _fd(-1), _type(SOCK_DGRAM) {}

RtcSocket::RtcSocket(int fd, const SocketAddress& localPort) : _boundPort(localPort), _fd(fd), _type(SOCK_STREAM) {}

RtcSocket::~RtcSocket()
{
    close();
}

void RtcSocket::detachHandle()
{
    _fd = -1;
}

void RtcSocket::close()
{
    if (_fd != -1)
    {
        ::close(_fd);
        _fd = -1;
    }
}

int RtcSocket::open(const SocketAddress& address, uint16_t port, int socketType)
{
    close();
    _type = socketType;

    SocketAddress ip(address);
    ip.setPort(port);

    _fd = ::socket(ip.getFamily(), socketType, 0);
    if (_fd == -1)
    {
        return errno;
    }

    int flags = 0;
    if (-1 == (flags = fcntl(_fd, F_GETFL, 0)))
    {
        flags = 0;
    }
    if (socketType == SOCK_STREAM)
    {
        int val = 1;
        ::setsockopt(_fd, SOL_SOCKET, SO_REUSEPORT, &val, sizeof(val));
    }

    if (0 != fcntl(_fd, F_SETFL, flags | O_NONBLOCK))
    {
        return errno;
    }

    if (port != 0 && ::bind(_fd, ip.getSockAddr(), ip.getSockAddrSize()))
    {
        close();
        return errno;
    }
    _boundPort = ip;
    return 0;
}

int RtcSocket::accept(RtcSocket& serverSocket, SocketAddress& peerAddress)
{
    int rc = accept(serverSocket.fd(), peerAddress, _boundPort, _fd);
    if (rc != 0)
    {
        return rc;
    }

    return 0;
}

int RtcSocket::accept(int serverFd, SocketAddress& peerAddress, SocketAddress& localAddress, int& fd)
{
    RawSockAddress src_addr;
    socklen_t addrSize = sizeof(src_addr);

    fd = ::accept(serverFd, &src_addr.gen, &addrSize);
    if (fd < 0)
    {
        return errno;
    }

    peerAddress = SocketAddress(&src_addr.gen, peerAddress.getName().c_str());
    RawSockAddress local_addr;
    socklen_t localAddrSize = sizeof(local_addr);
    ::getsockname(fd, &local_addr.gen, &localAddrSize);
    localAddress = SocketAddress(&local_addr.gen, localAddress.getName().c_str());
    return 0;
}

// socket must be opened first and registered to epoll
int RtcSocket::connect(const SocketAddress& remotePort)
{
    if (!isGood())
    {
        return -1;
    }

    int rc = ::connect(_fd, remotePort.getSockAddr(), remotePort.getSockAddrSize());
    if (rc != 0)
    {
        int errorCode = errno;
        if (errorCode != EINPROGRESS)
        {
            return errorCode;
        }
    }

    rc = updateBoundPort();
    if (rc)
    {
        return rc;
    }
    return 0;
}

int RtcSocket::updateBoundPort()
{
    RawSockAddress local_addr;
    socklen_t addrSize = sizeof(local_addr);
    int rc = ::getsockname(_fd, &local_addr.gen, &addrSize);
    if (rc != 0)
    {
        return errno;
    }

    _boundPort = SocketAddress(&local_addr.gen, nullptr);
    return 0;
}

int RtcSocket::setSendBuffer(uint32_t size)
{
    if (0 != ::setsockopt(_fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)))
    {
        return errno;
    }
    return 0;
}

int RtcSocket::setReceiveBuffer(uint32_t size)
{
    if (0 != ::setsockopt(_fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)))
    {
        return errno;
    }
    return 0;
}

bool RtcSocket::isGood() const
{
    return _fd != -1;
}

// returns 0 on success else error code from errno
int RtcSocket::sendTo(const void* msg, size_t length, const SocketAddress& target)
{
    size_t bytesSent = 0;
    const struct iovec buffers[1] = {{const_cast<void*>(msg), length}};
    return sendAggregate(buffers, 1, bytesSent, target);
}

int RtcSocket::sendAggregate(const void* buf0,
    size_t length0,
    const void* buf1,
    size_t length1,
    size_t& bytesSent,
    const SocketAddress& target)
{
    const struct iovec buffers[2] = {{const_cast<void*>(buf0), length0}, {const_cast<void*>(buf1), length1}};
    return sendAggregate(buffers, 2, bytesSent, target);
}

int RtcSocket::sendAggregate(const void* buf0, size_t length0, size_t& bytesSent, const SocketAddress& target)
{
    const struct iovec buffers[1] = {{const_cast<void*>(buf0), length0}};
    return sendAggregate(buffers, 1, bytesSent, target);
}

namespace
{
size_t lengthOf(const msghdr& header)
{
    size_t result = 0;
    for (int i = 0; i < static_cast<int>(header.msg_iovlen); ++i)
    {
        result += header.msg_iov[i].iov_len;
    }
    return result;
}
} // namespace

int RtcSocket::sendAggregate(const struct iovec* messages,
    uint16_t messageCount,
    size_t& bytesSent,
    const SocketAddress& target)
{
    if (!isGood())
    {
        return ENOTSOCK;
    }

    const struct msghdr header = {
        target.empty() ? nullptr : const_cast<void*>(reinterpret_cast<const void*>(target.getSockAddr())),
        target.empty() ? 0 : static_cast<socklen_t>(target.getSockAddrSize()),
        const_cast<struct iovec*>(messages),
        messageCount,
        nullptr,
        0,
        0};

    const auto totalLength = static_cast<ssize_t>(lengthOf(header));
    if (totalLength == 0)
    {
        return 0;
    }

    int errorCode = EWOULDBLOCK;
    for (int i = 0; i < 2 && (errorCode == EAGAIN || errorCode == EWOULDBLOCK); ++i)
    {
        ssize_t rc = ::sendmsg(_fd, &header, MSG_DONTWAIT);
        if (rc >= 0)
        {
            bytesSent = rc;
            return (bytesSent == totalLength ? 0 : EAGAIN);
        }
        else
        {
            bytesSent = 0;
            errorCode = errno;
        }
    }

    return errorCode;
}

// returns errno or 0
int RtcSocket::sendMultiple(Message* messages, const size_t count)
{
    if (count == 0)
    {
        return 0;
    }
    assert(count <= 500);
    int errorCount = 0;
    const int maxSendAttempts = 2;
    int attemptsLeft = maxSendAttempts;

#ifdef __APPLE__
    for (size_t sendCursor = 0; sendCursor < count;)
    {
        msghdr singleMessage = {const_cast<sockaddr*>(messages[sendCursor].target->getSockAddr()),
            static_cast<socklen_t>(messages[sendCursor].target->getSockAddrSize()),
            const_cast<struct iovec*>(messages[sendCursor].fragments),
            messages[sendCursor].fragmentCount,
            nullptr,
            0,
            0};

        const auto totalLength = static_cast<ssize_t>(lengthOf(singleMessage));
        ssize_t rc = 0;
        rc = ::sendmsg(_fd, &singleMessage, MSG_DONTWAIT);
        if (rc != totalLength)
        {
            const int errorCode = errno;
            if (errorCode == EAGAIN && --attemptsLeft > 0)
            {
                continue;
            }
            else if (errorCode != EINPROGRESS)
            {
                ++errorCount;
                messages[sendCursor].errorCode = errorCode;
            }
        }
        attemptsLeft = maxSendAttempts;
        ++sendCursor;
    }

    return errorCount;

#else
    mmsghdr items[count];
    const auto addressSize = static_cast<socklen_t>(messages[0].target->getSockAddrSize());
    for (size_t i = 0; i < count; ++i)
    {
        msghdr& header = items[i].msg_hdr;
        header.msg_name = const_cast<sockaddr*>(messages[i].target->getSockAddr());
        header.msg_namelen = addressSize;
        header.msg_iov = const_cast<struct iovec*>(messages[i].fragments);
        header.msg_iovlen = messages[i].fragmentCount;
        header.msg_control = nullptr;
        header.msg_controllen = 0;
        header.msg_flags = 0;
        items[i].msg_len = 0;
    }

    for (size_t sendCursor = 0; sendCursor < count;)
    {
        const auto remainingCount = count - sendCursor;
        int rc = ::sendmmsg(_fd, items + sendCursor, remainingCount, MSG_DONTWAIT);
        if (rc == static_cast<int>(remainingCount))
        {
            return errorCount;
        }
        else if (rc < 0)
        {
            const int errorCode = errno;
            if (--attemptsLeft > 0 && (errorCode == EAGAIN || errorCode == EWOULDBLOCK))
            {
                continue;
            }
            else
            {
                messages[sendCursor].errorCode = errorCode;
                ++errorCount;
                ++sendCursor;
                attemptsLeft = maxSendAttempts;
            }
            continue;
        }

        sendCursor += std::max(1, rc);
        attemptsLeft = maxSendAttempts;
    }

#endif
    return errorCount;
}

int RtcSocket::listen(int backlog)
{
    return ::listen(_fd, backlog);
}

const char* RtcSocket::explain(int errorCode)
{
    return strerror(errorCode);
}

} // namespace transport
