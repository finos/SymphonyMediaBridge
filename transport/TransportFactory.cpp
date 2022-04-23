#include "transport/TransportFactory.h"
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
        : _jobManager(jobManager),
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
                _sharedEndpoints.push_back(std::vector<Endpoint*>());
                for (SocketAddress portAddress : interfaces)
                {
                    portAddress.setPort(config.ice.singlePort + portOffset);
                    auto endPoint = new UdpEndpoint(jobManager, 1024, _mainAllocator, portAddress, _rtcePoll, true);

                    if (endPoint->isGood())
                    {
                        endPoint->registerDefaultListener(this);
                        ++_callbackRefCount;
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
                        delete endPoint;
                    }
                }
            }
        }
        if (config.ice.tcp.enable)
        {
            for (SocketAddress portAddress : interfaces)
            {
                portAddress.setPort(config.ice.tcp.port);
                auto endPoint =
                    new TcpServerEndpoint(_jobManager, _mainAllocator, _rtcePoll, 1024, this, portAddress, config);
                if (endPoint->isGood())
                {
                    ++_callbackRefCount;
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
                    delete endPoint;
                }
            }
        }
        if (config.recording.singlePort != 0)
        {
            for (uint32_t portOffset = 0; portOffset < std::max(1u, config.recording.sharedPorts.get()); ++portOffset)
            {
                _sharedRecordingEndpoints.emplace_back(std::vector<RecordingEndpoint*>());
                for (SocketAddress portAddress : interfaces)
                {
                    portAddress.setPort(config.recording.singlePort + portOffset);
                    auto endPoint =
                        new RecordingEndpoint(jobManager, 1024, _mainAllocator, portAddress, _rtcePoll, true);

                    if (endPoint->isGood())
                    {
                        endPoint->registerDefaultListener(this);
                        ++_callbackRefCount;
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
                        delete endPoint;
                    }
                }
            }
        }
    }

    ~TransportFactoryImpl()
    {
        for (auto* serverEndpoint : _tcpServerEndpoints)
        {
            serverEndpoint->close();
        }
        for (auto& sharedEndpointList : _sharedEndpoints)
        {
            for (auto* endPoint : sharedEndpointList)
            {
                endPoint->closePort();
            }
        }
        for (auto& sharedRecordingEndpointList : _sharedRecordingEndpoints)
        {
            for (auto* recEndPoint : sharedRecordingEndpointList)
            {
                recEndPoint->closePort();
            }
        }
        while (_callbackRefCount > 0)
        {
            std::this_thread::yield();
        }
        for (auto& sharedEndpointList : _sharedEndpoints)
        {
            for (auto* endPoint : sharedEndpointList)
            {
                delete endPoint;
            }
        }
        for (auto& sharedRecordingEndpointList : _sharedRecordingEndpoints)
        {
            for (auto* recEndPoint : sharedRecordingEndpointList)
            {
                delete recEndPoint;
            }
        }
        for (auto* serverEndpoint : _tcpServerEndpoints)
        {
            delete serverEndpoint;
        }
    }

    bool openPorts(const SocketAddress& ip, std::vector<Endpoint*>& rtpPorts, std::vector<Endpoint*>& rtcpPorts) const
    {
        const auto portRange = std::make_pair(_config.ice.udpPortRangeLow, _config.ice.udpPortRangeHigh);
        const int portCount = (portRange.second - portRange.first + 1);
        const int offset = _randomGenerator.next() % portCount;

        const int firstPort = (portRange.first + (offset % portCount)) & 0xFFFEu;
        auto rtpEndpoint =
            new UdpEndpoint(_jobManager, 32, _mainAllocator, SocketAddress(ip, firstPort), _rtcePoll, false);
        auto rtcpEndpoint =
            new UdpEndpoint(_jobManager, 32, _mainAllocator, SocketAddress(ip, firstPort + 1), _rtcePoll, false);

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
            delete rtpEndpoint;
            delete rtcpEndpoint;
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

    bool openPorts(const SocketAddress& ip, std::vector<Endpoint*>& rtpPorts) const
    {
        auto portRange = std::make_pair(_config.ice.udpPortRangeLow, _config.ice.udpPortRangeHigh);
        const int portCount = (portRange.second - portRange.first + 1);
        const int offset = _randomGenerator.next() % portCount;

        const int firstPort = (portRange.first + (offset % portCount)) & 0xFFFEu;
        auto rtpEndpoint =
            new UdpEndpoint(_jobManager, 32, _mainAllocator, SocketAddress(ip, firstPort), _rtcePoll, false);

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
            delete rtpEndpoint;
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

    Endpoint* createTcpEndpoint(const transport::SocketAddress& baseAddress) override
    {
        auto endpoint = new TcpEndpoint(_jobManager, _mainAllocator, baseAddress, _rtcePoll);
        endpoint->configureBufferSizes(512 * 1024, 5 * 1024 * 1024);
        return endpoint;
    }

    virtual std::vector<Endpoint*> createTcpEndpoints(int ipFamily) override
    {
        std::vector<Endpoint*> endPoints;
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
        std::vector<Endpoint*> rtpPorts;
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
        std::vector<Endpoint*> rtpPorts;
        std::vector<Endpoint*> rtcpPorts;
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
                    auto* endpoint = _sharedRecordingEndpoints[listIndex][endpointIndex];
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
            for (auto* endpoint : endpoints)
            {
                metrics += endpoint->getMetrics(timestamp);
            }
        }

        return metrics;
    }

    bool isGood() const override { return _good; }

    void maintenance(uint64_t timestamp) override
    {
        for (auto* endpoint : _tcpServerEndpoints)
        {
            endpoint->maintenance(timestamp);
        }
    }

private:
    void onServerPortClosed(ServerEndpoint& endpoint) override
    {
        --_callbackRefCount;
        logger::info("TCP server port %s closed.", "TransportFactory", endpoint.getLocalPort().toString().c_str());
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

    void onPortClosed(Endpoint& endpoint) override { --_callbackRefCount; }
    void onUnregistered(Endpoint& endpoint) override {}
    void onServerPortUnregistered(ServerEndpoint& endpoint) override {}

    jobmanager::JobManager& _jobManager;
    SrtpClientFactory& _srtpClientFactory;
    const config::Config& _config;
    const sctp::SctpConfig& _sctpConfig;
    const ice::IceConfig& _iceConfig;
    const bwe::Config& _bweConfig;
    const bwe::RateControllerConfig& _rateControllerConfig;
    const std::vector<SocketAddress> _interfaces;
    transport::RtcePoll& _rtcePoll;
    std::vector<std::vector<Endpoint*>> _sharedEndpoints;
    std::atomic_uint32_t _sharedEndpointListIndex;
    std::vector<ServerEndpoint*> _tcpServerEndpoints;
    memory::PacketPoolAllocator& _mainAllocator;
    mutable utils::MersienneRandom<uint32_t> _randomGenerator;
    std::atomic_uint32_t _callbackRefCount;

    std::vector<std::vector<RecordingEndpoint*>> _sharedRecordingEndpoints;
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
