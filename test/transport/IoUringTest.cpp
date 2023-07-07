#include "transport/iouring/IoUring.h"
#include "logger/Logger.h"
#include "memory/MemMap.h"
#include "transport/RtcSocket.h"
#include "utils/Time.h"
#include <array>
#include <errno.h>
#include <gtest/gtest.h>
#include <linux/io_uring.h>
#include <thread>

struct PacketItem : concurrency::StackItem
{
    PacketItem(void* p) : packet(p) {}

    void* packet;
};

TEST(IoUringTest, Send)
{
    using namespace transport;
    RtcSocket socket;

    std::string msg = "test message is simple but also complicated";

    auto target = SocketAddress::parse("127.0.0.1", 6768);

    socket.open(SocketAddress::parse("127.0.0.1", 6767), 6767);

    const int iterations = 500000;
    size_t pauses = 0;
    auto start = utils::Time::getAbsoluteTime();
    for (int i = 0; i < iterations;)
    {
        if (0 == socket.sendTo(msg.c_str(), msg.size(), target))
        {
            ++i;
        }
        else
        {
            std::this_thread::yield();
        }
    }
    auto end = utils::Time::getAbsoluteTime();

    logger::info("plain sendto %" PRIu64 "us pauses %zu", "", utils::Time::diff(start, end) / utils::Time::us, pauses);

    iouring::IoUring ring;

    auto ringRc = ring.createForUdp(1024);

    assert(ringRc);

    pauses = 0;
    start = utils::Time::getAbsoluteTime();
    for (int i = 0; i < iterations;)
    {
        if (i % 500 == 0)
        {
            ring.processCompletedItems();
        }

        if (ring.send(socket.fd(), msg.c_str(), msg.size(), target, 0))
        {
            ++i;
        }
        else
        {
            if (!ring.processCompletedItems())
            {
                ++pauses;
                std::this_thread::yield();
            }
        }
    }
    end = utils::Time::getAbsoluteTime();
    while (ring.processCompletedItems())
    {
        utils::Time::rawNanoSleep(1 * utils::Time::us);
    }

    logger::info("ioring sendto %" PRIu64 "us wakeups %zu, pauses %zu",
        "",
        utils::Time::diff(start, end) / utils::Time::us,
        ring.getWakeUps(),
        pauses);

    utils::Time::rawNanoSleep(2 * utils::Time::sec);
    ring.processCompletedItems();
}
