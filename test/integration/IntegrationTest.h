#pragma once

#include "bridge/Bridge.h"
#include "config/Config.h"
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
    std::shared_ptr<transport::EndpointFactory> _endpointFacory;

    uint32_t _instanceCounter;

    void SetUp() override;
    void TearDown() override;

    void initBridge(config::Config& config);

protected:
    void startInternet();
    void stopInternet();
    bool _internetStartedAtLeastOnce;
};
