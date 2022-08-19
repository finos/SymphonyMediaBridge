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

static const uint32_t outboundSsrc = 12345;
static const uint32_t outboundFeedbackSsrc = 67890;

void threadFunction(jobmanager::JobManager* jobManager)
{
    auto job = jobManager->wait();
    while (job)
    {
        job->run();
        jobManager->freeJob(job);
        job = jobManager->wait();
    }
}

} // namespace

class VideoNackReceiveJobTest : public ::testing::Test
{
    void SetUp() override
    {
        _jobManager = std::make_unique<jobmanager::JobManager>();
        _jobQueue = std::make_unique<jobmanager::JobQueue>(*_jobManager);
        _transport = std::make_unique<DummyRtcTransport>(*_jobQueue);

        _allocator = std::make_unique<memory::PacketPoolAllocator>(16, "VideoNackReceiveJobTest");
        _ssrcOutboundContext = std::make_unique<bridge::SsrcOutboundContext>(outboundSsrc,
            *_allocator,
            bridge::RtpMap(bridge::RtpMap::Format::VP8));

        _packetCache = std::make_unique<bridge::PacketCache>("VideoNackReceiveJobTest", outboundSsrc);
    }

    void TearDown() override
    {
        _ssrcOutboundContext.reset();

        auto thread = std::make_unique<std::thread>(threadFunction, _jobManager.get());
        _jobQueue.reset();
        _jobManager->stop();
        thread->join();
        _jobManager.reset();
    }

protected:
    std::unique_ptr<jobmanager::JobManager> _jobManager;
    std::unique_ptr<jobmanager::JobQueue> _jobQueue;
    std::unique_ptr<DummyRtcTransport> _transport;

    std::unique_ptr<memory::PacketPoolAllocator> _allocator;
    std::unique_ptr<bridge::SsrcOutboundContext> _ssrcOutboundContext;
    std::unique_ptr<bridge::PacketCache> _packetCache;
};

TEST_F(VideoNackReceiveJobTest, nacksNotAlreadyRespondedToAreHandled)
{
    uint16_t pid = 1;
    uint16_t blp = 3;
    uint64_t timestamp = 1000;
    const uint64_t rtt = 100 * utils::Time::ms;

    auto videoNackReceiveJob = std::make_unique<bridge::VideoNackReceiveJob>(*_ssrcOutboundContext,
        *_transport,
        *_packetCache,
        pid,
        blp,
        outboundFeedbackSsrc,
        timestamp,
        rtt);
    videoNackReceiveJob->run();

    EXPECT_EQ(timestamp, _ssrcOutboundContext->lastRespondedNackTimestamp);
    EXPECT_EQ(pid, _ssrcOutboundContext->lastRespondedNackPid);
    EXPECT_EQ(blp, _ssrcOutboundContext->lastRespondedNackBlp);

    pid = 5;
    blp = 7;
    timestamp = 1001;

    videoNackReceiveJob = std::make_unique<bridge::VideoNackReceiveJob>(*_ssrcOutboundContext,
        *_transport,
        *_packetCache,
        pid,
        blp,
        outboundFeedbackSsrc,
        timestamp,
        rtt);
    videoNackReceiveJob->run();

    EXPECT_EQ(timestamp, _ssrcOutboundContext->lastRespondedNackTimestamp);
    EXPECT_EQ(pid, _ssrcOutboundContext->lastRespondedNackPid);
    EXPECT_EQ(blp, _ssrcOutboundContext->lastRespondedNackBlp);
}

TEST_F(VideoNackReceiveJobTest, nacksAlreadyRespondedToWithinRttAreIgnored)
{
    const uint16_t pid = 1;
    const uint16_t blp = 3;
    const uint64_t timestamp = 1000;
    const uint64_t rtt = 100 * utils::Time::ms;

    auto videoNackReceiveJob = std::make_unique<bridge::VideoNackReceiveJob>(*_ssrcOutboundContext,
        *_transport,
        *_packetCache,
        pid,
        blp,
        outboundFeedbackSsrc,
        timestamp,
        rtt);
    videoNackReceiveJob->run();

    videoNackReceiveJob = std::make_unique<bridge::VideoNackReceiveJob>(*_ssrcOutboundContext,
        *_transport,
        *_packetCache,
        pid,
        blp,
        outboundFeedbackSsrc,
        timestamp + rtt - utils::Time::ms,
        rtt);
    videoNackReceiveJob->run();

    EXPECT_EQ(timestamp, _ssrcOutboundContext->lastRespondedNackTimestamp);
}

TEST_F(VideoNackReceiveJobTest, nacksAlreadyRespondedToOutsideRttAreHandled)
{
    const uint16_t pid = 1;
    const uint16_t blp = 3;
    uint64_t timestamp = 1000;
    const uint64_t rtt = 100 * utils::Time::ms;

    auto videoNackReceiveJob = std::make_unique<bridge::VideoNackReceiveJob>(*_ssrcOutboundContext,
        *_transport,
        *_packetCache,
        pid,
        blp,
        outboundFeedbackSsrc,
        timestamp,
        rtt);
    videoNackReceiveJob->run();

    timestamp += rtt + utils::Time::ms;

    videoNackReceiveJob = std::make_unique<bridge::VideoNackReceiveJob>(*_ssrcOutboundContext,
        *_transport,
        *_packetCache,
        pid,
        blp,
        outboundFeedbackSsrc,
        timestamp,
        rtt);
    videoNackReceiveJob->run();

    EXPECT_EQ(timestamp, _ssrcOutboundContext->lastRespondedNackTimestamp);
}
