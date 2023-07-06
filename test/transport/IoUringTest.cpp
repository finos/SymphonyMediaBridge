#include "transport/iouring/IoUring.h"
#include "logger/Logger.h"
#include "memory/MemMap.h"
#include "transport/RtcSocket.h"
#include "utils/Time.h"
#include <array>
#include <errno.h>
#include <gtest/gtest.h>

TEST(IoUringTest, Send)
{
    using namespace transport;
    RtcSocket socket;
    socket.open(SocketAddress::parse("172.18.190.101", 6767), 6767);

    iouring::IoUring ring;

    auto ringRc = ring.createForUdp(12);

    // assert(ringRc);
    memory::MemMap mem;
    auto memSize = memory::roundUpToPage(1500 * 2500);
    auto count = memSize / 1500;
    mem.allocate(memSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS);
    assert(mem.isGood());
    auto bufOK = ring.registerBuffers(mem.get<void*>(), count, 1500);
    assert(bufOK);
    std::string msg = "test message is simple but also complicated";

    std::memcpy(mem.get<char>(), msg.c_str(), msg.size());
    auto target = SocketAddress::parse("172.18.190.101", 6768);
    uint64_t cookie = 0;
    ring.send(socket.fd(), mem.get<void>(), msg.size(), target, cookie++);

    utils::Time::rawNanoSleep(4 * utils::Time::sec);
}
