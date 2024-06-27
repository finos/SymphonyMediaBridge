#pragma once
#include "logger/Logger.h"
#include "utils/FixString.h"
#include "utils/FowlerNollHash.h"
#include <arpa/inet.h>
#include <cassert>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <string>
#include <vector>

namespace transport
{
typedef union
{
    sockaddr gen;
    sockaddr_in v4;
    sockaddr_in6 v6;
} RawSockAddress;

class SocketAddress
{
    static const int NIC_NAME_MAX_SIZE = 12;

public:
    SocketAddress();
    explicit SocketAddress(const sockaddr* original, const char* nicName = nullptr);
    SocketAddress(uint32_t ipv4, uint16_t port, const char* nicName = nullptr);
    SocketAddress(const uint8_t* ipv6_networkOrder, uint16_t port, const char* nicName = nullptr);
    SocketAddress(const SocketAddress& other) : SocketAddress(&other._address.gen)
    {
        std::memcpy(_nicName, other._nicName, NIC_NAME_MAX_SIZE);
    }
    SocketAddress(const SocketAddress& ip, uint16_t port) : SocketAddress(&ip._address.gen) { setPort(port); }

    int getFamily() const { return _address.gen.sa_family; }
    const std::string& getFamilyString() const;

    const sockaddr* getSockAddr() const { return &_address.gen; }

    size_t getSockAddrSize() const { return (getFamily() == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6)); }

    const sockaddr_in* getIpv4() const;

    const sockaddr_in6* getIpv6() const;

    SocketAddress& setPort(uint16_t port);
    std::string ipToString() const;
    std::string toString() const;
    utils::FixString<46> toFixedString() const;

    uint16_t getPort() const;

    bool equalsIp(const SocketAddress& b) const;
    std::string getName() const { return std::string(_nicName); }

    bool empty() const { return _address.gen.sa_family == AF_UNSPEC; }
    bool isLinkLocal() const;
    static bool isSupported(sockaddr* addr)
    {
        if (!addr)
        {
            return false;
        }
        return addr->sa_family == AF_INET || addr->sa_family == AF_INET6;
    }

    static std::vector<SocketAddress> activeInterfaces(bool includeLoopback, bool includeLinkLocal);
    static SocketAddress parse(const std::string& ip, uint16_t port = 0);
    static SocketAddress createBroadcastIpv6() { return SocketAddress::parse("::1"); }
    static SocketAddress createBroadcastIpv4() { return SocketAddress(0u, static_cast<uint16_t>(0u)); }

private:
    RawSockAddress _address;
    char _nicName[NIC_NAME_MAX_SIZE];
};

bool operator==(const SocketAddress& a, const SocketAddress& b);
bool operator!=(const SocketAddress& a, const SocketAddress& b);
bool operator<(const SocketAddress& a, const SocketAddress& b);

} // namespace transport

namespace std
{
template <>
class hash<transport::SocketAddress>
{
public:
    size_t operator()(const transport::SocketAddress& address) const
    {
        CompactIp compactAddress;
        compactAddress.port = address.getPort();
        if (address.getFamily() == AF_INET6)
        {
            std::memcpy(&compactAddress.ip.v6, &address.getIpv6()->sin6_addr, sizeof(compactAddress.ip.v6));
        }
        else if (address.getFamily() == AF_INET)
        {
            std::memcpy(&compactAddress.ip.v4, &address.getIpv4()->sin_addr, sizeof(compactAddress.ip.v4));
        }
        else
        {
            assert(false);
        }

        return utils::FowlerNollVoHash(&compactAddress, sizeof(compactAddress));
    }

private:
    struct CompactIp
    {
        CompactIp() { std::memset(this, 0, sizeof(CompactIp)); }
        uint16_t port;
        union
        {
            in_addr v4;
            in6_addr v6;
        } ip;
    };
};

} // namespace std

namespace transport
{
/**
 * This function will mask the IP if the log level is not debug
 */
inline utils::FixString<46> maybeMasked(const SocketAddress& address)
{

    if (logger::_logLevel != logger::Level::DBG)
    {
        const char* prefix = "";
        size_t ipHash = 0;
        if (address.getFamily() == AF_INET6)
        {
            const auto sin6 = address.getIpv6()->sin6_addr;
            ipHash = utils::FowlerNollVoHash(&sin6, sizeof(sin6));
            prefix = "ipv6-";
        }
        else if (address.getFamily() == AF_INET)
        {
            const auto sin = address.getIpv4()->sin_addr;
            ipHash = utils::FowlerNollVoHash(&sin, sizeof(sin));
            prefix = "ipv4-";
        }
        else
        {
            assert(false);
        }

        return utils::FixString<46>::sprintf("%s%zu:%" PRIu16, prefix, ipHash, address.getPort());
    }

    return address.toFixedString();
}
} // namespace transport
