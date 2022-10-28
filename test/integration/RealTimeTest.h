#pragma once

#include "api/ConferenceEndpoint.h"
#include "bridge/Bridge.h"
#include "config/Config.h"
#include "emulator/TimeTurner.h"
#include "jobmanager/TimerQueue.h"
#include "test/integration/emulator/Httpd.h"
#include "test/transport/FakeNetwork.h"
#include "transport/EndpointFactory.h"
#include "utils/Pacer.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <mutex>

struct RealTimeTest : public ::testing::Test
{

    struct AudioAnalysisData
    {
        std::vector<double> dominantFrequencies;
        std::vector<std::pair<uint64_t, double>> amplitudeProfile;
        size_t audioSsrcCount = 0;
    };

    RealTimeTest();

    std::unique_ptr<bridge::Bridge> _bridge;
    config::Config _bridgeConfig;

    memory::PacketPoolAllocator _sendAllocator;
    memory::AudioPacketPoolAllocator _audioAllocator;
    std::unique_ptr<jobmanager::TimerQueue> _timerQueue;
    std::unique_ptr<jobmanager::JobManager> _jobManager;
    std::unique_ptr<memory::PacketPoolAllocator> _mainPoolAllocator;
    std::unique_ptr<transport::SslDtls> _sslDtls;
    std::unique_ptr<transport::SrtpClientFactory> _srtpClientFactory;
    std::vector<std::unique_ptr<jobmanager::WorkerThread>> _workerThreads;
    std::unique_ptr<transport::RtcePoll> _network;
    ice::IceConfig _iceConfig;
    sctp::SctpConfig _sctpConfig;
    bwe::Config _bweConfig;
    bwe::RateControllerConfig _rateControlConfig;
    utils::Pacer _pacer;

    // local client's transport infra
    config::Config _clientConfig;
    std::shared_ptr<transport::EndpointFactory> _clientsEndpointFactory;
    std::unique_ptr<transport::TransportFactory> _clientTransportFactory;

    uint32_t _instanceCounter;
    const size_t _numWorkerThreads;

    void SetUp() override;
    void TearDown() override;

    void initRealBridge(config::Config& config);

public:
    static bool isActiveTalker(const std::vector<api::ConferenceEndpoint>& endpoints, const std::string& endpoint);
    static std::vector<api::ConferenceEndpoint> getConferenceEndpointsInfo(emulator::HttpdFactory* httpd,
        const char* baseUrl);
    static api::ConferenceEndpointExtendedInfo getEndpointExtendedInfo(emulator::HttpdFactory* httpd,
        const char* baseUrl,
        const std::string& endpointId);

protected:
    void initLocalTransports();

protected:
    const uint32_t _clientsConnectionTimeout;

private:
    size_t getNumWorkerThreads() const;
};

namespace
{
class ScopedFinalize
{
public:
    explicit ScopedFinalize(std::function<void()> finalizeMethod) : _method(finalizeMethod) {}
    ~ScopedFinalize() { _method(); }

private:
    std::function<void()> _method;
};
} // namespace
