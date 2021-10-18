#pragma once
#include <memory>
namespace transport
{

// socket event listener for linux and BSD. Linux works differently in remote shutdown.
// It is required that you recv from the socket when the readable event fires in order
// for the shutdown event to fire.
class RtcePoll
{
public:
    // Do not perform any blocking operations in these methods
    class IEventListener
    {
    public:
        virtual void onSocketPollStarted(int fd) = 0;
        virtual void onSocketPollStopped(int fd) = 0;
        virtual void onSocketReadable(int fd) = 0;
        virtual void onSocketWriteable(int fd) = 0;
        virtual void onSocketShutdown(int fd) = 0;
    };
    virtual ~RtcePoll() = default;

    virtual void run() = 0;
    virtual void stop() = 0;

    virtual bool add(int fd, IEventListener* listener) = 0;
    virtual bool remove(int fd, IEventListener* listener) = 0;

    virtual bool isRunning() const = 0;
};

std::unique_ptr<RtcePoll> createRtcePoll();

} // namespace transport