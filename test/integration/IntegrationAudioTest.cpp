#include "api/Parser.h"
#include "api/utils.h"
#include "bridge/Mixer.h"
#include "bridge/endpointActions/ApiHelpers.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "codec/Opus.h"
#include "codec/OpusDecoder.h"
#include "concurrency/MpmcHashmap.h"
#include "emulator/FakeEndpointFactory.h"
#include "external/http.h"
#include "jobmanager/JobManager.h"
#include "jobmanager/WorkerThread.h"
#include "memory/PacketPoolAllocator.h"
#include "nlohmann/json.hpp"
#include "test/bwe/FakeVideoSource.h"
#include "test/integration/IntegrationTest.h"
#include "test/integration/SampleDataUtils.h"
#include "test/integration/emulator/AudioSource.h"
#include "test/integration/emulator/HttpRequests.h"
#include "transport/DataReceiver.h"
#include "transport/EndpointFactoryImpl.h"
#include "transport/RtcePoll.h"
#include "transport/Transport.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SslDtls.h"
#include "utils/Format.h"
#include "utils/IdGenerator.h"
#include "utils/ScopedFileHandle.h"
#include "utils/SimpleJson.h"
#include "utils/StringBuilder.h"
#include <chrono>
#include <complex>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <unordered_set>

using namespace emulator;

// TODO we should move some audio specific integration tests from ConfIntegrationTest.cpp to this file
class IntegrationAudioTest : public IntegrationTest
{
public:
    IntegrationAudioTest() { _networkTickInterval = 500 * utils::Time::us; }
};

TEST_F(IntegrationAudioTest, longMute)
{
    runTestInThread(
        expectedTestThreadCount(1),
        [this]() {
            _config.readFromString(_defaultSmbConfig);

            initBridge(_config);
            const auto baseUrl = "http://127.0.0.1:8080";

            GroupCall<SfuClient<Channel>> group(_httpd,
                _instanceCounter,
                *_mainPoolAllocator,
                _audioAllocator,
                *_clientTransportFactory,
                *_publicTransportFactory,
                *_sslDtls,
                2);

            Conference conf(_httpd);

            ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
            startSimulation();

            group.startConference(conf, baseUrl);
            CallConfigBuilder cfg(conf.getId());
            cfg.url(baseUrl).withAudio();

            CallConfigBuilder cfg2 = cfg;

            group.clients[0]->initiateCall(cfg2.withOpus().build());
            group.clients[1]->joinCall(cfg.build());

            ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));
            group.clients[0]->_audioSource->initSequenceCounter(65135); // mute before wrap

            startCallWithDefaultAudioProfile(group, utils::Time::sec * 2);
            group.clients[0]->_audioSource->setVolume(0);
            group.run(utils::Time::sec * 656);
            group.clients[0]->_audioSource->setVolume(0.6);

            group.run(utils::Time::sec * 10);
            auto audioReceiveStats1 = group.clients[1]->_bundleTransport->getCumulativeAudioReceiveCounters();

            endCall(group);

            group.awaitPendingJobs(utils::Time::sec * 7);
            finalizeSimulation();
            EXPECT_LT(audioReceiveStats1.packets, 700); // few packets received due to mute
            EXPECT_GT(audioReceiveStats1.packets, 500); // at least 8s audio received
            EXPECT_EQ(audioReceiveStats1.lostPackets, 0); // no packets lost to protect error
        },
        30 * 60);
}

// If you lose packets with ROC 0 you can never start decrypting as libsrtp does not register the ssrc unless there is a
// successful decryption of a packet with ROC0.
TEST_F(IntegrationAudioTest, losePacketsWithRoc0)
{
    runTestInThread(
        expectedTestThreadCount(1),
        [this]() {
            _config.readFromString(_defaultSmbConfig);

            const auto smbIp = _smbInterfaces[0];
            initBridge(_config);
            const auto baseUrl = "http://127.0.0.1:8080";

            GroupCall<SfuClient<Channel>> group(_httpd,
                _instanceCounter,
                *_mainPoolAllocator,
                _audioAllocator,
                *_clientTransportFactory,
                *_publicTransportFactory,
                *_sslDtls,
                2);

            Conference conf(_httpd);

            ScopedFinalize finalize(std::bind(&IntegrationTest::finalizeSimulation, this));
            startSimulation();

            group.startConference(conf, baseUrl);
            CallConfigBuilder cfg(conf.getId());
            cfg.url(baseUrl).withAudio();

            CallConfigBuilder cfg2 = cfg;

            group.clients[0]->initiateCall(cfg2.withOpus().build());
            group.clients[1]->joinCall(cfg.build());

            ASSERT_TRUE(group.connectAll(utils::Time::sec * _clientsConnectionTimeout));

            group.clients[0]->_audioSource->initSequenceCounter(65533);

            _firewall->block(group.clients[0]->_bundleTransport->getLocalRtpPort(),
                transport::SocketAddress::createBroadcastIpv4());
            startCallWithDefaultAudioProfile(group, utils::Time::ms * 2);

            _firewall->unblock(group.clients[0]->_bundleTransport->getLocalRtpPort(),
                transport::SocketAddress::createBroadcastIpv4());
            group.run(utils::Time::sec * 4);

            auto audioReceiveCounters = group.clients[1]->_bundleTransport->getCumulativeAudioReceiveCounters();
            endCall(group);

            group.awaitPendingJobs(utils::Time::sec * 7);
            finalizeSimulation();

            EXPECT_GT(audioReceiveCounters.packets, 140);
        },
        30 * 60);
}
