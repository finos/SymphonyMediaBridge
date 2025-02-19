#include "bridge/Mixer.h"
#include "bridge/AudioStream.h"
#include "bridge/DataStream.h"
#include "bridge/RtpMap.h"
#include "bridge/VideoStream.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineDataStream.h"
#include "bridge/engine/EngineRecordingStream.h"
#include "bridge/engine/EngineVideoStream.h"
#include "concurrency/SynchronizationContext.h"
#include "config/Config.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "memory/PacketPoolAllocator.h"
#include "mocks/MixerManagerAsyncMock.h"
#include "mocks/RtcTransportMock.h"
#include "mocks/TransportFactoryMock.h"
#include "utils/IdGenerator.h"
#include "utils/SsrcGenerator.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>

using namespace ::testing;
using namespace ::test;
using namespace ::bridge;

namespace
{

constexpr const uint32_t LAST_N = 9;

constexpr const uint32_t SCTP_PORT = 5000;
constexpr const uint32_t LOCAL_VIDEO_SRC = 887766;
constexpr const uint32_t REWRITE_AUDIO_SRC_0 = 220000;
constexpr const uint32_t REWRITE_AUDIO_SRC_1 = 220002;
constexpr const uint32_t REWRITE_AUDIO_SRC_2 = 220004;
constexpr const uint32_t REWRITE_AUDIO_SRC_3 = 220006;
constexpr const uint32_t REWRITE_AUDIO_SRC_4 = 220008;

const bridge::RtpMap AUDIO_RTP_MAP = bridge::RtpMap(bridge::RtpMap::Format::OPUS);
const bridge::RtpMap TELEPHONE_EVENT_RTP_MAP = bridge::RtpMap(bridge::RtpMap::Format::TELEPHONE_EVENT);
const bridge::RtpMap VIDEO_RTP_MAP = bridge::RtpMap(bridge::RtpMap::Format::VP8);
const bridge::RtpMap FEEDBACK_RTP_MAP = bridge::RtpMap(bridge::RtpMap::Format::RTX);

// FAIL()

#define CHECK(EXPRESSION)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(EXPRESSION))                                                                                             \
        {                                                                                                              \
            FAIL() << (std::string()                                                                                   \
                    .append("Line ")                                                                                   \
                    .append(std::to_string(__LINE__))                                                                  \
                    .append(": expression has failed!. Expression: '")                                                 \
                    .append(#EXPRESSION)                                                                               \
                    .append("'"));                                                                                     \
        }                                                                                                              \
    } while (0)

std::vector<uint32_t> getAudioSsrcs()
{
    return {REWRITE_AUDIO_SRC_0, REWRITE_AUDIO_SRC_1, REWRITE_AUDIO_SRC_2, REWRITE_AUDIO_SRC_3, REWRITE_AUDIO_SRC_4};
}

std::vector<api::SimulcastGroup> getVideoSsrcs(utils::SsrcGenerator& ssrcGenerator, uint32_t lastN)
{
    // Generate randomly for now
    std::vector<api::SimulcastGroup> simulcastGroups;
    for (uint32_t i = 0; i < lastN + 3; ++i)
    {
        simulcastGroups.emplace_back();
        for (uint32_t layer = 0; layer < 3; ++layer)
        {
            simulcastGroups.back().push_back({ssrcGenerator.next(), ssrcGenerator.next()});
        }
    }

    return simulcastGroups;
}

bridge::SimulcastStream emptySimulcast()
{
    bridge::SimulcastStream simulcast;
    simulcast.numLevels = 0;
    simulcast.highestActiveLevel = 0;
    simulcast.contentType = bridge::SimulcastStream::VideoContentType::VIDEO;
    return simulcast;
}

struct JobManagerProcessor
{
    JobManagerProcessor(jobmanager::TimerQueue& timeQueue) : jobManager(timeQueue, 512) {}
    ~JobManagerProcessor() { dropAll(); }

    size_t runPendingJobs()
    {
        size_t processed = 0;
        for (auto pendingJobIt = pendingJobs.begin(); pendingJobIt != pendingJobs.end();)
        {
            ++processed;
            if (!(*pendingJobIt)->runStep())
            {
                jobManager.freeJob(*pendingJobIt);
                pendingJobIt = pendingJobs.erase(pendingJobIt);
            }
            else
            {
                ++pendingJobIt;
            }
        }

        return processed;
    }

    size_t processAll()
    {
        size_t processed = runPendingJobs();

        jobmanager::MultiStepJob* job;
        while ((job = jobManager.pop()) != nullptr)
        {
            ++processed;
            if (!job->runStep())
            {
                jobManager.freeJob(job);
            }
            else
            {
                pendingJobs.push_back(job);
            }
        }

        processed += runPendingJobs();
        return processed;
    }

    void dropAll()
    {
        jobmanager::MultiStepJob* job;
        while ((job = jobManager.pop()) != nullptr)
        {
            jobManager.freeJob(job);
        }

        while (!pendingJobs.empty())
        {
            jobManager.freeJob(pendingJobs.back());
            pendingJobs.pop_back();
        }
    }

    jobmanager::JobManager& getJobManager() { return jobManager; }

private:
    jobmanager::JobManager jobManager;
    std::vector<jobmanager::MultiStepJob*> pendingJobs;
};

struct MixerTestScope
{
    MixerTestScope()
        : transportFactoryMock(),
          engineTaskQueue(512),
          engineSyncContext(engineTaskQueue),
          timeQueue(64),
          wtJobManagerProcessor(timeQueue),
          backgroundJobManagerProcessor(timeQueue),
          packetAllocator(4096, "MixerTestPoolAllocator"),
          audioPacketAllocator(4096, "MixerTestAudioPoolAllocator")
    {
    }

    NiceMock<TransportFactoryMock> transportFactoryMock;
    StrictMock<MixerManagerAsyncMock> mixerManagerAsyncMock;
    concurrency::MpmcQueue<utils::Function> engineTaskQueue;
    concurrency::SynchronizationContext engineSyncContext;
    jobmanager::TimerQueue timeQueue;
    JobManagerProcessor wtJobManagerProcessor;
    JobManagerProcessor backgroundJobManagerProcessor;
    memory::PacketPoolAllocator packetAllocator;
    memory::AudioPacketPoolAllocator audioPacketAllocator;
};

} // namespace

class MixerTest : public ::testing::Test
{
public:
    using ::testing::Test::Test;

protected:
    void SetUp() override { _testScope = std::make_unique<MixerTestScope>(); }

    void TearDown() override { _testScope.reset(); }

    std::unique_ptr<Mixer> createMixer(const std::string& id, bool useGlobalPort)
    {
        auto audioSsrc = getAudioSsrcs();
        auto videoSsrcs = getVideoSsrcs(_ssrcGenerator, LAST_N);
        std::vector<api::SsrcPair> videoPinSsrc;
        auto engineMixer = std::make_unique<EngineMixer>(id,
            _testScope->wtJobManagerProcessor.getJobManager(),
            _testScope->engineSyncContext,
            _testScope->backgroundJobManagerProcessor.getJobManager(),
            _testScope->mixerManagerAsyncMock,
            LOCAL_VIDEO_SRC,
            _config,
            _testScope->packetAllocator,
            _testScope->audioPacketAllocator,
            _testScope->packetAllocator,
            audioSsrc,
            videoSsrcs,
            LAST_N);

        return std::make_unique<Mixer>(id,
            1,
            _testScope->transportFactoryMock,
            _testScope->backgroundJobManagerProcessor.getJobManager(),
            std::move(engineMixer),
            _idGenerator,
            _ssrcGenerator,
            _config,
            audioSsrc,
            videoSsrcs,
            videoPinSsrc,
            VideoCodecSpec::makeVp8(),
            useGlobalPort);
    }

    void addEndpointWithBundleTransport(Mixer& mixer,
        const std::string& endpointId,
        bool useAudio,
        bool useVideo,
        bool useData)
    {
        std::string outId;
        CHECK(mixer.addBundleTransportIfNeeded(endpointId, ice::IceRole::CONTROLLING, true));
        CHECK(mixer.addBundledAudioStream(outId, endpointId, MediaMode::SSRC_REWRITE));
        CHECK(mixer.addBundledVideoStream(outId, endpointId, true));
        CHECK(mixer.addBundledDataStream(outId, endpointId));
        CHECK(mixer.configureAudioStream(endpointId,
            AUDIO_RTP_MAP,
            TELEPHONE_EVENT_RTP_MAP,
            utils::Optional<uint32_t>(),
            {}));

        CHECK(mixer.configureVideoStream(endpointId,
            VIDEO_RTP_MAP,
            FEEDBACK_RTP_MAP,
            emptySimulcast(),
            utils::Optional<SimulcastStream>(),
            bridge::SsrcWhitelist()));

        CHECK(mixer.configureDataStream(endpointId, SCTP_PORT));

        CHECK(mixer.addAudioStreamToEngine(endpointId));
        CHECK(mixer.addVideoStreamToEngine(endpointId));
        CHECK(mixer.addDataStreamToEngine(endpointId));
        CHECK(mixer.startBundleTransport(endpointId));
    }

    void dropAllBackgroundJobs() { _testScope->backgroundJobManagerProcessor.dropAll(); }
    void dropAllWorkerThreadJobs() { _testScope->wtJobManagerProcessor.dropAll(); }

    void processAllEngineQueue()
    {
        utils::Function func;
        while (_testScope->engineTaskQueue.pop(func))
        {
            func();
        }
    }

protected:
    config::Config _config;
    utils::IdGenerator _idGenerator;
    utils::SsrcGenerator _ssrcGenerator;
    std::unique_ptr<MixerTestScope> _testScope;
};

TEST_F(MixerTest, bundleTransportShouldNotBeFinalizedWhenUsedOnAudioStream)
{
    const std::string endpointId0 = "ENDPOINT-0";
    auto mixer = createMixer("MixerTest0", true);

    auto transportMock = std::make_shared<NiceMock<RtcTransportMock>>();

    EXPECT_CALL(*transportMock, stop()).Times(0);

    _testScope->transportFactoryMock.willReturnByDefaultForAllWeakly(transportMock);

    addEndpointWithBundleTransport(*mixer, endpointId0, true, true, true);

    auto* engineVideoStream = mixer->getEngineVideoStream(endpointId0);
    auto* engineDataStream = mixer->getEngineDataStream(endpointId0);

    ASSERT_NE(nullptr, engineVideoStream);
    ASSERT_NE(nullptr, engineDataStream);

    dropAllBackgroundJobs();

    std::weak_ptr<RtcTransportMock> transportMockWeakPointer = transportMock;
    transportMock.reset(); // Allow to be deleted if a strong reference is lost in mixer

    mixer->removeVideoStream(endpointId0);
    mixer->removeDataStream(endpointId0);

    mixer->engineVideoStreamRemoved(*engineVideoStream);
    mixer->engineDataStreamRemoved(*engineDataStream);

    const size_t processedJobs = _testScope->backgroundJobManagerProcessor.processAll();
    ASSERT_EQ(0, processedJobs);
    ASSERT_NE(nullptr, transportMockWeakPointer.lock());
}

TEST_F(MixerTest, bundleTransportShouldNotBeFinalizedWhenUsedOnVideoStream)
{
    const std::string endpointId0 = "ENDPOINT-0";
    auto mixer = createMixer("MixerTest0", true);

    auto transportMock = std::make_shared<NiceMock<RtcTransportMock>>();

    EXPECT_CALL(*transportMock, stop()).Times(0);

    _testScope->transportFactoryMock.willReturnByDefaultForAllWeakly(transportMock);

    addEndpointWithBundleTransport(*mixer, endpointId0, true, true, true);

    auto* engineAudioStream = mixer->getEngineAudioStream(endpointId0);
    auto* engineDataStream = mixer->getEngineDataStream(endpointId0);

    ASSERT_NE(nullptr, engineAudioStream);
    ASSERT_NE(nullptr, engineDataStream);

    dropAllBackgroundJobs();

    std::weak_ptr<RtcTransportMock> transportMockWeakPointer = transportMock;
    transportMock.reset(); // Allow to be deleted if a strong reference is lost in mixer

    mixer->removeAudioStream(endpointId0);
    mixer->removeDataStream(endpointId0);

    mixer->engineAudioStreamRemoved(*engineAudioStream);
    mixer->engineDataStreamRemoved(*engineDataStream);

    const size_t processedJobs = _testScope->backgroundJobManagerProcessor.processAll();
    ASSERT_EQ(0, processedJobs);
    ASSERT_NE(nullptr, transportMockWeakPointer.lock());
}

TEST_F(MixerTest, bundleTransportShouldNotBeFinalizedWhenUsedOnDataStream)
{
    const std::string endpointId0 = "ENDPOINT-0";
    auto mixer = createMixer("MixerTest0", true);

    auto transportMock = std::make_shared<NiceMock<RtcTransportMock>>();

    EXPECT_CALL(*transportMock, stop()).Times(0);

    _testScope->transportFactoryMock.willReturnByDefaultForAllWeakly(transportMock);

    addEndpointWithBundleTransport(*mixer, endpointId0, true, true, true);

    auto* engineAudioStream = mixer->getEngineAudioStream(endpointId0);
    auto* engineVideoStream = mixer->getEngineVideoStream(endpointId0);

    ASSERT_NE(nullptr, engineAudioStream);
    ASSERT_NE(nullptr, engineVideoStream);

    dropAllBackgroundJobs();

    std::weak_ptr<RtcTransportMock> transportMockWeakPointer = transportMock;
    transportMock.reset(); // Allow to be deleted if a strong reference is lost in mixer

    mixer->removeAudioStream(endpointId0);
    mixer->removeVideoStream(endpointId0);

    mixer->engineAudioStreamRemoved(*engineAudioStream);
    mixer->engineVideoStreamRemoved(*engineVideoStream);

    const size_t processedJobs = _testScope->backgroundJobManagerProcessor.processAll();
    ASSERT_EQ(0, processedJobs);
    ASSERT_NE(nullptr, transportMockWeakPointer.lock());
}

TEST_F(MixerTest, bundleTransportShouldDeleteBundleTransport)
{
    const std::string endpointId0 = "ENDPOINT-0";
    auto mixer = createMixer("MixerTest0", true);

    auto transportMock = std::make_shared<NiceMock<RtcTransportMock>>();

    EXPECT_CALL(*transportMock, hasPendingJobs()).WillOnce(Return(false));
    EXPECT_CALL(*transportMock, stop());

    _testScope->transportFactoryMock.willReturnByDefaultForAllWeakly(transportMock);

    addEndpointWithBundleTransport(*mixer, endpointId0, true, true, true);

    auto* engineAudioStream = mixer->getEngineAudioStream(endpointId0);
    auto* engineVideoStream = mixer->getEngineVideoStream(endpointId0);
    auto* engineDataStream = mixer->getEngineDataStream(endpointId0);

    ASSERT_NE(nullptr, engineAudioStream);
    ASSERT_NE(nullptr, engineVideoStream);
    ASSERT_NE(nullptr, engineDataStream);

    std::weak_ptr<RtcTransportMock> transportMockWeakPointer = transportMock;
    transportMock.reset(); // Allow to be deleted if a strong reference is lost in mixer

    mixer->removeAudioStream(endpointId0);
    mixer->removeVideoStream(endpointId0);
    mixer->removeDataStream(endpointId0);

    // Remove should not really delete but trigger the deletion on EngineMixer
    size_t processedJobs = _testScope->backgroundJobManagerProcessor.processAll();
    ASSERT_EQ(0, processedJobs);
    ASSERT_NE(nullptr, transportMockWeakPointer.lock());
    ASSERT_NE(nullptr, mixer->getEngineAudioStream(endpointId0));
    ASSERT_NE(nullptr, mixer->getEngineVideoStream(endpointId0));
    ASSERT_NE(nullptr, mixer->getEngineDataStream(endpointId0));

    dropAllBackgroundJobs();

    mixer->engineAudioStreamRemoved(*engineAudioStream);
    mixer->engineVideoStreamRemoved(*engineVideoStream);
    mixer->engineDataStreamRemoved(*engineDataStream);

    ASSERT_NE(nullptr, transportMockWeakPointer.lock());

    processedJobs = _testScope->backgroundJobManagerProcessor.processAll();
    ASSERT_EQ(1, processedJobs);
    ASSERT_EQ(nullptr, transportMockWeakPointer.lock());
    ASSERT_EQ(nullptr, mixer->getEngineAudioStream(endpointId0));
    ASSERT_EQ(nullptr, mixer->getEngineVideoStream(endpointId0));
    ASSERT_EQ(nullptr, mixer->getEngineDataStream(endpointId0));
}
