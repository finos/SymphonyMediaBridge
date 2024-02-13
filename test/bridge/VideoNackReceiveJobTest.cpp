#include "bridge/engine/VideoNackReceiveJob.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "jobmanager/JobManager.h"
#include "memory/PacketPoolAllocator.h"
#include "test/bridge/DummyRtcTransport.h"
#include <gtest/gtest.h>
#include <memory>
#include <thread>

namespace
{

static const uint32_t mediaSsrc = 12345;
static const uint32_t rtxSsrc = 54321;

const bridge::RtpMap VP8_RTP_MAP(bridge::RtpMap::Format::VP8);
const bridge::RtpMap RTX_RTP_MAP(bridge::RtpMap::Format::VP8);

} // namespace

class VideoNackReceiveJobTest : public ::testing::Test
{
    void SetUp() override
    {
        _timers = std::make_unique<jobmanager::TimerQueue>(4096 * 8);
        _jobManager = std::make_unique<jobmanager::JobManager>(*_timers);
        _jobQueue = std::make_unique<jobmanager::JobQueue>(*_jobManager);
        _transport = std::make_unique<DummyRtcTransport>(*_jobQueue);

        _allocator = std::make_unique<memory::PacketPoolAllocator>(16, "VideoNackReceiveJobTest");
        _mainOutboundContext =
            std::make_unique<bridge::SsrcOutboundContext>(mediaSsrc, *_allocator, VP8_RTP_MAP, bridge::RtpMap::EMPTY);

        _rtxOutboundContext =
            std::make_unique<bridge::SsrcOutboundContext>(rtxSsrc, *_allocator, RTX_RTP_MAP, bridge::RtpMap::EMPTY);

        _packetCache = std::make_unique<bridge::PacketCache>("VideoNackReceiveJobTest", mediaSsrc);
        _mainOutboundContext->packetCache.set(_packetCache.get());
    }

    void TearDown() override
    {
        _mainOutboundContext.reset();
        _rtxOutboundContext.reset();

        auto thread = std::make_unique<jobmanager::WorkerThread>(*_jobManager, true);
        _jobQueue.reset();
        _timers->stop();
        _jobManager->stop();
        thread->stop();
        _jobManager.reset();
    }

protected:
    std::unique_ptr<jobmanager::TimerQueue> _timers;
    std::unique_ptr<jobmanager::JobManager> _jobManager;
    std::unique_ptr<jobmanager::JobQueue> _jobQueue;
    std::unique_ptr<DummyRtcTransport> _transport;

    std::unique_ptr<memory::PacketPoolAllocator> _allocator;
    std::unique_ptr<bridge::SsrcOutboundContext> _mainOutboundContext;
    std::unique_ptr<bridge::SsrcOutboundContext> _rtxOutboundContext;
    std::unique_ptr<bridge::PacketCache> _packetCache;
};

TEST_F(VideoNackReceiveJobTest, nacksNotAlreadyRespondedToAreHandled)
{
    uint16_t pid = 1;
    uint16_t blp = 3;
    uint64_t timestamp = 1000;
    const uint64_t rtt = 100 * utils::Time::ms;

    auto videoNackReceiveJob = std::make_unique<bridge::VideoNackReceiveJob>(*_rtxOutboundContext,
        *_transport,
        *_mainOutboundContext,
        pid,
        blp,
        timestamp,
        rtt);
    videoNackReceiveJob->run();

    EXPECT_EQ(timestamp, _rtxOutboundContext->lastRespondedNackTimestamp);
    EXPECT_EQ(pid, _rtxOutboundContext->lastRespondedNackPid);
    EXPECT_EQ(blp, _rtxOutboundContext->lastRespondedNackBlp);

    pid = 5;
    blp = 7;
    timestamp = 1001;

    videoNackReceiveJob = std::make_unique<bridge::VideoNackReceiveJob>(*_rtxOutboundContext,
        *_transport,
        *_mainOutboundContext,
        pid,
        blp,
        timestamp,
        rtt);
    videoNackReceiveJob->run();

    EXPECT_EQ(timestamp, _rtxOutboundContext->lastRespondedNackTimestamp);
    EXPECT_EQ(pid, _rtxOutboundContext->lastRespondedNackPid);
    EXPECT_EQ(blp, _rtxOutboundContext->lastRespondedNackBlp);
}

TEST_F(VideoNackReceiveJobTest, nacksAlreadyRespondedToWithinRttAreIgnored)
{
    const uint16_t pid = 1;
    const uint16_t blp = 3;
    const uint64_t timestamp = 1000;
    const uint64_t rtt = 100 * utils::Time::ms;

    auto videoNackReceiveJob = std::make_unique<bridge::VideoNackReceiveJob>(*_rtxOutboundContext,
        *_transport,
        *_mainOutboundContext,
        pid,
        blp,
        timestamp,
        rtt);
    videoNackReceiveJob->run();

    videoNackReceiveJob = std::make_unique<bridge::VideoNackReceiveJob>(*_rtxOutboundContext,
        *_transport,
        *_mainOutboundContext,
        pid,
        blp,
        timestamp + rtt - utils::Time::ms,
        rtt);
    videoNackReceiveJob->run();

    EXPECT_EQ(timestamp, _rtxOutboundContext->lastRespondedNackTimestamp);
}

TEST_F(VideoNackReceiveJobTest, nacksAlreadyRespondedToOutsideRttAreHandled)
{
    const uint16_t pid = 1;
    const uint16_t blp = 3;
    uint64_t timestamp = 1000;
    const uint64_t rtt = 100 * utils::Time::ms;

    auto videoNackReceiveJob = std::make_unique<bridge::VideoNackReceiveJob>(*_rtxOutboundContext,
        *_transport,
        *_mainOutboundContext,
        pid,
        blp,
        timestamp,
        rtt);
    videoNackReceiveJob->run();

    timestamp += rtt + utils::Time::ms;

    videoNackReceiveJob = std::make_unique<bridge::VideoNackReceiveJob>(*_rtxOutboundContext,
        *_transport,
        *_mainOutboundContext,
        pid,
        blp,
        timestamp,
        rtt);
    videoNackReceiveJob->run();

    EXPECT_EQ(timestamp, _rtxOutboundContext->lastRespondedNackTimestamp);
}
