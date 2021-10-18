#include "SocketAddress.h"
#include <string>

namespace transport
{
SocketAddress::SocketAddress()
{
    memset(&_address, 0, sizeof(_address));
    _nicName[0] = '\0';
    _address.gen.sa_family = AF_UNSPEC;
}

SocketAddress::SocketAddress(const sockaddr* original, const char* nicName)
{
    memset(&_address, 0, sizeof(_address));
    if (original->sa_family == AF_INET)
    {
        std::memcpy(&_address, original, sizeof(sockaddr_in));
    }
    else if (original->sa_family == AF_INET6)
    {
        std::memcpy(&_address, original, sizeof(sockaddr_in6));
    }
    else
    {
        _address.gen.sa_family = AF_UNSPEC;
    }

    _nicName[0] = '\0';
    if (nicName)
    {
        strncpy(_nicName, nicName, NIC_NAME_MAX_SIZE);
        _nicName[NIC_NAME_MAX_SIZE - 1] = '\0';
    }
}

SocketAddress::SocketAddress(uint32_t ipv4, uint16_t port, const char* nicName)
{
    memset(&_address, 0, sizeof(_address));
    _address.gen.sa_family = AF_INET;
    _address.v4.sin_addr.s_addr = htonl(ipv4);
    _address.v4.sin_port = htons(port);

    _nicName[0] = '\0';
    if (nicName)
    {
        strncpy(_nicName, nicName, NIC_NAME_MAX_SIZE);
        _nicName[NIC_NAME_MAX_SIZE - 1] = '\0';
    }
}

SocketAddress::SocketAddress(const uint8_t* ipv6_networkOrder, uint16_t port, const char* nicName)
{
    memset(&_address, 0, sizeof(_address));
    _address.gen.sa_family = AF_INET6;
    _address.v6.sin6_port = htons(port);
    memcpy(&_address.v6.sin6_addr, ipv6_networkOrder, 16);

    _nicName[0] = '\0';
    if (nicName)
    {
        strncpy(_nicName, nicName, NIC_NAME_MAX_SIZE);
        _nicName[NIC_NAME_MAX_SIZE - 1] = '\0';
    }
}

const sockaddr_in* SocketAddress::getIpv4() const
{
    if (getFamily() == AF_INET)
    {
        return &_address.v4;
    }
    return nullptr;
}

const sockaddr_in6* SocketAddress::getIpv6() const
{
    if (getFamily() == AF_INET6)
    {
        return &_address.v6;
    }
    return nullptr;
}

SocketAddress& SocketAddress::setPort(uint16_t port)
{
    _address.v4.sin_port = htons(port);
    return *this;
}

std::string SocketAddress::ipToString() const
{
    static const int BUF_SIZE = 50;
    char result[BUF_SIZE];
    if (getFamily() == AF_INET)
    {
        if (result == ::inet_ntop(getFamily(), &_address.v4.sin_addr, result, BUF_SIZE))
        {
            return std::string(result);
        }
    }
    else
    {
        if (result == ::inet_ntop(getFamily(), &_address.v6.sin6_addr, result, BUF_SIZE))
        {
            return std::string(result);
        }
    }
    return std::string();
}

std::string SocketAddress::toString() const
{
    if (getFamily() == AF_INET)
    {
        if (getPort() != 0)
        {
            return ipToString() + ":" + std::to_string(getPort());
        }
        else
        {
            return ipToString();
        }
    }
    else
    {
        if (getPort() != 0)
        {
            return "[" + ipToString() + "]:" + std::to_string(getPort());
        }
        else
        {
            return ipToString();
        }
    }
}

uint16_t SocketAddress::getPort() const
{
    return ntohs(_address.v4.sin_port);
}

bool SocketAddress::equalsIp(const SocketAddress& b) const
{
    if (getFamily() != b.getFamily())
    {
        return false;
    }

    if (getFamily() == AF_INET)
    {
        return 0 == memcmp(&b.getIpv4()->sin_addr, &getIpv4()->sin_addr, sizeof(in_addr));
    }
    else if (getFamily() == AF_INET6)
    {
        return 0 == memcmp(&b.getIpv6()->sin6_addr, &getIpv6()->sin6_addr, sizeof(in6_addr));
    }
    return false;
}

std::vector<SocketAddress> SocketAddress::activeInterfaces(bool includeLoopback)
{
    std::vector<SocketAddress> list;
    struct ifaddrs* nics;
    if (!getifaddrs(&nics))
    {
        for (const struct ifaddrs* nicIp = nics; nicIp; nicIp = nicIp->ifa_next)
        {
            if (SocketAddress::isSupported(nicIp->ifa_addr) && (nicIp->ifa_flags & IFF_UP) == IFF_UP &&
                ((nicIp->ifa_flags & IFF_LOOPBACK) == 0 || includeLoopback))
            {
                list.emplace_back(nicIp->ifa_addr, nicIp->ifa_name);
            }
        }
        freeifaddrs(nics);
    }

    return list;
}

bool operator==(const SocketAddress& a, const SocketAddress& b)
{
    if (a.getFamily() != b.getFamily() || a.getPort() != b.getPort())
    {
        return false;
    }

    return a.equalsIp(b);
}

bool operator!=(const SocketAddress& a, const SocketAddress& b)
{
    return !(a == b);
}

// TODO overload with parsing of port. ipv4:port and [ipv6]:port
SocketAddress SocketAddress::parse(const std::string& ip, uint16_t port)
{
    RawSockAddress addr;
    std::memset(&addr, 0, sizeof(addr));

    if (::inet_pton(AF_INET, ip.c_str(), &addr.v4.sin_addr))
    {
        addr.gen.sa_family = AF_INET;
    }
    else if (::inet_pton(AF_INET6, ip.c_str(), &addr.v6.sin6_addr))
    {
        addr.gen.sa_family = AF_INET6;
    }

    addr.v4.sin_port = htons(port);
    return SocketAddress(&addr.gen);
}

bool operator<(const SocketAddress& a, const SocketAddress& b)
{
    if (a.getFamily() != b.getFamily())
    {
        return a.getFamily() == AF_INET;
    }

    return std::memcmp(a.getSockAddr(), b.getSockAddr(), a.getSockAddrSize()) > 0;
}
} // namespace transport
