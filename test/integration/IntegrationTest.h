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

template <typename T>
class GroupCall
{
public:
    GroupCall(const std::vector<T*> clients) : _clients(clients) {}

    bool connect(uint64_t timeout)
    {
        auto start = utils::Time::getAbsoluteTime();
        for (auto client : _clients)
        {
            if (!client->_channel.isSuccess())
            {
                return false;
            }
        }

        for (auto client : _clients)
        {
            client->processOffer();
            if (!client->_transport || !client->_audioSource)
            {
                return false;
            }
        }

        for (auto client : _clients)
        {
            client->connect();
        }

        for (size_t connectedCount = 0; utils::Time::getAbsoluteTime() - start < timeout; connectedCount = 0)
        {
            for (auto client : _clients)
            {
                if (!client->_transport->isConnected())
                {
                    break;
                }
                else
                {
                    ++connectedCount;
                }
            }

            if (connectedCount == _clients.size())
            {
                return true;
            }
            utils::Time::nanoSleep(1 * utils::Time::sec);
            logger::debug("waiting for connect...", "test");
        }

        return false;
    }

    void run(uint64_t period)
    {
        const auto start = utils::Time::getAbsoluteTime();
        utils::Pacer pacer(10 * utils::Time::ms);
        for (auto timestamp = utils::Time::getAbsoluteTime(); timestamp - start < period;)
        {
            for (auto client : _clients)
            {
                client->process(timestamp);
            }
            pacer.tick(utils::Time::getAbsoluteTime());
            utils::Time::nanoSleep(pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
            timestamp = utils::Time::getAbsoluteTime();
        }
    }

    bool awaitPendingJobs(uint64_t timeout)
    {
        auto start = utils::Time::getAbsoluteTime();
        for (size_t runCount = 1; utils::Time::getAbsoluteTime() - start < timeout;)
        {
            runCount = 0;
            utils::Time::nanoSleep(utils::Time::ms * 100);
            for (auto client : _clients)
            {
                if (client->_transport->hasPendingJobs())
                {
                    ++runCount;
                }
            }
            if (runCount == 0)
            {
                return true;
            }
        }
        return false;
    }

    std::vector<T*> _clients;
};
