#pragma once

#include "test/integration/emulator/Conference.h"

namespace emulator
{
template <typename TClient>
class GroupCall
{
public:
    GroupCall(emulator::HttpdFactory* httpd,
        uint32_t& idCounter,
        memory::PacketPoolAllocator& allocator,
        memory::AudioPacketPoolAllocator& audioAllocator,
        transport::TransportFactory& transportFactory,
        transport::TransportFactory& publicTransportFactory,
        transport::SslDtls& sslDtls,
        uint32_t callCount)
        : _idCounter(idCounter),
          _allocator(allocator),
          _audioAllocator(audioAllocator),
          _transportFactory(transportFactory),
          _publicTransportFactory(publicTransportFactory),
          _sslDtls(sslDtls)
    {
        for (uint32_t i = 0; i < callCount; ++i)
        {
            add(httpd);
        }
    }

    bool connectAll(uint64_t timeout)
    {
        auto start = utils::Time::getAbsoluteTime();
        for (auto& client : clients)
        {
            if (!client->_channel.isSuccess())
            {
                logger::warn("client has not received offer yet", client->getLoggableId().c_str());
                return false;
            }
        }

        for (auto& client : clients)
        {
            if (client->hasProcessedOffer())
            {
                continue; // already connected
            }

            client->processOffer();
            if (!client->hasProcessedOffer())
            {
                logger::warn("client did not parse offer successfully transport %s, audio source %s",
                    client->getLoggableId().c_str(),
                    (client->_bundleTransport || client->_audioTransport) ? "ok" : "bad",
                    client->_audioSource ? "ok" : "bad");
                return false;
            }
        }

        for (auto& client : clients)
        {
            client->connect();
        }

        logger::PruneSpam logPrune(1, 10);
        auto currTime = utils::Time::getAbsoluteTime();
        while (currTime - start < timeout)
        {
            auto it = std::find_if_not(clients.begin(), clients.end(), [](auto& c) { return c->isConnected(); });

            if (it == clients.end())
            {
                logger::info("all clients connected", "test");
                return true;
            }

            utils::Time::nanoSleep(10 * utils::Time::ms);
            if (logPrune.canLog())
            {
                logger::debug("waiting for connect...", "test");
            }
            currTime = utils::Time::getAbsoluteTime();
        }

        logger::warn("client transports failed to connect", "GroupCall.connectAll");
        return false;
    }

    bool connectSingle(uint32_t clientIndex, uint64_t timeout)
    {
        auto start = utils::Time::getAbsoluteTime();
        auto& client = clients[clientIndex];
        if (!client->_channel.isSuccess())
        {
            return false;
        }

        if (client->hasTransport())
        {
            return client->isConnected();
        }

        client->processOffer();
        if (!client->hasProcessedOffer())
        {
            return false;
        }

        for (auto& client : clients)
        {
            client->connect();
        }

        auto currTime = utils::Time::getAbsoluteTime();
        while (currTime - start < timeout)
        {
            if (client->isConnected())
            {
                return true;
            }

            utils::Time::nanoSleep(10 * utils::Time::ms);
            logger::debug("waiting for connect...", "test");
            currTime = utils::Time::getAbsoluteTime();
        }

        return false;
    }

    void run(uint64_t period, const size_t modulateVolumeForNSpeakers = 0)
    {
        const auto start = utils::Time::getAbsoluteTime();
        const size_t modulateFirstNSpeakers = std::min(modulateVolumeForNSpeakers, clients.size());
        utils::Pacer pacer(10 * utils::Time::ms);
        for (auto timestamp = utils::Time::getAbsoluteTime(); timestamp - start < period;)
        {
            for (auto& client : clients)
            {
                client->process(timestamp);
            }
            pacer.tick(utils::Time::getAbsoluteTime());
            utils::Time::nanoSleep(pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
            timestamp = utils::Time::getAbsoluteTime();

            if (modulateVolumeForNSpeakers > 0)
            {
                const size_t inveer_volume_window = 2;
                auto window = (utils::Time::diff(start, timestamp) / utils::Time::sec) % 10;
                for (size_t i = 0; i < modulateFirstNSpeakers; i++)
                {
                    auto volume = window < inveer_volume_window ? 0.4 : 0.6;
                    clients[i]->_audioSource->setVolume(volume);
                }
            }
        }
        logger::debug("exit run period", "SfuClient");
    }

    bool awaitPendingJobs(uint64_t timeout)
    {
        auto start = utils::Time::getAbsoluteTime();
        for (size_t runCount = 1; utils::Time::getAbsoluteTime() - start < timeout;)
        {
            runCount = 0;
            utils::Time::nanoSleep(utils::Time::ms * 100);
            for (auto& client : clients)
            {
                if (client->hasPendingJobs())
                {
                    ++runCount;
                }
            }
            if (runCount == 0)
            {
                return true;
            }
        }
        logger::warn("pending jobs not completed", "GroupCall");
        return false;
    }

    static bool startConference(Conference& conf, std::string url, bool useGlobalPort = true)
    {
        conf.create(url, useGlobalPort);
        auto result = conf.isSuccess();
        utils::Time::nanoSleep(1 * utils::Time::sec);
        return result;
    }

    void add(emulator::HttpdFactory* httpd)
    {
        clients.emplace_back(std::make_unique<TClient>(httpd,
            ++_idCounter,
            _allocator,
            _audioAllocator,
            _transportFactory,
            _publicTransportFactory,
            _sslDtls));
    }

    void disconnectClients()
    {
        for (auto& client : clients)
        {
            client->disconnect();
        }
    }

    void stopTransports()
    {
        for (auto& client : clients)
        {
            client->stopTransports();
        }
    }

    std::vector<std::unique_ptr<TClient>> clients;

private:
    uint32_t& _idCounter;
    memory::PacketPoolAllocator& _allocator;
    memory::AudioPacketPoolAllocator& _audioAllocator;
    transport::TransportFactory& _transportFactory;
    transport::TransportFactory& _publicTransportFactory;
    transport::SslDtls& _sslDtls;
    std::string _baseUrl;
};

} // namespace emulator
