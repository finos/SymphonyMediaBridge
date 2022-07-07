#pragma once

#include "bridge/Bridge.h"
#include "config/Config.h"
#include "utils/Pacer.h"
#include <cstdint>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

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

    uint32_t _instanceCounter;

    void SetUp() override;
    void TearDown() override;

    void initBridge(config::Config& config);
};


