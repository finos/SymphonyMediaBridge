#include "transport/TransportFactory.h"
#include "concurrency/MpmcHashmap.h"
#include "config/Config.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/RecordingTransport.h"
#include "transport/RtcTransport.h"
#include "transport/TcpEndpoint.h"
#include "transport/TcpServerEndpoint.h"
#include "transport/UdpEndpointImpl.h"
#include "utils/MersienneRandom.h"

namespace transport
{

class TransportFactoryImpl final : public TransportFactory,
                                   public TcpEndpointFactory,
                                   public Endpoint::IStopEvents,
                                   public ServerEndpoint::IStopEvents
{
    class EndpointDeleter
    {
    public:
        EndpointDeleter(TransportFactoryImpl* factory) : _allocator(factory) {}

        template <typename T>
        void operator()(T* r)
        {
            assert(_allocator);
            if (_allocator)
            {
                _allocator->shutdownEndpoint(r);
            }
        }

        TransportFactoryImpl* _allocator;
    };

    EndpointDeleter getDeleter() const { return _deleter; }
    EndpointDeleter _deleter;

public:
    TransportFactoryImpl(jobmanager::JobManager& jobManager,
        SrtpClientFactory& srtpClientFactory,
        const config::Config& config,
        const sctp::SctpConfig& sctpConfig,
        const ice::IceConfig& iceConfig,
        const bwe::Config& bweConfig,
        const bwe::RateControllerConfig& rateControllerConfig,
        const std::vector<SocketAddress>& interfaces,
        transport::RtcePoll& rtcePoll,
        memory::PacketPoolAllocator& mainAllocator,
        std::shared_ptr<transport::EndpointFactory>& endpointFactory)
        : _deleter(this),
          _jobManager(jobManager),
          _srtpClientFactory(srtpClientFactory),
          _config(config),
          _sctpConfig(sctpConfig),
          _iceConfig(iceConfig),
          _bweConfig(bweConfig),
          _rateControllerConfig(rateControllerConfig),
          _interfaces(interfaces),
          _rtcePoll(rtcePoll),
          _sharedEndpointListIndex(0),
          _mainAllocator(mainAllocator),
          _pendingTasks(0),
          _sharedRecordingEndpointListIndex(0),
          _good(true),
          _endpointFactory(endpointFactory),
          _garbageQueue(jobManager)
    {
#ifdef __APPLE__
        const size_t receiveBufferSize = 5 * 1024 * 1024;
#else
        const size_t receiveBufferSize = 40 * 1024 * 1024;
#endif
        if (config.ice.singlePort != 0)
        {
            for (uint32_t portOffset = 0; portOffset < std::max(1u, config.ice.sharedPorts.get()); ++portOffset)
            {
                _sharedEndpoints.push_back(Endpoints());
                for (SocketAddress portAddress : interfaces)
                {
                    portAddress.setPort(config.ice.singlePort + portOffset);
                    auto endPoint = std::shared_ptr<UdpEndpoint>(
                        _endpointFactory
                            ->createUdpEndpoint(jobManager, 1024, _mainAllocator, portAddress, _rtcePoll, true),
                        getDeleter());

                    if (endPoint->isGood())
                    {
                        if (!endPoint->configureBufferSizes(2 * 1024 * 1024, receiveBufferSize))
                        {
                            logger::error("failed to set socket send buffer %d", _name, errno);
                        }
                        logger::info("opened main media port at %s", _name, portAddress.toString().c_str());
                        _sharedEndpoints[portOffset].push_back(endPoint);
                        endPoint->start();
                    }
                    else
                    {
                        _good = false;
                        logger::error("failed to open main media port on interface %s",
                            _name,
                            portAddress.toString().c_str());
                    }
                }
            }
        }
        if (config.ice.tcp.enable)
        {
            for (SocketAddress portAddress : interfaces)
            {
                portAddress.setPort(config.ice.tcp.port);
                auto endPoint = std::shared_ptr<ServerEndpoint>(_endpointFactory->createTcpServerEndpoint(_jobManager,
                                                                    _mainAllocator,
                                                                    _rtcePoll,
                                                                    1024,
                                                                    this,
                                                                    portAddress,
                                                                    config),
                    getDeleter());

                if (endPoint->isGood())
                {
                    // no need to set buffers as we will not send on this socket
                    logger::info("opened main TCP server port at %s", _name, portAddress.toString().c_str());
                    _tcpServerEndpoints.push_back(endPoint);
                    endPoint->start();
                }
                else
                {
                    _good = false;
                    logger::error("failed to open TCP server port at %s", _name, portAddress.toString().c_str());
                }
            }
        }
        if (config.recording.singlePort != 0)
        {
            for (uint32_t portOffset = 0; portOffset < std::max(1u, config.recording.sharedPorts.get()); ++portOffset)
            {
                _sharedRecordingEndpoints.emplace_back(std::vector<std::shared_ptr<RecordingEndpoint>>());
                for (SocketAddress portAddress : interfaces)
                {
                    portAddress.setPort(config.recording.singlePort + portOffset);
                    auto endPoint = std::shared_ptr<RecordingEndpoint>(
                        _endpointFactory
                            ->createRecordingEndpoint(jobManager, 1024, _mainAllocator, portAddress, _rtcePoll, true),
                        getDeleter());
                    if (!endPoint)
                    {
                        logger::warn("failed to create recording endpoint for interface %s",
                            _name,
                            portAddress.toString().c_str());
                        continue;
                    }

                    if (endPoint->isGood())
                    {
                        if (!endPoint->configureBufferSizes(2 * 1024 * 1024, receiveBufferSize))
                        {
                            logger::error("failed to set socket send buffer %d", _name, errno);
                        }
                        logger::info("opened recording port at %s", _name, portAddress.toString().c_str());
                        _sharedRecordingEndpoints[portOffset].push_back(endPoint);
                        endPoint->start();
                    }
                    else
                    {
                        _good = false;
                        logger::error("failed to open recording port on interface %s",
                            _name,
                            portAddress.toString().c_str());
                    }
                }
            }
        }
    }

    ~TransportFactoryImpl()
    {
        if (!_tcpServerEndpoints.empty() || !_sharedEndpoints.empty() || !_sharedRecordingEndpoints.empty())
        {
            assert(_rtcePoll.isRunning());
        }

        _tcpServerEndpoints.clear();
        _sharedEndpoints.clear();
        _sharedRecordingEndpoints.clear();
        auto start = utils::Time::getAbsoluteTime();
        while ((_pendingTasks > 0 || _garbageQueue.getCount() > 0) &&
            utils::Time::diffLE(start, utils::Time::getAbsoluteTime(), utils::Time::sec * 10))
        {
            std::this_thread::yield();
        }
        if (_pendingTasks > 0)
        {
            assert(false);
            logger::warn("not all endpoints were properly deleted", _name);
        }
    }

    bool openPorts(const SocketAddress& ip, Endpoints& rtpPorts, Endpoints& rtcpPorts, uint32_t maxSessions) const
    {
        const auto portRange = std::make_pair(_config.ice.udpPortRangeLow, _config.ice.udpPortRangeHigh);
        const int portCount = (portRange.second - portRange.first + 1);
        const int offset = _randomGenerator.next() % portCount;

        const int firstPort = (portRange.first + (offset % portCount)) & 0xFFFEu;
        auto rtpEndpoint = std::shared_ptr<UdpEndpoint>(_endpointFactory->createUdpEndpoint(_jobManager,
                                                            maxSessions,
                                                            _mainAllocator,
                                                            SocketAddress(ip, firstPort),
                                                            _rtcePoll,
                                                            false),
            getDeleter());

        auto rtcpEndpoint = std::shared_ptr<UdpEndpoint>(_endpointFactory->createUdpEndpoint(_jobManager,
                                                             maxSessions,
                                                             _mainAllocator,
                                                             SocketAddress(ip, firstPort + 1),
                                                             _rtcePoll,
                                                             false),
            getDeleter());

        for (int i = 2; i < portCount && (!rtpEndpoint->isGood() || !rtcpEndpoint->isGood()); i += 2)
        {
            const uint16_t port = (portRange.first + ((offset + i) % portCount)) & 0xFFFEu;
            rtpEndpoint->openPort(port);
            rtcpEndpoint->openPort(port + 1);
        }

        if (rtpEndpoint->isGood() && rtcpEndpoint->isGood())
        {
            rtpPorts.emplace_back(rtpEndpoint);
            rtcpPorts.emplace_back(rtcpEndpoint);
        }
        else
        {
            logger::error("Failed to create udp end points in port range", _name);
            return false;
        }

        if (!rtpEndpoint->configureBufferSizes(512 * 1024, 5 * 1024 * 1024) ||
            !rtcpEndpoint->configureBufferSizes(512 * 1024, 512 * 1024))
        {
            logger::error("failed to set socket send buffer %d", _name, errno);
            return false;
        }

        return true;
    }

    bool openPorts(const SocketAddress& ip, Endpoints& rtpPorts, uint32_t maxSessions) const
    {
        auto portRange = std::make_pair(_config.ice.udpPortRangeLow, _config.ice.udpPortRangeHigh);
        const int portCount = (portRange.second - portRange.first + 1);
        const int offset = _randomGenerator.next() % portCount;

        const int firstPort = (portRange.first + (offset % portCount)) & 0xFFFEu;
        auto rtpEndpoint = std::shared_ptr<UdpEndpoint>(_endpointFactory->createUdpEndpoint(_jobManager,
                                                            maxSessions,
                                                            _mainAllocator,
                                                            SocketAddress(ip, firstPort),
                                                            _rtcePoll,
                                                            false),
            getDeleter());

        for (int i = 2; i < portCount && !rtpEndpoint->isGood(); i += 2)
        {
            const uint16_t port = (portRange.first + ((offset + i) % portCount)) & 0xFFFEu;
            rtpEndpoint->openPort(port);
        }

        if (rtpEndpoint->isGood())
        {
            rtpPorts.emplace_back(rtpEndpoint);
            logger::info("opened rtp port %s", _name, rtpEndpoint->getLocalPort().toString().c_str());
        }
        else
        {
            logger::error("Failed to create udp end point in port range", _name);
            return false;
        }

        if (!rtpEndpoint->configureBufferSizes(512 * 1024, 5 * 1024 * 1024))
        {
            logger::error("failed to set socket send buffer %d", _name, errno);
            return false;
        }

        return true;
    }

    bool openRtpMuxPorts(Endpoints& rtpPorts, uint32_t maxSessions) const override
    {
        for (auto nic : _interfaces)
        {
            if (!openPorts(nic, rtpPorts, maxSessions))
            {
                return false;
            }
        }
        return true;
    }

    std::shared_ptr<Endpoint> createTcpEndpoint(const transport::SocketAddress& baseAddress) override
    {
        return std::shared_ptr<TcpEndpoint>(
            _endpointFactory->createTcpEndpoint(_jobManager, _mainAllocator, baseAddress, _rtcePoll),
            getDeleter());
    }

    std::shared_ptr<Endpoint> createTcpEndpoint(int fd,
        const transport::SocketAddress& localPort,
        const transport::SocketAddress& peerPort) override
    {
        auto endpoint = std::shared_ptr<TcpEndpoint>(
            _endpointFactory->createTcpEndpoint(_jobManager, _mainAllocator, _rtcePoll, fd, localPort, peerPort),
            getDeleter());

        return endpoint;
    }

    virtual Endpoints createTcpEndpoints(int ipFamily) override
    {
        Endpoints endPoints;
        for (auto& nic : _interfaces)
        {
            if (ipFamily == nic.getFamily())
            {
                endPoints.push_back(createTcpEndpoint(nic));
            }
        }
        return endPoints;
    }

    std::shared_ptr<RtcTransport> create(const ice::IceRole iceRole,
        const size_t sendPoolSize,
        const size_t endpointId) override
    {
        if (!_sharedEndpoints.empty())
        {
            return createOnSharedPort(iceRole, sendPoolSize, endpointId);
        }

        return createOnPrivatePort(iceRole, sendPoolSize, endpointId);
    }

    std::shared_ptr<RtcTransport> createOnPrivatePort(const ice::IceRole iceRole,
        const size_t sendPoolSize,
        const size_t endpointId) override
    {
        Endpoints rtpPorts;
        for (auto& interface : _interfaces)
        {
            openPorts(interface, rtpPorts, 8);
        }

        if (!rtpPorts.empty())
        {
            return transport::createTransport(_jobManager,
                _srtpClientFactory,
                endpointId,
                _config,
                _sctpConfig,
                _iceConfig,
                iceRole,
                _bweConfig,
                _rateControllerConfig,
                rtpPorts,
                _tcpServerEndpoints,
                this,
                _mainAllocator,
                16,
                256,
                false,
                false);
        }
        return nullptr;
    }

    std::shared_ptr<RtcTransport> createOnPorts(const ice::IceRole iceRole,
        const size_t sendPoolSize,
        const size_t endpointId,
        const Endpoints& rtpPorts,
        size_t expectedInboundStreamCount,
        size_t expectedOutboundStreamCount,
        bool enableUplinkEstimation,
        bool enableDownlinkEstimation) override
    {
        return transport::createTransport(_jobManager,
            _srtpClientFactory,
            endpointId,
            _config,
            _sctpConfig,
            _iceConfig,
            iceRole,
            _bweConfig,
            _rateControllerConfig,
            rtpPorts,
            _tcpServerEndpoints,
            this,
            _mainAllocator,
            expectedInboundStreamCount,
            expectedOutboundStreamCount,
            enableUplinkEstimation,
            enableDownlinkEstimation);
    }

    std::shared_ptr<RtcTransport> createOnSharedPort(const ice::IceRole iceRole,
        const size_t sendPoolSize,
        const size_t endpointId) override
    {
        const uint32_t index = _sharedEndpointListIndex.fetch_add(1) % _sharedEndpoints.size();
        return transport::createTransport(_jobManager,
            _srtpClientFactory,
            endpointId,
            _config,
            _sctpConfig,
            _iceConfig,
            iceRole,
            _bweConfig,
            _rateControllerConfig,
            _sharedEndpoints[index],
            _tcpServerEndpoints,
            this,
            _mainAllocator,
            16,
            256,
            true,
            true);
    }

    std::shared_ptr<RtcTransport> create(const size_t sendPoolSize, const size_t endpointId) override
    {
        Endpoints rtpPorts;
        Endpoints rtcpPorts;
        if (openPorts(_interfaces.front(), rtpPorts, rtcpPorts, 8))
        {
            return transport::createTransport(_jobManager,
                _srtpClientFactory,
                endpointId,
                _config,
                _sctpConfig,
                _bweConfig,
                _rateControllerConfig,
                rtpPorts,
                rtcpPorts,
                _mainAllocator);
        }

        return nullptr;
    }

    std::unique_ptr<RecordingTransport> createForRecording(const size_t endpointHashId,
        const size_t streamHashId,
        const SocketAddress& peer,
        const uint8_t aesKey[32],
        const uint8_t salt[12]) override
    {
        if (!_sharedRecordingEndpoints.empty())
        {
            const uint32_t initialIndex =
                _sharedRecordingEndpointListIndex.fetch_add(1) % _sharedRecordingEndpoints.size();
            uint32_t listIndex = initialIndex;
            do
            {
                for (size_t endpointIndex = 0; endpointIndex < _sharedRecordingEndpoints[listIndex].size();
                     ++endpointIndex)
                {
                    auto endpoint = _sharedRecordingEndpoints[listIndex][endpointIndex];
                    if (endpoint->getLocalPort().getFamily() == peer.getFamily())
                    {
                        return createRecordingTransport(_jobManager,
                            _config,
                            endpoint,
                            endpointHashId,
                            streamHashId,
                            peer,
                            aesKey,
                            salt,
                            _mainAllocator);
                    }
                }

                listIndex = (listIndex + 1) % _sharedRecordingEndpoints.size();

            } while (listIndex != initialIndex);
        }

        logger::error("No shared recording endpoints configured", _name);
        return nullptr;
    }

    EndpointMetrics getSharedUdpEndpointsMetrics() const override
    {
        const auto timestamp = utils::Time::getAbsoluteTime();
        EndpointMetrics metrics;
        for (auto& endpoints : _sharedEndpoints)
        {
            for (auto& endpoint : endpoints)
            {
                metrics += endpoint->getMetrics(timestamp);
            }
        }

        return metrics;
    }

    bool isGood() const override { return _good; }

    void maintenance(uint64_t timestamp) override
    {
        for (auto& endpoint : _tcpServerEndpoints)
        {
            endpoint->maintenance(timestamp);
        }
    }

    void shutdownEndpoint(Endpoint* endpoint)
    {
        if (!endpoint)
        {
            return;
        }

        logger::debug("closing %s", _name, endpoint->getName());
        if (endpoint->getState() == Endpoint::State::CREATED)
        {
            // When transport is CREATED we can delete it right now without call stop
            enqueueDeleteJobNow(endpoint);
        }
        else
        {
            ++_pendingTasks;
            endpoint->stop(this);
        }
    }

    void shutdownEndpoint(ServerEndpoint* endpoint)
    {
        if (!endpoint)
        {
            return;
        }

        logger::debug("closing %s", _name, endpoint->getName());
        ++_pendingTasks;
        endpoint->stop(this);
    }

    void registerIceListener(Endpoint::IEvents& listener, const std::string& ufrag) override
    {
        if (_sharedEndpoints.empty())
        {
            return;
        }

        for (auto& endpoint : _sharedEndpoints[0])
        {
            endpoint->registerListener(ufrag, &listener);
        }
    }

    void registerIceListener(ServerEndpoint::IEvents& listener, const std::string& ufrag) override
    {
        for (auto& endpoint : _tcpServerEndpoints)
        {
            endpoint->registerListener(ufrag, &listener);
        }
    }

    void unregisterIceListener(Endpoint::IEvents& listener, const std::string& ufrag) override
    {
        for (auto& endpoints : _sharedEndpoints)
        {
            for (auto& endpoint : endpoints)
            {
                endpoint->unregisterListener(&listener);
            }
        }
    }

    void unregisterIceListener(ServerEndpoint::IEvents& listener, const std::string& ufrag) override
    {
        for (auto& endpoint : _tcpServerEndpoints)
        {
            endpoint->unregisterListener(ufrag, &listener);
        }
    }

private:
    template <typename T>
    class DeleteJob : public jobmanager::CountedJob
    {
    public:
        DeleteJob(T* endpoint, std::atomic_uint32_t& counter) : CountedJob(counter), _endpoint(endpoint) {}

        void run() override { delete _endpoint; }

    private:
        T* _endpoint;
    };

    void onEndpointStopped(ServerEndpoint* endpoint) override
    {
        logger::info("%s stopped.", _name, endpoint->getName());
        _garbageQueue.addJob<DeleteJob<ServerEndpoint>>(endpoint, _pendingTasks);
        --_pendingTasks; // epoll stop is complete
    }

    void onEndpointStopped(Endpoint* endpoint) override
    {
        logger::info("%s stopped. %u", _name, endpoint->getName(), _pendingTasks.load());
        _garbageQueue.addJob<DeleteJob<Endpoint>>(endpoint, _pendingTasks);
        --_pendingTasks; // epoll stop is complete
    }

    void enqueueDeleteJobNow(Endpoint* endpoint) { _garbageQueue.addJob<DeleteJob<Endpoint>>(endpoint, _pendingTasks); }

    jobmanager::JobManager& _jobManager;
    SrtpClientFactory& _srtpClientFactory;
    const config::Config& _config;
    const sctp::SctpConfig& _sctpConfig;
    const ice::IceConfig& _iceConfig;
    const bwe::Config& _bweConfig;
    const bwe::RateControllerConfig& _rateControllerConfig;
    const std::vector<SocketAddress> _interfaces;
    transport::RtcePoll& _rtcePoll;
    std::vector<Endpoints> _sharedEndpoints;
    std::atomic_uint32_t _sharedEndpointListIndex;
    ServerEndpoints _tcpServerEndpoints;
    memory::PacketPoolAllocator& _mainAllocator;
    mutable utils::MersienneRandom<uint32_t> _randomGenerator;
    std::atomic_uint32_t _pendingTasks;

    std::vector<std::vector<std::shared_ptr<RecordingEndpoint>>> _sharedRecordingEndpoints;
    std::atomic_uint32_t _sharedRecordingEndpointListIndex;
    bool _good;
    std::shared_ptr<transport::EndpointFactory> _endpointFactory;
    static const char* _name;
    jobmanager::JobQueue _garbageQueue; // must be last
};

const char* TransportFactoryImpl::_name = "TransportFactory";

std::unique_ptr<TransportFactory> createTransportFactory(jobmanager::JobManager& jobManager,
    SrtpClientFactory& srtpClientFactory,
    const config::Config& config,
    const sctp::SctpConfig& sctpConfig,
    const ice::IceConfig& iceConfig,
    const bwe::Config& bweConfig,
    const bwe::RateControllerConfig& rateControllerConfig,
    const std::vector<SocketAddress>& interfaces,
    transport::RtcePoll& rtcePoll,
    memory::PacketPoolAllocator& mainAllocator,
    std::shared_ptr<EndpointFactory> endpointFactory)
{
    return std::make_unique<TransportFactoryImpl>(jobManager,
        srtpClientFactory,
        config,
        sctpConfig,
        iceConfig,
        bweConfig,
        rateControllerConfig,
        interfaces,
        rtcePoll,
        mainAllocator,
        endpointFactory);
}

} // namespace transport
