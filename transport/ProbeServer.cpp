#include "transport/ProbeServer.h"
#include "transport/ice/Stun.h"
#include "utils/Time.h"

namespace transport
{
ProbeServer::ProbeServer()
{
    _probeCredentials = std::make_pair<std::string, std::string>("", "");
}


// Endpoint::IEvents
void ProbeServer::onRtpReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet) {
    }

void ProbeServer::onDtlsReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet) {};

void ProbeServer::onRtcpReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet) {}

void ProbeServer::onIceReceived(Endpoint& endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet) 
{
    // TODO validate credentials?
    replyStunOk(endpoint, source, std::move(packet));
}

void ProbeServer::onRegistered(Endpoint& endpoint) {}
void ProbeServer::onUnregistered(Endpoint& endpoint) {}

// ServerEndpoint::IEvents
void ProbeServer::onServerPortRegistered(ServerEndpoint& endpoint) {}

void ProbeServer::onServerPortUnregistered(ServerEndpoint& endpoint)
{
    // TODO do not re-register if unregistered on shutdown?
    endpoint.registerListener(_probeCredentials.first, this);
}

void ProbeServer::onIceTcpConnect(std::shared_ptr<Endpoint> endpoint,
    const SocketAddress& source,
    const SocketAddress& target,
    memory::UniquePacket packet) 
{
    if (endpoint->getTransportType() == ice::TransportType::TCP)
    {
        replyStunOk(*endpoint, source, std::move(packet));
    }
}

void ProbeServer::replyStunOk(Endpoint& endpoint, 
    const SocketAddress& destination,
    memory::UniquePacket packet)
{
    uint64_t timestamp = utils::Time::getAbsoluteTime();
    const void *data = packet->get();

    auto* msg = ice::StunMessage::fromPtr(data);
    const auto method = msg->header.getMethod();

    // TODO
    // verify the ufrag password

    if (method == ice::StunHeader::BindingRequest)
    {
        ice::StunMessage response;
        response.header.transactionId = msg->header.transactionId;
        response.header.setMethod(ice::StunHeader::BindingResponse);
        //response.add(StunGenericAttribute(StunAttribute::SOFTWARE, _config.software));
        response.add(ice::StunXorMappedAddress(destination, response.header));
        response.addMessageIntegrity(_probeCredentials.second);
        response.addFingerprint();

        endpoint.sendStunTo(
            destination, 
            response.header.transactionId.get(), 
            &response, 
            response.size(), 
            timestamp);         
    }    
}

void ProbeServer::registerEndpoint(Endpoint& endpoint)
{
    endpoint.registerListener(_probeCredentials.first, this);
}

void ProbeServer::registerEndpoint(ServerEndpoint& endpoint)
{
    endpoint.registerListener(_probeCredentials.first, this);
}

// ICE
const std::pair<std::string, std::string>& ProbeServer::getProbeCredentials() const
{
    return _probeCredentials;
}

}