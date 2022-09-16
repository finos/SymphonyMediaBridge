#include "IceCandidate.h"
#include "utils/StdExtensions.h"
#include <cinttypes>

namespace
{

const std::string UNKNOWN = "UNKNOWN";

const std::string ICE_TYPE_HOST_STR = "HOST";
const std::string ICE_TYPE_SRFLX_STR = "SRFLX";
const std::string ICE_TYPE_PRFLX_STR = "PRFLX";
const std::string ICE_TYPE_RELAY_STR = "RELAY";

const std::string TRANSPORT_TYPE_UDP_STR = "UDP";
const std::string TRANSPORT_TYPE_TCP_STR = "TCP";
const std::string TRANSPORT_TYPE_SSLTCP_STR = "SSLTCP";

} // namespace
namespace ice
{
IceCandidate::IceCandidate()
    : component(IceComponent::RTP),
      transportType(TransportType::UDP),
      priority(100),
      type(Type::HOST),
      tcpType(TcpType::ACTIVE)
{
    _foundation[0] = '\0';
}

IceCandidate::IceCandidate(const char* foundation,
    IceComponent _component,
    TransportType _transportType,
    uint32_t _priority,
    const transport::SocketAddress& _address,
    Type _type,
    TcpType tcpType_)
    : component(_component),
      transportType(_transportType),
      priority(_priority),
      address(_address),
      type(_type),
      tcpType(tcpType_)
{
    utils::strncpy(_foundation, foundation, MAX_FOUNDATION);
}

IceCandidate::IceCandidate(const char* foundation,
    IceComponent _component,
    TransportType _transportType,
    uint32_t _priority,
    const transport::SocketAddress& _address,
    const transport::SocketAddress& _baseAddress,
    Type _type,
    TcpType tcpType_)
    : component(_component),
      transportType(_transportType),
      priority(_priority),
      address(_address),
      baseAddress(_baseAddress),
      type(_type),
      tcpType(tcpType_)
{
    utils::strncpy(_foundation, foundation, MAX_FOUNDATION);
}

IceCandidate::IceCandidate(IceComponent component_,
    TransportType transportType_,
    uint32_t priority_,
    const transport::SocketAddress& address_,
    const transport::SocketAddress& baseAddress_,
    Type type_,
    TcpType tcpType_)
    : component(component_),
      transportType(transportType_),
      priority(priority_),
      address(address_),
      baseAddress(baseAddress_),
      type(type_),
      tcpType(tcpType_)
{
    size_t value = (std::hash<transport::SocketAddress>()(baseAddress_) >> 24) & ~0xFull;
    value += (static_cast<int>(transportType) << 2) + static_cast<int>(type);
    std::snprintf(_foundation, MAX_FOUNDATION, "%zu", value);
}

IceCandidate::IceCandidate(const IceCandidate& b, Type newType)
    : component(b.component),
      transportType(b.transportType),
      priority(b.priority),
      address(b.address),
      baseAddress(b.baseAddress),
      type(newType),
      tcpType(b.tcpType)
{
    utils::strncpy(_foundation, b._foundation, MAX_FOUNDATION);
}

IceCandidate::IceCandidate(const IceCandidate& b)
    : component(b.component),
      transportType(b.transportType),
      priority(b.priority),
      address(b.address),
      baseAddress(b.baseAddress),
      type(b.type),
      tcpType(b.tcpType)
{
    utils::strncpy(_foundation, b._foundation, MAX_FOUNDATION);
}

IceCandidate& IceCandidate::operator=(const IceCandidate& b)
{
    component = b.component;
    transportType = b.transportType;
    priority = b.priority;
    address = b.address;
    type = b.type;
    baseAddress = b.baseAddress;
    tcpType = b.tcpType;
    setFoundation(b.getFoundation());

    return *this;
}

const std::string& toString(IceCandidate::Type type)
{
    switch (type)
    {
    case IceCandidate::Type::HOST:
        return ICE_TYPE_HOST_STR;
    case IceCandidate::Type::SRFLX:
        return ICE_TYPE_SRFLX_STR;
    case IceCandidate::Type::PRFLX:
        return ICE_TYPE_PRFLX_STR;
    case IceCandidate::Type::RELAY:
        return ICE_TYPE_RELAY_STR;
    }

    return UNKNOWN;
}

const std::string& toString(TransportType type)
{
    switch (type)
    {
    case TransportType::UDP:
        return TRANSPORT_TYPE_UDP_STR;
    case TransportType::TCP:
        return TRANSPORT_TYPE_TCP_STR;
    case TransportType::SSLTCP:
        return TRANSPORT_TYPE_SSLTCP_STR;
    }

    return UNKNOWN;
}

uint32_t IceCandidate::computeCandidatePriority(IceCandidate::Type type,
    int localInterfacePreference,
    IceComponent component,
    TransportType transportType)
{
    int typePreference = 0;
    switch (type)
    {
    case IceCandidate::Type::HOST:
        typePreference = 126;
        break;
    case IceCandidate::Type::PRFLX:
        typePreference = 110;
        break;
    case IceCandidate::Type::SRFLX:
        typePreference = 100;
        break;
    case IceCandidate::Type::RELAY:
        typePreference = 0;
    }
    return ((8 - int(transportType)) << 24) + (typePreference << 16) + (localInterfacePreference << 8) +
        (256 - static_cast<int>(component));
}

} // namespace ice