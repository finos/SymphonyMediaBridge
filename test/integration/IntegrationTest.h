#pragma once

#include "TransportFactoryStub.h"
#include "bridge/LegacyApiRequestHandler.h"
#include "bridge/Mixer.h"
#include "bridge/MixerManager.h"
#include "bridge/engine/Engine.h"
#include "bridge/engine/EngineMixer.h"
#include "config/Config.h"
#include "httpd/Httpd.h"
#include "jobmanager/JobManager.h"
#include "jobmanager/WorkerThread.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/RtcePoll.h"
#include "transport/dtls/SslDtls.h"
#include "utils/IdGenerator.h"
#include "utils/SsrcGenerator.h"
#include <cstdint>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <unistd.h>

struct IntegrationTest : public ::testing::Test
{
    std::unique_ptr<utils::IdGenerator> idGenerator;
    std::unique_ptr<utils::SsrcGenerator> ssrcGenerator;
    std::unique_ptr<jobmanager::JobManager> jobManager;
    std::unique_ptr<transport::SslDtls> sslDtls;
    std::unique_ptr<TransportFactoryStub> transportFactory;
    std::unique_ptr<config::Config> config;
    std::unique_ptr<bridge::Engine> engine;
    std::unique_ptr<bridge::MixerManager> mixerManager;
    std::unique_ptr<transport::RtcePoll> network;
    std::unique_ptr<bridge::RequestHandler> requestHandler;
    std::unique_ptr<httpd::Httpd> httpd;
    std::unique_ptr<memory::PacketPoolAllocator> poolAllocator;

    void SetUp() override;
    void TearDown() override;

    void startDefaultMixerManager();
    void startHttpDaemon(int port);
};
