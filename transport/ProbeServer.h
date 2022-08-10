#pragma once
#include "transport/Endpoint.h"
#include "ice/IceCandidate.h"

namespace transport {

struct ProbeCandidate
{
    std::string protocol;
    transport::SocketAddress address;
    ice::TransportType transport;
};


class ProbeServer : public Endpoint::IEvents,
                    public ServerEndpoint::IEvents 
{

public:
    ProbeServer();
    virtual ~ProbeServer() {};

    // Endpoint::IEvents
    virtual void onRtpReceived(Endpoint&,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket) override;

    virtual void onDtlsReceived(Endpoint&,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket) override;

    virtual void onRtcpReceived(Endpoint&,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket) override;

    virtual void onIceReceived(Endpoint&,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket) override;

    virtual void onRegistered(Endpoint&) override;
    virtual void onUnregistered(Endpoint&) override;

    // ServerEndpoint::IEvents
    virtual void onServerPortRegistered(ServerEndpoint&) override;
    virtual void onServerPortUnregistered(ServerEndpoint&) override;

    virtual void onIceTcpConnect(std::shared_ptr<Endpoint>,
        const SocketAddress& source,
        const SocketAddress& target,
        memory::UniquePacket) override;

    // Ice
    const std::pair<std::string, std::string>& getProbeCredentials() const;
    //const std::vector<ProbeCandidate> getProbeCandidates() const;

    // Endpoints
    void registerEndpoint(Endpoint&);
    void registerEndpoint(ServerEndpoint&);
    void unregisterEndpoint(Endpoint&);
    void unregisterEndpoint(ServerEndpoint&);    

private:
    std::pair<std::string, std::string> _probeCredentials;
    //std::vector<ProbeCandidate> _probeCandidates;

    void replyStunOk(Endpoint&,
        const SocketAddress&,
        memory::UniquePacket);
};



}