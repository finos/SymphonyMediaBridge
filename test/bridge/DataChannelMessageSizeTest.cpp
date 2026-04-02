#include "api/DataChannelMessage.h"
#include "api/DataChannelMessageParser.h"
#include "bridge/AudioStream.h"
#include "bridge/DataStream.h"
#include "bridge/Mixer.h"
#include "bridge/VideoStream.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineBarbell.h"
#include "bridge/engine/EngineDataStream.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/EngineRecordingStream.h"
#include "bridge/engine/EngineVideoStream.h"
#include "config/Config.h"
#include "jobmanager/JobManager.h"
#include "jobmanager/TimerQueue.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "memory/PoolBuffer.h"
#include "memory/PacketPoolAllocator.h"
#include "mocks/MixerManagerAsyncMock.h"
#include "mocks/RtcTransportMock.h"
#include "mocks/TransportFactoryMock.h"
#include "utils/Function.h"
#include "utils/IdGenerator.h"
#include "utils/Optional.h"
#include "utils/SsrcGenerator.h"
#include "utils/StdExtensions.h"
#include "utils/SimpleJson.h"
#include "webrtc/DataChannel.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>

#include "bridge/MixerManager.h"
#include "bridge/engine/Engine.h"

using namespace ::testing;
using namespace ::test;
using namespace ::bridge;

namespace
{
struct JobManagerProcessor
{
    JobManagerProcessor(jobmanager::TimerQueue& timeQueue) : jobManager(timeQueue, 512) {}
    ~JobManagerProcessor() { dropAll(); }

    void process()
    {
        jobmanager::MultiStepJob* job;
        while ((job = jobManager.pop()) != nullptr)
        {
            if (!job->runStep())
            {
                jobManager.freeJob(job);
            }
            else
            {
                pendingJobs.push_back(job);
            }
        }
    }

    void dropAll()
    {
        jobmanager::MultiStepJob* job;
        while ((job = jobManager.pop()) != nullptr)
        {
            jobManager.freeJob(job);
        }

        for (auto* pendingJob : pendingJobs)
        {
            jobManager.freeJob(pendingJob);
        }
        pendingJobs.clear();
    }

    jobmanager::JobManager& getJobManager() { return jobManager; }

private:
    jobmanager::JobManager jobManager;
    std::vector<jobmanager::MultiStepJob*> pendingJobs;
};

struct MixerTestScope
{
    MixerTestScope()
        : transportFactoryMock(std::make_shared<NiceMock<TransportFactoryMock>>()),
          engineTaskQueue(512),
          engineSyncContext(engineTaskQueue),
          timeQueue(64),
          jobManagerProcessor(timeQueue),
          backgroundJobManagerProcessor(timeQueue),
          packetAllocator(4096, "MixerTestPoolAllocator"),
          audioPacketAllocator(4096, "MixerTestAudioPoolAllocator")
    {
    }

    std::shared_ptr<NiceMock<TransportFactoryMock>> transportFactoryMock;
    StrictMock<MixerManagerAsyncMock> mixerManagerAsyncMock;
    concurrency::MpmcQueue<utils::Function> engineTaskQueue;
    concurrency::SynchronizationContext engineSyncContext;
    jobmanager::TimerQueue timeQueue;
    JobManagerProcessor jobManagerProcessor;
    JobManagerProcessor backgroundJobManagerProcessor;
    memory::PacketPoolAllocator packetAllocator;
    memory::AudioPacketPoolAllocator audioPacketAllocator;
};
} // namespace

class DataChannelMessageSizeTest : public ::testing::Test
{
public:
    DataChannelMessageSizeTest() : _testScope(std::make_unique<MixerTestScope>()) {}

protected:
    void SetUp() override
    {
        _config.sctp.maxMessageSize = 4096;
        std::vector<uint32_t> audioSsrcs = {1, 2, 3};
        std::vector<api::SimulcastGroup> videoSsrcs;
        std::vector<api::SsrcPair> videoPinSsrcs;

        auto engineMixer = std::make_unique<EngineMixer>("test-mixer",
            _testScope->jobManagerProcessor.getJobManager(),
            _testScope->engineSyncContext,
            _testScope->backgroundJobManagerProcessor.getJobManager(),
            _testScope->mixerManagerAsyncMock,
            0,
            _config,
            _testScope->packetAllocator,
            _testScope->audioPacketAllocator,
            _testScope->packetAllocator,
            audioSsrcs,
            videoSsrcs,
            0);

        _mixer = std::make_unique<Mixer>("test-mixer",
            1,
            *_testScope->transportFactoryMock,
            _testScope->backgroundJobManagerProcessor.getJobManager(),
            std::move(engineMixer),
            _idGenerator,
            _ssrcGenerator,
            _config,
            audioSsrcs,
            videoSsrcs,
            videoPinSsrcs,
            VideoCodecSpec::makeVp8(),
            true);
    }

    void TearDown() override
    {
        utils::Function func;
        while (_testScope->engineTaskQueue.pop(func))
        {
        }
        _testScope->jobManagerProcessor.dropAll();
        _testScope->backgroundJobManagerProcessor.dropAll();

        _mixer.reset();
        _testScope.reset();
    }

    void processAllEngineQueue()
    {
        utils::Function func;
        while (_testScope->engineTaskQueue.pop(func))
        {
            func();
        }
    }

    config::Config _config;
    std::unique_ptr<MixerTestScope> _testScope;
    std::unique_ptr<Mixer> _mixer;
    utils::IdGenerator _idGenerator;
    utils::SsrcGenerator _ssrcGenerator;
};

TEST_F(DataChannelMessageSizeTest, sendLargeEndpointMessageSucceeds)
{
    const std::string endpointId0 = "endpoint-0";
    const std::string endpointId1 = "endpoint-1";

    auto transport0 = std::make_shared<NiceMock<RtcTransportMock>>();
    auto transport1 = std::make_shared<NiceMock<RtcTransportMock>>();

    const auto endpointId0Hash = utils::hash<std::string>{}(endpointId0);
    const auto endpointId1Hash = utils::hash<std::string>{}(endpointId1);

    alignas(memory::Packet) const char webRtcOpen[] =
        "\x03\x00\x00\x00\x00\x00\x00\x00\x00\x12\x00\x00\x77\x65\x62\x72"
        "\x74\x63\x2d\x64\x61\x74\x61\x63\x68\x61\x6e\x6e\x65\x6c\x00\x00";

    _testScope->transportFactoryMock->willReturnByDefaultForAll(nullptr);
    EXPECT_CALL(*_testScope->transportFactoryMock, create(_, endpointId0Hash, _, _, _, _, _, _))
        .WillOnce(Return(transport0));
    EXPECT_CALL(*_testScope->transportFactoryMock, create(_, endpointId1Hash, _, _, _, _, _, _))
        .WillOnce(Return(transport1));

    _mixer->addBundleTransportIfNeeded(endpointId0, ice::IceRole::CONTROLLING, false, false);
    _mixer->addBundleTransportIfNeeded(endpointId1, ice::IceRole::CONTROLLING, false, false);

    std::string dataStreamId;
    ASSERT_TRUE(_mixer->addBundledDataStream(dataStreamId, endpointId0));
    ASSERT_TRUE(_mixer->addBundledDataStream(dataStreamId, endpointId1));
    _mixer->configureDataStream(endpointId0, 5000);
    _mixer->configureDataStream(endpointId1, 5000);
    _mixer->addDataStreamToEngine(endpointId0);
    _mixer->addDataStreamToEngine(endpointId1);

    processAllEngineQueue();

    auto* dataStream1 = _mixer->getEngineDataStream(endpointId1);
    ASSERT_NE(nullptr, dataStream1);
    dataStream1->stream.onSctpMessage(&dataStream1->transport, 0, 0, webrtc::DataChannelPpid::WEBRTC_ESTABLISH,
        webRtcOpen,
        sizeof(webRtcOpen) - 1); // Open the stream

    // Opening this data channel is not actually required for this test, but it's added for clarity.
    auto* dataStream0 = _mixer->getEngineDataStream(endpointId0);
    ASSERT_NE(nullptr, dataStream0);
    dataStream0->stream.onSctpMessage(&dataStream0->transport, 0, 0, webrtc::DataChannelPpid::WEBRTC_ESTABLISH,
        webRtcOpen, 
        sizeof(webRtcOpen) - 1); // Open the stream


    //Creating a payload so that full message will still be
    // slighly below configured max (see "_config.sctp.maxMessageSize = 4096" in SetUp);
    std::string largeMessage(4000, 'a');

    utils::StringBuilder<8192> builder;
    std::string quotedPayload = "\"" + largeMessage + "\"";
    api::DataChannelMessage::makeEndpointMessage(builder, endpointId1, endpointId0, quotedPayload.c_str());
    auto expectedJson = utils::SimpleJson::create(builder.get(), builder.getLength());

    const auto payloadJson = api::DataChannelMessageParser::getEndpointMessagePayload(expectedJson);
    ASSERT_FALSE(payloadJson.isNone());

    EXPECT_CALL(*transport1, sendSctp(_, _, _, _))
        .WillOnce(Invoke(
            [&](uint16_t streamId, uint32_t protocolId, const void* data, uint16_t len) {
                std::string sentData(static_cast<const char*>(data), len);
                EXPECT_EQ(std::string(expectedJson.jsonBegin(), expectedJson.size()), sentData);
                return true;
            }));

    auto fromEndpointIdHash = utils::hash<std::string>{}(endpointId0);
    _mixer->sendEndpointMessage(endpointId1, fromEndpointIdHash, payloadJson);
    processAllEngineQueue();
}

TEST_F(DataChannelMessageSizeTest, sendLargeEndpointMessageFails)
{
    const std::string endpointId0 = "endpoint-0";
    const std::string endpointId1 = "endpoint-1";

    auto transport0 = std::make_shared<NiceMock<RtcTransportMock>>();
    auto transport1 = std::make_shared<NiceMock<RtcTransportMock>>();

    const auto endpointId0Hash = utils::hash<std::string>{}(endpointId0);
    const auto endpointId1Hash = utils::hash<std::string>{}(endpointId1);

    alignas(memory::Packet) const char webRtcOpen[] =
        "\x03\x00\x00\x00\x00\x00\x00\x00\x00\x12\x00\x00\x77\x65\x62\x72"
        "\x74\x63\x2d\x64\x61\x74\x61\x63\x68\x61\x6e\x6e\x65\x6c\x00\x00";

    _testScope->transportFactoryMock->willReturnByDefaultForAll(nullptr);
    EXPECT_CALL(*_testScope->transportFactoryMock, create(_, endpointId0Hash, _, _, _, _, _, _))
        .WillOnce(Return(transport0));
    EXPECT_CALL(*_testScope->transportFactoryMock, create(_, endpointId1Hash, _, _, _, _, _, _))
        .WillOnce(Return(transport1));

    _mixer->addBundleTransportIfNeeded(endpointId0, ice::IceRole::CONTROLLING, false, false);
    _mixer->addBundleTransportIfNeeded(endpointId1, ice::IceRole::CONTROLLING, false, false);

    std::string dataStreamId;
    ASSERT_TRUE(_mixer->addBundledDataStream(dataStreamId, endpointId0));
    ASSERT_TRUE(_mixer->addBundledDataStream(dataStreamId, endpointId1));
    _mixer->configureDataStream(endpointId0, 5000);
    _mixer->configureDataStream(endpointId1, 5000);
    _mixer->addDataStreamToEngine(endpointId0);
    _mixer->addDataStreamToEngine(endpointId1);

    processAllEngineQueue();

    auto* dataStream1 = _mixer->getEngineDataStream(endpointId1);
    ASSERT_NE(nullptr, dataStream1);
    dataStream1->stream.onSctpMessage(&dataStream1->transport, 0, 0, webrtc::DataChannelPpid::WEBRTC_ESTABLISH,
        webRtcOpen,
        sizeof(webRtcOpen) - 1); // Open the stream

    // Opening this data channel is not actually required for this test, but it's added for clarity.
    auto* dataStream0 = _mixer->getEngineDataStream(endpointId0);
    ASSERT_NE(nullptr, dataStream0);
    dataStream0->stream.onSctpMessage(&dataStream0->transport, 0, 0, webrtc::DataChannelPpid::WEBRTC_ESTABLISH,
        webRtcOpen, 
        sizeof(webRtcOpen) - 1); // Open the stream


    //Creating a payload so that full message will  be
    //slightly above configured max (see "_config.sctp.maxMessageSize = 4096" in SetUp);
    std::string largeMessage(4096, 'a');

    utils::StringBuilder<8192> builder;
    std::string quotedPayload = "\"" + largeMessage + "\"";
    api::DataChannelMessage::makeEndpointMessage(builder, endpointId1, endpointId0, quotedPayload.c_str());
    auto expectedJson = utils::SimpleJson::create(builder.get(), builder.getLength());

    const auto payloadJson = api::DataChannelMessageParser::getEndpointMessagePayload(expectedJson);
    ASSERT_FALSE(payloadJson.isNone());

    EXPECT_CALL(*transport1, sendSctp(_, _, _, _))
        .Times(0); // Expect sendSctp to never be called.

    auto fromEndpointIdHash = utils::hash<std::string>{}(endpointId0);
    _mixer->sendEndpointMessage(endpointId1, fromEndpointIdHash, payloadJson);
    processAllEngineQueue();
}

TEST_F(DataChannelMessageSizeTest, forwardLargeEndpointMessageSucceeds)
{
    const std::string endpointId0 = "endpoint-0";
    const std::string endpointId1 = "endpoint-1";

    auto transport0 = std::make_shared<NiceMock<RtcTransportMock>>();
    auto transport1 = std::make_shared<NiceMock<RtcTransportMock>>();

    ON_CALL(*transport0, getTag()).WillByDefault(Return("tag-transport0"));
    ON_CALL(*transport1, getTag()).WillByDefault(Return("tag-transport1"));

    const auto endpointId0Hash = utils::hash<std::string>{}(endpointId0);
    const auto endpointId1Hash = utils::hash<std::string>{}(endpointId1);

    ON_CALL(*transport0, getEndpointIdHash()).WillByDefault(Return(endpointId0Hash));
    ON_CALL(*transport1, getEndpointIdHash()).WillByDefault(Return(endpointId1Hash));

    jobmanager::TimerQueue timerQueue(1024);
    JobManagerProcessor backgroundJobManager(timerQueue);

    bridge::Engine engine(backgroundJobManager.getJobManager());
    bridge::MixerManager mixerManager(_idGenerator,
        _ssrcGenerator,
        _testScope->jobManagerProcessor.getJobManager(),
        backgroundJobManager.getJobManager(),
        *_testScope->transportFactoryMock,
        engine,
        _config,
        _testScope->packetAllocator,
        _testScope->packetAllocator,
        _testScope->audioPacketAllocator);

    auto mixer = mixerManager.create(utils::Optional<uint32_t>(5),true,false);

    ON_CALL(_testScope->mixerManagerAsyncMock, post(_))
    .WillByDefault(Invoke([](auto&& task) {
        task();
        return true;
    }));

    _testScope->transportFactoryMock->willReturnByDefaultForAll(nullptr);
    EXPECT_CALL(*_testScope->transportFactoryMock, create(_, endpointId0Hash, _, _, _, _, _, _))
        .WillOnce(Return(transport0));
    EXPECT_CALL(*_testScope->transportFactoryMock, create(_, endpointId1Hash, _, _, _, _, _, _))
        .WillOnce(Return(transport1));

    mixer->addBundleTransportIfNeeded(endpointId0, ice::IceRole::CONTROLLING, false, false);
    mixer->addBundleTransportIfNeeded(endpointId1, ice::IceRole::CONTROLLING, false, false);

    std::string dataStreamId;
    ASSERT_TRUE(mixer->addBundledDataStream(dataStreamId, endpointId0));
    ASSERT_TRUE(mixer->addBundledDataStream(dataStreamId, endpointId1));
    mixer->configureDataStream(endpointId0, 5000);
    mixer->configureDataStream(endpointId1, 5000);
    mixer->addDataStreamToEngine(endpointId0);
    mixer->addDataStreamToEngine(endpointId1);

    backgroundJobManager.process();
    processAllEngineQueue();

    alignas(memory::Packet) const char webRtcOpen[] =
        "\x03\x00\x00\x00\x00\x00\x00\x00\x00\x12\x00\x00\x77\x65\x62\x72"
        "\x74\x63\x2d\x64\x61\x74\x61\x63\x68\x61\x6e\x6e\x65\x6c\x00\x00";

    auto* dataStream1 = mixer->getEngineDataStream(endpointId1);
    ASSERT_NE(nullptr, dataStream1);
    dataStream1->stream.onSctpMessage(&dataStream1->transport, 0, 0, webrtc::DataChannelPpid::WEBRTC_ESTABLISH,
        webRtcOpen,
        sizeof(webRtcOpen) - 1); // Open the stream

    auto* dataStream0 = mixer->getEngineDataStream(endpointId0);
    ASSERT_NE(nullptr, dataStream0);
    dataStream0->stream.onSctpMessage(&dataStream0->transport, 0, 0, webrtc::DataChannelPpid::WEBRTC_ESTABLISH,
        webRtcOpen,
        sizeof(webRtcOpen) - 1); // Open the stream

    //Creating a payload so that full message will still be
    //slighly below configured max (see "_config.sctp.maxMessageSize = 4096" in SetUp);
    std::string largeMessage(4000, 'a');

    utils::StringBuilder<8192> builder;
    std::string quotedPayload = "\"" + largeMessage + "\"";
    api::DataChannelMessage::makeEndpointMessage(builder, endpointId1, endpointId0, quotedPayload.c_str());
    const auto message = builder.get();

    EXPECT_CALL(*transport1, sendSctp(_, _, _, _))
        .WillOnce(Invoke(
            [&](uint16_t streamId, uint32_t protocolId, const void* data, uint16_t len) {
                std::string sentData(static_cast<const char*>(data), len);
                EXPECT_EQ(message, sentData);
                return true;
            }));

    engine.stop();

    mixer->getEngineMixer()->onSctpMessage(&dataStream0->transport,
        0, 0,
        webrtc::DataChannelPpid::WEBRTC_STRING,
        message,
        builder.getLength());

    backgroundJobManager.process();
    processAllEngineQueue();
}
