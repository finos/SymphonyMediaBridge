#include "utils/SocketAddress.h"
#include <string>

namespace
{

const std::string FAMILY_IPV4_STR = "IPv4";
const std::string FAMILY_IPV6_STR = "IPv6";
const std::string FAMILY_UNSPEC_STR = "UNSPEC";

} // namespace

namespace transport
{
SocketAddress::SocketAddress()
{
    std::memset(&_address, 0, sizeof(_address));
    _nicName[0] = '\0';
    _address.gen.sa_family = AF_UNSPEC;
}

SocketAddress::SocketAddress(const sockaddr* original, const char* nicName)
{
    std::memset(&_address, 0, sizeof(_address));
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
        std::strncpy(_nicName, nicName, NIC_NAME_MAX_SIZE);
        _nicName[NIC_NAME_MAX_SIZE - 1] = '\0';
    }
}

SocketAddress::SocketAddress(uint32_t ipv4, uint16_t port, const char* nicName)
{
    std::memset(&_address, 0, sizeof(_address));
    _address.gen.sa_family = AF_INET;
    _address.v4.sin_addr.s_addr = htonl(ipv4);
    _address.v4.sin_port = htons(port);

    _nicName[0] = '\0';
    if (nicName)
    {
        std::strncpy(_nicName, nicName, NIC_NAME_MAX_SIZE);
        _nicName[NIC_NAME_MAX_SIZE - 1] = '\0';
    }
}

SocketAddress::SocketAddress(const uint8_t* ipv6_networkOrder, uint16_t port, const char* nicName)
{
    std::memset(&_address, 0, sizeof(_address));
    _address.gen.sa_family = AF_INET6;
    _address.v6.sin6_port = htons(port);
    std::memcpy(&_address.v6.sin6_addr, ipv6_networkOrder, 16);

    _nicName[0] = '\0';
    if (nicName)
    {
        std::strncpy(_nicName, nicName, NIC_NAME_MAX_SIZE);
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

const std::string& SocketAddress::getFamilyString() const
{
    switch (getFamily())
    {
    case AF_INET:
        return FAMILY_IPV4_STR;
    case AF_INET6:
        return FAMILY_IPV6_STR;
    default:
        return FAMILY_UNSPEC_STR;
    }
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

utils::FixString<46> SocketAddress::toFixedString() const
{
    static constexpr int BUF_SIZE = 47; // 46 + null terminator
    char result[BUF_SIZE];
    result[0] = '\0';
    if (getFamily() == AF_INET)
    {
        ::inet_ntop(getFamily(), &_address.v4.sin_addr, result, BUF_SIZE);
    }
    else
    {
        ::inet_ntop(getFamily(), &_address.v6.sin6_addr, result, BUF_SIZE);
    }

    size_t len = std::strlen(result);
    // worst case we need more 6 bytes for port (:65535)
    if (len + 6 >= BUF_SIZE)
    {
        return utils::FixString<46>(result);
    }

    result[len++] = ':';

    int digits = 0;
    uint16_t port = getPort();
    do
    {
        result[len++] = '0' + (port % 10);
        port /= 10;
        ++digits;
    } while (port);

    std::reverse(&result[len - digits], &result[len]);

    result[len] = '\0';
    return utils::FixString<46>(result);
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
        return 0 == std::memcmp(&b.getIpv4()->sin_addr, &getIpv4()->sin_addr, sizeof(in_addr));
    }
    else if (getFamily() == AF_INET6)
    {
        return 0 == std::memcmp(&b.getIpv6()->sin6_addr, &getIpv6()->sin6_addr, sizeof(in6_addr));
    }
    return false;
}

bool SocketAddress::isLinkLocal() const
{
    if (getFamily() == AF_INET)
    {
        const auto address = getIpv4();
        return address && (ntohl(address->sin_addr.s_addr) & 0xFFFF0000u) == 0xa9fe0000u;
    }
    else if (getFamily() == AF_INET6)
    {
        const auto address = getIpv6();
#ifdef __APPLE__
        return address && ntohs(address->sin6_addr.__u6_addr.__u6_addr16[0]) == 0xfe80;
#else
        return address && ntohs(address->sin6_addr.__in6_u.__u6_addr16[0]) == 0xfe80;
#endif
    }

    return false;
}

/**
 * Link local addresses are fe80::/64 and 169.254.0.0/16 used for communication with routers only.
 */
std::vector<SocketAddress> SocketAddress::activeInterfaces(bool includeLoopback, bool includeLinkLocal)
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
                SocketAddress a(nicIp->ifa_addr, nullptr);
                if (includeLinkLocal || !a.isLinkLocal())
                {
                    list.emplace_back(nicIp->ifa_addr, nicIp->ifa_name);
                }
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
