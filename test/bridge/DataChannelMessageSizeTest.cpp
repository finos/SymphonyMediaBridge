#include "api/DataChannelMessage.h"
#include "api/DataChannelMessageParser.h"
#include "memory/PacketPoolAllocator.h"
#include "memory/PoolBuffer.h"
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
#include <thread>
#include <chrono>

#include "bridge/MixerManager.h"

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

    void activatePendingJobs() {
       for (auto job : pendingJobs) {
            jobManager.addJobItem(job);
       }
       pendingJobs.clear();
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
          mainPacketAllocator(128 * 1024, "MainAllocator-test"),
          sendPacketAllocator(32 * 1024, "SendAllocator-test"),
          audioPacketAllocator(4 * 1024, "AudioAllocator-test")
    {
    }

    std::shared_ptr<NiceMock<TransportFactoryMock>> transportFactoryMock;
    StrictMock<MixerManagerAsyncMock> mixerManagerAsyncMock;
    concurrency::MpmcQueue<utils::Function> engineTaskQueue;
    concurrency::SynchronizationContext engineSyncContext;
    jobmanager::TimerQueue timeQueue;
    JobManagerProcessor jobManagerProcessor;
    JobManagerProcessor backgroundJobManagerProcessor;
    memory::PacketPoolAllocator mainPacketAllocator;
    memory::PacketPoolAllocator sendPacketAllocator;
    memory::AudioPacketPoolAllocator audioPacketAllocator;
};
} // namespace

class DataChannelMessageSizeTest : public ::testing::Test
{
public:
    DataChannelMessageSizeTest() : _testScope(std::make_unique<MixerTestScope>()) {}

protected:
    struct DataChannelEndpoints
    {
        std::shared_ptr<NiceMock<RtcTransportMock>> transport0;
        std::shared_ptr<NiceMock<RtcTransportMock>> transport1;
        const std::string endpointId0 = "endpoint-0";
        const std::string endpointId1 = "endpoint-1";
        size_t endpointId0Hash;
        size_t endpointId1Hash;
    };

    DataChannelEndpoints createDataChannelEndpoints()
    {
        DataChannelEndpoints endpoints;
        endpoints.endpointId0Hash = utils::hash<std::string>{}(endpoints.endpointId0);
        endpoints.endpointId1Hash = utils::hash<std::string>{}(endpoints.endpointId1);

        endpoints.transport0 = std::make_shared<NiceMock<RtcTransportMock>>();
        endpoints.transport1 = std::make_shared<NiceMock<RtcTransportMock>>();

        _testScope->transportFactoryMock->willReturnByDefaultForAll(nullptr);
        EXPECT_CALL(*_testScope->transportFactoryMock, create(_, endpoints.endpointId0Hash, _, _, _, _, _, _))
            .WillOnce(Return(endpoints.transport0));
        EXPECT_CALL(*_testScope->transportFactoryMock, create(_, endpoints.endpointId1Hash, _, _, _, _, _, _))
            .WillOnce(Return(endpoints.transport1));

        return endpoints;
    }

    void connectDataStreams(Mixer& mixer, const DataChannelEndpoints& endpoints)
    {
        mixer.addBundleTransportIfNeeded(endpoints.endpointId0, ice::IceRole::CONTROLLING, false, false);
        mixer.addBundleTransportIfNeeded(endpoints.endpointId1, ice::IceRole::CONTROLLING, false, false);

        std::string dataStreamId;
        ASSERT_TRUE(mixer.addBundledDataStream(dataStreamId, endpoints.endpointId0));
        ASSERT_TRUE(mixer.addBundledDataStream(dataStreamId, endpoints.endpointId1));
        mixer.configureDataStream(endpoints.endpointId0, 5000);
        mixer.configureDataStream(endpoints.endpointId1, 5000);
        mixer.addDataStreamToEngine(endpoints.endpointId0);
        mixer.addDataStreamToEngine(endpoints.endpointId1);
    }

    void openDataChannel(Mixer& mixer, const std::string& endpointsId)
    {
        alignas(memory::Packet) const char webRtcOpen[] =
            "\x03\x00\x00\x00\x00\x00\x00\x00\x00\x12\x00\x00\x77\x65\x62\x72"
            "\x74\x63\x2d\x64\x61\x74\x61\x63\x68\x61\x6e\x6e\x65\x6c\x00\x00";

        webrtc::SctpStreamMessageHeader header = {webrtc::DataChannelPpid::WEBRTC_ESTABLISH, 0, 0};
        memory::PoolBuffer<memory::PacketPoolAllocator> buffer(_testScope->mainPacketAllocator);
        buffer.allocate(sizeof(webrtc::SctpStreamMessageHeader) + sizeof(webRtcOpen));
        buffer.copyFrom(&header, sizeof(webrtc::SctpStreamMessageHeader), 0);
        buffer.copyFrom(webRtcOpen, sizeof(webRtcOpen) - 1, sizeof(webrtc::SctpStreamMessageHeader));

        auto* dataStream = mixer.getEngineDataStream(endpointsId);
        ASSERT_NE(nullptr, dataStream);
        dataStream->stream.onSctpMessageBuffer(&dataStream->transport, buffer);
    }

    void openDataChannels(Mixer& mixer, const DataChannelEndpoints& endpoints)
    {
        openDataChannel(mixer, endpoints.endpointId0);
        openDataChannel(mixer, endpoints.endpointId1);
    }

    void gracefullyTerminateMixerManager(bridge::Mixer* mixer,
        bridge::MixerManager& mixerManager,
        JobManagerProcessor& backgroundJobManager)
    {
        auto id = mixer->getId();
        mixerManager.remove(id);
        processAllEngineQueue(); // will be removed on engine thread
        backgroundJobManager.process(); // and event posted to background thread of MixerManager

        std::thread backgroundProcessor([&backgroundJobManager]() {
            for (int i = 0; i < 300; ++i)
            {
                backgroundJobManager.process();
                if (i % 10 == 0)
                {
                    backgroundJobManager.activatePendingJobs();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

        mixerManager.stop();
        backgroundProcessor.join();
    }


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
            _testScope->sendPacketAllocator,
            _testScope->audioPacketAllocator,
            _testScope->mainPacketAllocator,
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

struct SendMessageSizeTestParam
{
    size_t payloadSize;
    int expectedSendSctpCalls;
};

class DataChannelSendMessageSizeTest : public DataChannelMessageSizeTest,
                                       public WithParamInterface<SendMessageSizeTestParam>
{
};

TEST_P(DataChannelSendMessageSizeTest, sendLargeEndpointMessage)
{
    const auto param = GetParam();
    auto endpoints = createDataChannelEndpoints();

    ON_CALL(*endpoints.transport0, getAllocator()).WillByDefault(ReturnRef(_testScope->mainPacketAllocator));
    ON_CALL(*endpoints.transport1, getAllocator()).WillByDefault(ReturnRef(_testScope->mainPacketAllocator));

    // 1. ARRANGE: setup data channels and message size
    connectDataStreams(*_mixer, endpoints);
    processAllEngineQueue();
    openDataChannels(*_mixer, endpoints);
    std::string largeMessage(param.payloadSize, 'a');

    // 2. ACT: create a message of size close to or exceeding max and attempt to send it via 'sendEndpointMessage'
    utils::StringBuilder<8192> builder;
    std::string quotedPayload = "\"" + largeMessage + "\"";
    api::DataChannelMessage::makeEndpointMessage(builder, endpoints.endpointId1, endpoints.endpointId0, quotedPayload.c_str());
    auto expectedJson = utils::SimpleJson::create(builder.get(), builder.getLength());

    const auto payloadJson = api::DataChannelMessageParser::getEndpointMessagePayload(expectedJson);
    ASSERT_FALSE(payloadJson.isNone());

    // 3. ASSERT: if message size is smaller than _config.sctp.maxMessageSize = 4096 send should succeed, otherwise fail
    auto& expect = EXPECT_CALL(*endpoints.transport1, sendSctp(_, _, _))
        .Times(param.expectedSendSctpCalls);
    if (param.expectedSendSctpCalls > 0)
    {
        expect.WillOnce(Invoke(
            [&](uint16_t streamId, uint32_t protocolId, memory::PoolBuffer<memory::PacketPoolAllocator> buffer) {
                char continuousBuffer[buffer.getLength()];
                buffer.copyTo(continuousBuffer, 0, buffer.getLength());
                std::string sentData(continuousBuffer, buffer.getLength());

                EXPECT_EQ(std::string(expectedJson.jsonBegin(), expectedJson.size()), sentData);
                return true;
            }));
    }

    _mixer->sendEndpointMessage(endpoints.endpointId1, endpoints.endpointId0Hash, payloadJson);
    processAllEngineQueue();
}

INSTANTIATE_TEST_SUITE_P(DataChannelMessageSize,
    DataChannelSendMessageSizeTest,
    Values(SendMessageSizeTestParam{4000, 1},   // size of endpoint message < 4096 - send should happen (1 time)
           SendMessageSizeTestParam{4096, 0}),  // size of endpoint message > 4096 - send should fail (happen 0 times)
    [](const testing::TestParamInfo<SendMessageSizeTestParam>& info) {
        if (info.param.expectedSendSctpCalls > 0)
        {
            return "Succeeds";
        }
        return "Fails";
    });

struct ForwardMessageSizeTestParam
{
    size_t payloadSize;
    int expectedSendSctpCalls;
};

class MockEngine : public bridge::Engine
{
public:
    MockEngine(jobmanager::JobManager& backgroundJobQueue) : bridge::Engine(backgroundJobQueue, {}) {}

    MOCK_METHOD(bool, post, (utils::Function && task), (override));
    MOCK_METHOD(concurrency::SynchronizationContext, getSynchronizationContext, (), (override));
};

class DataChannelForwardMessageSizeTest : public DataChannelMessageSizeTest,
                                          public WithParamInterface<ForwardMessageSizeTestParam>
{
};

TEST_P(DataChannelForwardMessageSizeTest, forwardLargeEndpointMessage)
{
    const auto param = GetParam();

    // 1. ARRANGE: setup mixer, engine, data channels and message size
    jobmanager::TimerQueue timerQueue(1024);
    JobManagerProcessor backgroundJobManager(timerQueue);

    concurrency::MpmcQueue<utils::Function>& engineQueue = _testScope->engineTaskQueue;
    NiceMock<MockEngine> engine(backgroundJobManager.getJobManager());
    ON_CALL(engine, post(_)).WillByDefault(Invoke([&engineQueue](utils::Function&& task) {
        return engineQueue.push(std::move(task));
    }));
    ON_CALL(engine, getSynchronizationContext()).WillByDefault(Return(concurrency::SynchronizationContext(_testScope->engineTaskQueue)));

    bridge::MixerManager mixerManager(_idGenerator,
        _ssrcGenerator,
        _testScope->jobManagerProcessor.getJobManager(),
        backgroundJobManager.getJobManager(),
        *_testScope->transportFactoryMock,
        engine,
        _config,
        _testScope->mainPacketAllocator,
        _testScope->sendPacketAllocator,
        _testScope->audioPacketAllocator);
    auto mixer = mixerManager.create(utils::Optional<uint32_t>(5), true, false);

    auto endpoints = createDataChannelEndpoints();

    ON_CALL(*endpoints.transport0, getAllocator()).WillByDefault(ReturnRef(_testScope->mainPacketAllocator));
    ON_CALL(*endpoints.transport1, getAllocator()).WillByDefault(ReturnRef(_testScope->mainPacketAllocator));

    ON_CALL(*endpoints.transport0, getTag()).WillByDefault(Return("tag-transport0"));
    ON_CALL(*endpoints.transport1, getTag()).WillByDefault(Return("tag-transport1"));
    ON_CALL(*endpoints.transport0, getEndpointIdHash()).WillByDefault(Return(endpoints.endpointId0Hash));
    ON_CALL(*endpoints.transport1, getEndpointIdHash()).WillByDefault(Return(endpoints.endpointId1Hash));

    connectDataStreams(*mixer, endpoints);

    backgroundJobManager.process();

    openDataChannels(*mixer, endpoints);

    std::string largeMessage(param.payloadSize, 'a');

    // 2. ACT: create a message of size close to or exceeding max and attempt to FORWARD it via 'onSctpMessage'
    // FORWARD: engine mixer receives the in 'onSctpMessage' and later transport sends/forwards it via 'sendSctp'
    utils::StringBuilder<8192> builder;
    std::string quotedPayload = "\"" + largeMessage + "\"";
    api::DataChannelMessage::makeEndpointMessage(builder, endpoints.endpointId1, endpoints.endpointId0, quotedPayload.c_str());
    const auto message = builder.get();

    // 3. ASSERT: if message size is smaller than _config.sctp.maxMessageSize = 4096 FORWARD should succeed, otherwise fail
    auto& expect = EXPECT_CALL(*endpoints.transport1, sendSctp(_, _, _))
        .Times(param.expectedSendSctpCalls);
    if (param.expectedSendSctpCalls > 0)
    {
        expect.WillOnce(Invoke([&](uint16_t streamId, uint32_t protocolId, memory::PoolBuffer<memory::PacketPoolAllocator> buffer) {
            char continuousBuffer[buffer.getLength()];
            buffer.copyTo(continuousBuffer, 0, buffer.getLength());
            std::string sentData(continuousBuffer, buffer.getLength());

            EXPECT_EQ(message, sentData);
            return true;
        }));
    }

    mixer->getEngineMixer()->onSctpMessage(&mixer->getEngineDataStream(endpoints.endpointId0)->transport,
        0,
        0,
        webrtc::DataChannelPpid::WEBRTC_STRING,
        message,
        builder.getLength());

    backgroundJobManager.process();
    processAllEngineQueue();

    gracefullyTerminateMixerManager(mixer, mixerManager, backgroundJobManager);
}

INSTANTIATE_TEST_SUITE_P(DataChannelMessageSize,
    DataChannelForwardMessageSizeTest,
    Values(ForwardMessageSizeTestParam{4000, 1},  // size of endpoint message < 4096 - send should happen (1 time)
           ForwardMessageSizeTestParam{4097, 0}), // size of endpoint message > 4096 - send should fail (happen 0 times)
    [](const testing::TestParamInfo<ForwardMessageSizeTestParam>& info) {
        if (info.param.expectedSendSctpCalls > 0)
        {
            return "Succeeds";
        }
        return "Fails";
    });

TEST(DataChannelMessageTest, makeLoggableStringFromBuffer_smallBufferEllipsis)
{
    // Test case for T < 4 where ellipsis is needed
    // This targets the potential buffer underflow before the fix.

    memory::PacketPoolAllocator testAllocator(1024, "TestAllocator");

    // Test with T = 3, payload "abcde"
    memory::Array<char, 3> outArray3;
    memory::PoolBuffer<memory::PacketPoolAllocator> payload(testAllocator, "abcde", 5);

    // Call the function - it should not crash
    api::DataChannelMessage::makeLoggableStringFromBuffer(outArray3, payload);

    // Verify it's null-terminated and no crash
    ASSERT_EQ(outArray3.size(), 3);
    ASSERT_EQ(std::string(outArray3.data()), "ab");

    // Test with T = 2, payload "abcde"
    memory::Array<char, 2> outArray2;
    memory::PoolBuffer<memory::PacketPoolAllocator> payload2(testAllocator, "abcde", 5);

    api::DataChannelMessage::makeLoggableStringFromBuffer(outArray2, payload2);
    ASSERT_EQ(outArray2.size(), 2);
    ASSERT_EQ(std::string(outArray2.data()), "a");

    // Test with T = 1, payload "abcde"
    memory::Array<char, 1> outArray1;
    // payload is still in scope from previous test for its content

    api::DataChannelMessage::makeLoggableStringFromBuffer(outArray1, payload);
    ASSERT_EQ(outArray1.size(), 1);
    ASSERT_EQ(std::string(outArray1.data()), "");
}

TEST(DataChannelMessageTest, makeLoggableStringFromBuffer_bigBufferEllipsis)
{
    memory::PacketPoolAllocator testAllocator(1024, "TestAllocator");
    std::string payloadString = "abcdefghjklmnopqrstuvwxyz";

    // Test with T = 10
    memory::Array<char, 10> outArray10;
    memory::PoolBuffer<memory::PacketPoolAllocator> payload(testAllocator, payloadString.c_str(), payloadString.length());

    // Call the function - it should not crash
    api::DataChannelMessage::makeLoggableStringFromBuffer(outArray10, payload);

    // Verify it's null-terminated and no crash
    ASSERT_EQ(outArray10.size(), 10);

    std::string resultStr10(outArray10.data());

    ASSERT_EQ(resultStr10.length(), 9);
    ASSERT_EQ(resultStr10, "abcdef...");

    // Test with T = 5
    memory::Array<char, 5> outArray5;

    // Call the function - it should not crash
    api::DataChannelMessage::makeLoggableStringFromBuffer(outArray5, payload);

    // Verify it's null-terminated and no crash
    ASSERT_EQ(outArray5.size(), 5);

    std::string resultStr5(outArray5.data());

    ASSERT_EQ(resultStr5.length(), 4);
    ASSERT_EQ(resultStr5, "a...");

    // Test with T = 4
    memory::Array<char, 4> outArray4;

    // Call the function - it should not crash
    api::DataChannelMessage::makeLoggableStringFromBuffer(outArray4, payload);

    // Verify it's null-terminated and no crash
    ASSERT_EQ(outArray4.size(), 4);

    std::string resultStr4(outArray4.data());

    ASSERT_EQ(resultStr4.length(), 3);
    ASSERT_EQ(resultStr4, "...");
}