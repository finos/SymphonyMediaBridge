#pragma once

#include "api/ConferenceEndpoint.h"
#include "bridge/Bridge.h"
#include "config/Config.h"
#include "emulator/TimeTurner.h"
#include "test/transport/FakeNetwork.h"
#include "transport/EndpointFactory.h"
#include "utils/Pacer.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <mutex>

struct IntegrationTest : public ::testing::Test
{
    IntegrationTest();
    std::unique_ptr<bridge::Bridge> _bridge;
    config::Config _config;

    memory::PacketPoolAllocator _sendAllocator;
    memory::AudioPacketPoolAllocator _audioAllocator;
    std::unique_ptr<jobmanager::JobManager> _jobManager;
    std::unique_ptr<memory::PacketPoolAllocator> _mainPoolAllocator;
    transport::SslDtls* _sslDtls;
    std::unique_ptr<transport::SrtpClientFactory> _srtpClientFactory;
    std::vector<std::unique_ptr<jobmanager::WorkerThread>> _workerThreads;
    std::unique_ptr<transport::RtcePoll> _network;
    ice::IceConfig _iceConfig;
    sctp::SctpConfig _sctpConfig;
    bwe::Config _bweConfig;
    bwe::RateControllerConfig _rateControlConfig;
    utils::Pacer _pacer;

    std::unique_ptr<transport::TransportFactory> _transportFactory;
    std::shared_ptr<fakenet::InternetRunner> _internet;
    std::shared_ptr<transport::EndpointFactory> _bridgeEndpointFactory;
    std::shared_ptr<transport::EndpointFactory> _clientsEndpointFactory;

    uint32_t _instanceCounter;
    const size_t _numWorkerThreads;

    void SetUp() override;
    void TearDown() override;

    void initBridge(config::Config& config);

    void finalizeSimulationWithTimeout(uint64_t rampdownTimeout);
    void finalizeSimulation();

public:
    static bool isActiveTalker(const std::vector<api::ConferenceEndpoint>& endpoints, const std::string& endpoint);
    static std::vector<api::ConferenceEndpoint> getConferenceEndpointsInfo(const char* baseUrl);
    static api::ConferenceEndpointExtendedInfo getEndpointExtendedInfo(const char* baseUrl,
        const std::string& endpointId);
    static void analyzeRecording(const std::vector<int16_t>& recording,
        std::vector<double>& frequencyPeaks,
        std::vector<std::pair<uint64_t, double>>& amplitudeProfile,
        const char* logId,
        uint64_t cutAtTime = 0);

protected:
    void runTestInThread(const size_t expectedNumThreads, std::function<void()> test);
    void startSimulation();

protected:
    bool _internetStartedAtLeastOnce;
    emulator::TimeTurner _timeSource;

    struct NetworkLinkInfo
    {
        fakenet::NetworkLink* ptrLink;
        transport::SocketAddress address;
    };

    std::map<std::string, NetworkLinkInfo> _endpointNetworkLinkMap;

private:
    size_t getNumWorkerThreads();
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
