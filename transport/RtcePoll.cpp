#include "RtcePoll.h"
#include <cassert>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/event.h>
#else
#include <sys/epoll.h>
#endif
#include "concurrency/SafeQueue.h"
#include "concurrency/ThreadUtils.h"
#include "logger/Logger.h"
#include <ifaddrs.h>
#include <mutex>
#include <net/if.h>
#include <queue>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unordered_map>

namespace transport
{

class RtcePollImpl : public RtcePoll
{
public:
    RtcePollImpl();
    ~RtcePollImpl();

    void run() override;
    void stop() override;

    bool add(int fd, RtcePoll::IEventListener* listener) override;
    bool remove(int fd, RtcePoll::IEventListener* listener) override;
    bool isRunning() const override { return _running; }

private:
    int _kernel_fd;
#ifdef __APPLE__
    std::vector<struct kevent> _firedEvents;
#else
    std::vector<struct epoll_event> _firedEvents;
#endif

    bool listenSocketEvents(RtcePoll::IEventListener* listener, int fd);
    bool unlistenSocketEvents(int fd);
    void awaitSocketEvents(int64_t timeoutMs);

    struct SocketRegistration
    {
        SocketRegistration() : registration(true), listener(nullptr), fd(-1) {}

        SocketRegistration(bool registration, RtcePoll::IEventListener* listener, int fd)
            : registration(registration),
              listener(listener),
              fd(fd)
        {
        }

        bool registration;
        RtcePoll::IEventListener* listener;
        int fd;
    };

    concurrency::LockFullQueue<SocketRegistration, 2500> _pendingRegistrations;
    std::unordered_map<int, SocketRegistration> _monitoredSockets;

    std::atomic_bool _running;
    std::unique_ptr<std::thread> _networkThread;
};

RtcePollImpl::RtcePollImpl() : _kernel_fd(-1), _firedEvents(100), _running(false), _networkThread(nullptr)
{
    _monitoredSockets.reserve(1000);
#ifdef __APPLE__
    _kernel_fd = kqueue();
#else
    _kernel_fd = epoll_create1(0);
#endif
    if (_kernel_fd == -1)
    {
        printf("%d\n", errno);
        return; // failed to init
    }

    _running = true;
    _networkThread = std::make_unique<std::thread>([this]() { this->run(); });
}

RtcePollImpl::~RtcePollImpl()
{
    if (_running)
    {
        stop();
    }
    if (_kernel_fd != -1)
    {
        ::close(_kernel_fd);
    }
}

void RtcePollImpl::stop()
{
    _running = false;
    _networkThread->join();
}

bool RtcePollImpl::listenSocketEvents(RtcePoll::IEventListener* listener, int fd)
{
    if (fd == -1)
    {
        return true;
    }

    if (_monitoredSockets.find(fd) != _monitoredSockets.end())
    {
        unlistenSocketEvents(fd);
    }

    auto itpair = _monitoredSockets.emplace(std::make_pair(fd, SocketRegistration{true, listener, fd}));
    if (!itpair.second)
    {
        assert(false);
        return false;
    }

    uint32_t socketType = 0;
    uint32_t dataLen = sizeof(socketType);
    if (-1 == getsockopt(fd, SOL_SOCKET, SO_TYPE, &socketType, &dataLen))
    {
        return false;
    }
#ifdef __APPLE__
    struct kevent readEvent;

    EV_SET(&readEvent, fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, listener);
    if (0 != kevent(_kernel_fd, &readEvent, 1, nullptr, 0, nullptr))
    {
        return false;
    }
    if (socketType == SOCK_STREAM)
    {
        struct kevent writeEvent;
        EV_SET(&writeEvent, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, listener);
        if (0 != kevent(_kernel_fd, &writeEvent, 1, nullptr, 0, nullptr))
        {
            return false;
        }
    }
#else
    struct epoll_event ePollEvent;
    ePollEvent.events = EPOLLIN + EPOLLET + (socketType == SOCK_STREAM ? EPOLLHUP + EPOLLOUT : 0);
    ePollEvent.data.ptr = &itpair.first->second;

    if (epoll_ctl(_kernel_fd, EPOLL_CTL_ADD, fd, &ePollEvent))
    {
        return false;
    }
#endif
    if (_monitoredSockets.size() >= _firedEvents.size())
    {
        _firedEvents.resize(_monitoredSockets.size() + 50);
    }
    return true;
}

//    TODO. what do we do if we would encounter an error unregistering the epoll.
//    Potentially it means the entire event notification may seize to work,
//    or that specific handle is triggered and leads to nowhere.
bool RtcePollImpl::unlistenSocketEvents(int fd)
{
    if (fd == -1)
    {
        return true;
    }
    _monitoredSockets.erase(fd);

#ifdef __APPLE__
    uint32_t socketType = 0;
    uint32_t dataLen = sizeof(socketType);
    if (-1 == getsockopt(fd, SOL_SOCKET, SO_TYPE, &socketType, &dataLen))
    {
        return false;
    }

    // closing fd will remove it from kernel event queue automatically, also
    struct kevent readEvent;
    EV_SET(&readEvent, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    auto rc = kevent(_kernel_fd, &readEvent, 1, nullptr, 0, nullptr);
    int rc2 = 0;
    if (socketType == SOCK_STREAM)
    {
        struct kevent writeEvent;
        EV_SET(&writeEvent, fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        rc2 = kevent(_kernel_fd, &writeEvent, 1, nullptr, 0, nullptr);
    }
    return rc == 0 && rc2 == 0;
#else
    auto epoll_rc = epoll_ctl(_kernel_fd, EPOLL_CTL_DEL, fd, nullptr);
    return !epoll_rc;
#endif
}

// This thread is time critical.
void RtcePollImpl::run()
{
    concurrency::setThreadName("Rtce");

    while (_running)
    {
        if (_monitoredSockets.empty())
        {
            _pendingRegistrations.waitForItems(5000);
        }
        SocketRegistration message;
        while (_pendingRegistrations.pop(message))
        {
            if (message.registration)
            {
                if (listenSocketEvents(message.listener, message.fd))
                {
                    message.listener->onSocketPollStarted(message.fd);
                }
            }
            else
            {
                unlistenSocketEvents(message.fd);
                message.listener->onSocketPollStopped(message.fd);
            }
        }

        awaitSocketEvents(100);
    }
}

/*
    On Mac we could add a user event to wake it up when there are pending registrations.
    But it does not seem to be that easy on linux unless you create some in mem file and close it.
*/
void RtcePollImpl::awaitSocketEvents(int64_t timeoutMs)
{
#ifdef __APPLE__
    const struct timespec timeout
    {
        0, 1000000ll * timeoutMs
    };
    auto event_count = kevent(_kernel_fd, nullptr, 0, _firedEvents.data(), _firedEvents.size(), &timeout);
#else
    auto event_count = epoll_wait(_kernel_fd, _firedEvents.data(), _firedEvents.size(), timeoutMs);
#endif
    for (int i = 0; i < event_count; ++i)
    {
        auto& event = _firedEvents[i];
#ifdef __APPLE__
        if (event.filter == EVFILT_READ)
        {
            if ((event.flags & EV_EOF) == EV_EOF)
            {
                reinterpret_cast<IEventListener*>(event.udata)->onSocketShutdown(event.ident);
            }
            else
            {
                reinterpret_cast<IEventListener*>(event.udata)->onSocketReadable(event.ident);
            }
        }
        else if (event.filter == EVFILT_WRITE)
        {
            if ((event.flags & EV_EOF) == EV_EOF)
            {
                reinterpret_cast<IEventListener*>(event.udata)->onSocketShutdown(event.ident);
            }
            else
            {
                reinterpret_cast<IEventListener*>(event.udata)->onSocketWriteable(event.ident);
            }
        }
#else
        // on linux the hup event is signalled after readable if client calls recv on socket if at all
        auto socketRegistration = reinterpret_cast<const SocketRegistration*>(event.data.ptr);

        if ((event.events & EPOLLIN) == EPOLLIN)
        {
            socketRegistration->listener->onSocketReadable(socketRegistration->fd);
            if ((event.events & EPOLLHUP) == EPOLLHUP || (event.events & EPOLLRDHUP) == EPOLLRDHUP ||
                (event.events & EPOLLERR) == EPOLLERR)
            {
                socketRegistration->listener->onSocketShutdown(socketRegistration->fd);
            }
        }
        if ((event.events & EPOLLOUT) == EPOLLOUT)
        {
            if ((event.events & EPOLLHUP) == EPOLLHUP || (event.events & EPOLLRDHUP) == EPOLLRDHUP ||
                (event.events & EPOLLERR) == EPOLLERR)
            {
                // This signal typically happens if server side does not have port open when connecting.
                // Writing to socket handle in this situation will cause SIG PIPE crash.
                socketRegistration->listener->onSocketShutdown(socketRegistration->fd);
            }
            else
            {
                socketRegistration->listener->onSocketWriteable(socketRegistration->fd);
            }
        }

#endif
    }
    if (event_count < 0 && errno != EINTR)
    {
        logger::error("failed to wait for socket event. err: %d", "", errno);
    }
}

bool RtcePollImpl::add(int fd, RtcePoll::IEventListener* listener)
{
    return _pendingRegistrations.push(SocketRegistration{true, listener, fd});
}

// You must await the notifyClosed callback after this
bool RtcePollImpl::remove(int fd, RtcePoll::IEventListener* listener)
{
    return _pendingRegistrations.push(SocketRegistration{false, listener, fd});
}

std::unique_ptr<RtcePoll> createRtcePoll()
{
    return std::make_unique<RtcePollImpl>();
}

} // namespace transport