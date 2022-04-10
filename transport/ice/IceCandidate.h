#pragma once

#include "IceComponent.h"
#include "utils/SocketAddress.h"

namespace ice
{

enum class TransportType : int
{
    UDP,
    TCP,
    SSLTCP
};

enum class TcpType
{
    ACTIVE,
    PASSIVE,
    SO
};

class IceCandidate
{
public:
    static const int MAX_FOUNDATION = 32;
    enum class Type
    {
        HOST,
        SRFLX,
        PRFLX,
        RELAY
    };

    IceCandidate();
    IceCandidate(const char* foundation,
        IceComponent component,
        TransportType transportType,
        uint32_t priority,
        const transport::SocketAddress& address,
        Type type,
        TcpType tcpType = TcpType::ACTIVE);

    IceCandidate(const char* foundation,
        IceComponent component,
        TransportType transportType,
        uint32_t priority,
        const transport::SocketAddress& address,
        const transport::SocketAddress& baseAddress,
        Type type,
        TcpType tcpType = TcpType::ACTIVE);

    IceCandidate(IceComponent component,
        TransportType transportType,
        uint32_t priority,
        const transport::SocketAddress& address,
        const transport::SocketAddress& baseAddress,
        Type type,
        TcpType tcpType = TcpType::ACTIVE);

    IceCandidate(const IceCandidate& b, Type newType);
    IceCandidate(const IceCandidate& b);

    IceCandidate& operator=(const IceCandidate& b);

    bool empty() const { return address.empty(); }
    const char* getFoundation() const { return _foundation; }
    void setFoundation(const char* f) { std::strncpy(_foundation, f, MAX_FOUNDATION); }

    IceComponent component;
    TransportType transportType;
    uint32_t priority;
    transport::SocketAddress address;
    transport::SocketAddress baseAddress;
    Type type;
    TcpType tcpType;

private:
    char _foundation[MAX_FOUNDATION + 1];
};
} // namespace ice
