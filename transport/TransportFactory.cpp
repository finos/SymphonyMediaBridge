#include "transport/TransportFactory.h"
#include "concurrency/MpmcHashmap.h"
#include "config/Config.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/RecordingTransport.h"
#include "transport/RtcTransport.h"
#include "transport/TcpEndpoint.h"
#include "transport/TcpServerEndpoint.h"
#include "transport/UdpEndpoint.h"
#include "utils/MersienneRandom.h"

namespace transport
{

class TransportFactoryImpl final : public TransportFactory,
                                   public ServerEndpoint::IEvents,
                                   public TcpEndpointFactory,
                                   public Endpoint::IEvents
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
        memory::PacketPoolAllocator& mainAllocator)
        : _deleter(this),
          _jobManager(jobManager),
          _garbageQueue(_jobManager),
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
          _callbackRefCount(0),
          _sharedRecordingEndpointListIndex(0),
          _good(true)
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
                        new UdpEndpoint(jobManager, 1024, _mainAllocator, portAddress, _rtcePoll, true),
                        getDeleter());

                    if (endPoint->isGood())
                    {
                        endPoint->registerDefaultListener(this);
                        if (!endPoint->configureBufferSizes(2 * 1024 * 1024, receiveBufferSize))
                        {
                            logger::error("failed to set socket send buffer %d", "TransportFactory", errno);
                        }
                        logger::info("opened main media port at %s",
                            "TransportFactory",
                            portAddress.toString().c_str());
                        _sharedEndpoints[portOffset].push_back(endPoint);
                        endPoint->start();
                    }
                    else
                    {
                        _good = false;
                        logger::error("failed to open main media port on interface %s",
                            "TransportFactory",
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
                auto endPoint = std::shared_ptr<TcpServerEndpoint>(new TcpServerEndpoint(_jobManager,
                                                                       _mainAllocator,
                                                                       _rtcePoll,
                                                                       1024,
                                                                       this,
                                                                       *this,
                                                                       portAddress,
                                                                       config),
                    getDeleter());

                if (endPoint->isGood())
                {
                    // no need to set buffers as we will not send on this socket
                    logger::info("opened main TCP server port at %s",
                        "TransportFactory",
                        portAddress.toString().c_str());
                    _tcpServerEndpoints.push_back(endPoint);
                    endPoint->start();
                }
                else
                {
                    _good = false;
                    logger::error("failed to open TCP server port at %s",
                        "TransportFactory",
                        portAddress.toString().c_str());
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
                        new RecordingEndpoint(jobManager, 1024, _mainAllocator, portAddress, _rtcePoll, true),
                        getDeleter());

                    if (endPoint->isGood())
                    {
                        endPoint->registerDefaultListener(this);
                        if (!endPoint->configureBufferSizes(2 * 1024 * 1024, receiveBufferSize))
                        {
                            logger::error("failed to set socket send buffer %d", "TransportFactory", errno);
                        }
                        logger::info("opened recording port at %s", "TransportFactory", portAddress.toString().c_str());
                        _sharedRecordingEndpoints[portOffset].push_back(endPoint);
                        endPoint->start();
                    }
                    else
                    {
                        _good = false;
                        logger::error("failed to open recording port on interface %s",
                            "TransportFactory",
                            portAddress.toString().c_str());
                    }
                }
            }
        }
    }

    ~TransportFactoryImpl()
    {
        _tcpServerEndpoints.clear();
        _sharedEndpoints.clear();
        _sharedRecordingEndpoints.clear();
        while (_callbackRefCount > 0)
        {
            std::this_thread::yield();
        }
    }

    bool openPorts(const SocketAddress& ip, Endpoints& rtpPorts, Endpoints& rtcpPorts) const
    {
        const auto portRange = std::make_pair(_config.ice.udpPortRangeLow, _config.ice.udpPortRangeHigh);
        const int portCount = (portRange.second - portRange.first + 1);
        const int offset = _randomGenerator.next() % portCount;

        const int firstPort = (portRange.first + (offset % portCount)) & 0xFFFEu;
        auto rtpEndpoint = std::shared_ptr<UdpEndpoint>(
            new UdpEndpoint(_jobManager, 32, _mainAllocator, SocketAddress(ip, firstPort), _rtcePoll, false),
            getDeleter());

        auto rtcpEndpoint = std::shared_ptr<UdpEndpoint>(
            new UdpEndpoint(_jobManager, 32, _mainAllocator, SocketAddress(ip, firstPort + 1), _rtcePoll, false),
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
            logger::error("Failed to create udp end points in port range", "TransportFactory");
            return false;
        }

        if (!rtpEndpoint->configureBufferSizes(512 * 1024, 5 * 1024 * 1024) ||
            !rtcpEndpoint->configureBufferSizes(512 * 1024, 512 * 1024))
        {
            logger::error("failed to set socket send buffer %d", "TransportFactory", errno);
            return false;
        }

        return true;
    }

    bool openPorts(const SocketAddress& ip, Endpoints& rtpPorts) const
    {
        auto portRange = std::make_pair(_config.ice.udpPortRangeLow, _config.ice.udpPortRangeHigh);
        const int portCount = (portRange.second - portRange.first + 1);
        const int offset = _randomGenerator.next() % portCount;

        const int firstPort = (portRange.first + (offset % portCount)) & 0xFFFEu;
        auto rtpEndpoint = std::shared_ptr<UdpEndpoint>(
            new UdpEndpoint(_jobManager, 32, _mainAllocator, SocketAddress(ip, firstPort), _rtcePoll, false),
            getDeleter());

        for (int i = 2; i < portCount && !rtpEndpoint->isGood(); i += 2)
        {
            const uint16_t port = (portRange.first + ((offset + i) % portCount)) & 0xFFFEu;
            rtpEndpoint->openPort(port);
        }

        if (rtpEndpoint->isGood())
        {
            rtpPorts.emplace_back(rtpEndpoint);
        }
        else
        {
            logger::error("Failed to create udp end point in port range", "TransportFactory");
            return false;
        }

        if (!rtpEndpoint->configureBufferSizes(512 * 1024, 5 * 1024 * 1024))
        {
            logger::error("failed to set socket send buffer %d", "TransportFactory", errno);
            return false;
        }

        return true;
    }

    std::shared_ptr<Endpoint> createTcpEndpoint(const transport::SocketAddress& baseAddress) override
    {
        return std::shared_ptr<TcpEndpoint>(new TcpEndpoint(_jobManager, _mainAllocator, baseAddress, _rtcePoll),
            getDeleter());
    }

    std::shared_ptr<Endpoint> createTcpEndpoint(int fd,
        const transport::SocketAddress& localPort,
        const transport::SocketAddress& peerPort) override
    {
        auto endpoint = std::shared_ptr<TcpEndpoint>(
            new TcpEndpoint(_jobManager, _mainAllocator, _rtcePoll, fd, localPort, peerPort),
            getDeleter());

        // if read event is fired now we may miss it and it is edge triggered.
        // everything relies on that the ice session will want to respond to the request
        _rtcePoll.add(fd, endpoint.get());
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

    virtual std::shared_ptr<RtcTransport> createOnPrivatePort(const ice::IceRole iceRole,
        const size_t sendPoolSize,
        const size_t endpointId) override
    {
        Endpoints rtpPorts;
        if (openPorts(_interfaces.front(), rtpPorts))
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
                _mainAllocator);
        }

        return nullptr;
    }

    std::shared_ptr<RtcTransport> createOnPorts(const ice::IceRole iceRole,
        const size_t sendPoolSize,
        const size_t endpointId,
        const Endpoints& rtpPorts) override
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
            _mainAllocator);
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
            _mainAllocator);
    }

    std::shared_ptr<RtcTransport> create(const size_t sendPoolSize, const size_t endpointId) override
    {
        Endpoints rtpPorts;
        Endpoints rtcpPorts;
        if (openPorts(_interfaces.front(), rtpPorts, rtcpPorts))
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

        logger::error("No shared recording endpoints configured", "TransportFactory");
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
        logger::debug("closing %s", "TransportFactory", endpoint->getName());
        _garbageQueue.addJob<DeleteJob<Endpoint>>(endpoint, _callbackRefCount);
    }

    void shutdownEndpoint(ServerEndpoint* endpoint)
    {
        logger::debug("closing %s", "TransportFactory", endpoint->getName());
        _garbageQueue.addJob<DeleteJob<ServerEndpoint>>(endpoint, _callbackRefCount);
    }

private:
    template <typename T>
    class DeleteJob : public jobmanager::CountedJob
    {
    public:
        DeleteJob(T* endpoint, std::atomic_uint32_t& counter) : CountedJob(counter), _endpoint(endpoint) {}

        void run() override
        {
            _endpoint->closePort();
            while (_endpoint->getState() != Endpoint::State::CLOSED)
            {
                std::this_thread::yield();
            }
            delete _endpoint;
        }

    private:
        T* _endpoint;
    };

    void onServerPortClosed(ServerEndpoint& endpoint) override
    {
        logger::info("TCP server port %s closed.", "TransportFactory", endpoint.getName());
    }

    void onPortClosed(Endpoint& endpoint) override
    {
        logger::debug("deleting %s", "TransportFactory", endpoint.getName());
    }

    void onRtpReceived(Endpoint& endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket packet) override
    {
    }

    void onDtlsReceived(Endpoint& endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket packet) override
    {
    }

    void onRtcpReceived(Endpoint& endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket packet) override
    {
    }

    void onIceReceived(Endpoint& endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket packet) override
    {
    }

    void onIceTcpConnect(std::shared_ptr<Endpoint> endpoint,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket packet) override
    {
    }

    void onRegistered(Endpoint& endpoint) override {}
    void onUnregistered(Endpoint& endpoint) override {}
    void onServerPortRegistered(ServerEndpoint& endpoint) override {}
    void onServerPortUnregistered(ServerEndpoint& endpoint) override {}

    jobmanager::JobManager& _jobManager;
    jobmanager::JobQueue _garbageQueue;
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
    std::atomic_uint32_t _callbackRefCount;

    std::vector<std::vector<std::shared_ptr<RecordingEndpoint>>> _sharedRecordingEndpoints;
    std::atomic_uint32_t _sharedRecordingEndpointListIndex;
    bool _good;
};

std::unique_ptr<TransportFactory> createTransportFactory(jobmanager::JobManager& jobManager,
    SrtpClientFactory& srtpClientFactory,
    const config::Config& config,
    const sctp::SctpConfig& sctpConfig,
    const ice::IceConfig& iceConfig,
    const bwe::Config& bweConfig,
    const bwe::RateControllerConfig& rateControllerConfig,
    const std::vector<SocketAddress>& interfaces,
    transport::RtcePoll& rtcePoll,
    memory::PacketPoolAllocator& mainAllocator)
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
        mainAllocator);
}

} // namespace transport
