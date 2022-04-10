#include "IceSerialize.h"

memory::MemoryFile& operator<<(memory::MemoryFile& f, const transport::SocketAddress& address)
{
    f << static_cast<uint8_t>(address.getSockAddrSize());
    f.write(address.getSockAddr(), address.getSockAddrSize());
    return f;
}

memory::MemoryFile& operator>>(memory::MemoryFile& f, transport::SocketAddress& address)
{
    uint8_t len = 0;
    f >> len;
    transport::RawSockAddress a;
    f.read(&a, len);
    address = transport::SocketAddress(&a.gen, nullptr);
    return f;
}

memory::MemoryFile& operator<<(memory::MemoryFile& f, const ice::IceCandidate& candidate)
{
    if (!f.isGood())
    {
        return f;
    }

    auto currentPos = f.getPosition();
    f << static_cast<uint8_t>(candidate.component);
    f << static_cast<uint8_t>(candidate.transportType);
    f << candidate.priority;
    f << candidate.address;
    f << candidate.baseAddress;
    f << static_cast<uint8_t>(candidate.type);
    f << candidate.getFoundation();
    if (candidate.transportType != ice::TransportType::UDP)
    {
        f << static_cast<uint8_t>(candidate.tcpType);
    }

    if (!f.isGood())
    {
        f.setPosition(currentPos); // failed
        f.write(&candidate, sizeof(candidate)); // set good flag to false
    }

    return f;
}

memory::MemoryFile& operator>>(memory::MemoryFile& f, ice::IceCandidate& candidate)
{
    uint8_t tmp8 = 0;
    char foundation[35];
    f >> tmp8;
    candidate.component = static_cast<ice::IceComponent>(tmp8);
    f >> tmp8;
    candidate.transportType = static_cast<ice::TransportType>(tmp8);
    f >> candidate.priority;
    f >> candidate.address;
    f >> candidate.baseAddress;
    f >> tmp8;
    candidate.type = static_cast<ice::IceCandidate::Type>(tmp8);
    f >> foundation;
    candidate.setFoundation(foundation);
    if (candidate.transportType != ice::TransportType::UDP)
    {
        f >> tmp8;
        candidate.tcpType = static_cast<ice::TcpType>(tmp8);
    }

    return f;
}
