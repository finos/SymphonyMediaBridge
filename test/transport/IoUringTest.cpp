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

/**
This test has been run on Debian in GCE kernel 5.10
AWS EC2 Amazon Linux kernel  5.10.184-175.731.amzn2.x86_64
GCE  e2-standard-4  5.10.0-23-cloud-amd64
WSL2 Ubuntu 22.04 5.15.90.1-microsoft-standard-WSL2

It seems to be the case that performance is very similar for sendmmsg and iouring.
We use the same batch size to reduce the number of syscalls and atomic writes.
Still the iouring performs the same or worse.
The time slept when sqe is full sum up to 15ms out of a second so there is not much slack cpu for worker thread to use
either.

Sending is the heavier part of SFU in all multi-person conferences. It is of interest if there is gain to be made in
iouring receive but it would have very little impact on the SMB performance.

*/

TEST(IoUringTest, Send)
{
    using namespace transport;
    RtcSocket socket;

    std::string msg = "test message is simple but also complicated";

    auto target = SocketAddress::parse("127.0.0.1", 6768);

    socket.open(SocketAddress::parse("127.0.0.1", 6767), 6767);

    uint8_t rawData[1250];

    const int iterations = 500000;
    size_t pauses = 0;
    const int batchCount = 200;
    RtcSocket::Message messages[batchCount];
    for (int i = 0; i < batchCount; ++i)
    {
        auto& m = messages[i];
        m.add(rawData, 1250);
        m.target = &target;
    }

    iouring::IoUring::Message iouringMessages[batchCount];
    for (int i = 0; i < batchCount; ++i)
    {
        auto& m = iouringMessages[i];
        m.add(rawData, 1250);
        m.setTarget(target);
        m.error = 0;
        m.socketFd = socket.fd();
    }

    auto start = utils::Time::getAbsoluteTime();
    for (int i = 0; i < iterations;)
    {
        if (0 == socket.sendMultiple(messages, batchCount))
        {
            i += batchCount;
        }
        else
        {
            std::this_thread::yield();
        }
    }
    auto end = utils::Time::getAbsoluteTime();

    logger::info("plain sendto %" PRIu64 "us pauses %zu", "", utils::Time::diff(start, end) / utils::Time::us, pauses);

    iouring::IoUring ring;

    auto ringRc = ring.createForUdp(2048);
    if (!ringRc)
    {
        logger::error("could not init iouring", "");
        return;
    }

    assert(ringRc);

    pauses = 0;
    start = utils::Time::getAbsoluteTime();
    for (int i = 0; i < iterations;)
    {
        if (i % 50 == 0)
        {
            ring.processCompletedItems();
        }

        if (ring.sendBatch(iouringMessages, batchCount) == batchCount)
        {
            i += batchCount;
            // logger::info("sent batch", "iouring");
        }
        else
        {
            // logger::info("process cqe", "iouring");
            if (!ring.processCompletedItems())
            {
                ++pauses;
                utils::Time::rawNanoSleep(1 * utils::Time::us);
                // std::this_thread::yield();
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
