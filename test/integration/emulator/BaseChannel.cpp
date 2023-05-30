#include "test/integration/emulator/BaseChannel.h"
#include "test/integration/emulator/HttpRequests.h"
#include "utils/IdGenerator.h"

namespace emulator
{
std::string newGuuid()
{
    utils::IdGenerator idGen;
    std::string uuid(36, '\0');

    snprintf(&uuid.front(), // + null terminator
        uuid.size() + 1,
        "%08x-%04x-%04x-%04x-%012x",
        static_cast<uint32_t>(idGen.next() & 0xFFFFFFFFu),
        static_cast<uint32_t>(idGen.next() & 0xFFFFu),
        static_cast<uint32_t>(idGen.next() & 0xFFFFu),
        static_cast<uint32_t>(idGen.next() & 0xFFFFu),
        static_cast<uint32_t>(idGen.next()));

    return uuid;
}

std::string newIdString()
{
    utils::IdGenerator idGen;
    std::string uuid(8, '\0');

    snprintf(&uuid.front(),
        uuid.size() + 1, // + null terminator
        "%08u",
        static_cast<uint32_t>(idGen.next() & 0xFFFFFFFFu));

    return uuid;
}

BaseChannel::BaseChannel(emulator::HttpdFactory* httpd)
    : _httpd(httpd),
      _id(newGuuid()),
      _audioId(newIdString()),
      _dataId(newIdString()),
      _videoId(newIdString())
{
}

void BaseChannel::setRemoteIce(transport::RtcTransport& transport,
    nlohmann::json bundle,
    const char* candidatesGroupName,
    memory::AudioPacketPoolAllocator& allocator)
{
    ice::IceCandidates candidates;

    for (auto& c : bundle[candidatesGroupName]["candidates"])
    {
        ice::IceCandidate candidate(c["foundation"].template get<std::string>().c_str(),
            ice::IceComponent::RTP,
            c["protocol"] == "udp" ? ice::TransportType::UDP : ice::TransportType::TCP,
            c["priority"].template get<uint32_t>(),
            transport::SocketAddress::parse(c["ip"], c["port"]),
            ice::IceCandidate::Type::HOST,
            ice::TcpType::PASSIVE);

        if (!_callConfig.enableIpv6 && candidate.address.getFamily() == AF_INET6)
        {
            _ipv6RemoteCandidates.push_back(candidate);
            continue;
        }

        candidates.push_back(candidate);
    }

    std::pair<std::string, std::string> credentials;
    credentials.first = bundle[candidatesGroupName]["ufrag"];
    credentials.second = bundle[candidatesGroupName]["pwd"];

    logger::info("client %s remote ICE for %s, %s",
        "Channel",
        transport.getLoggableId().c_str(),
        credentials.first.c_str(),
        credentials.second.c_str());

    transport.setRemoteIce(credentials, candidates, allocator);
}

void BaseChannel::addIpv6RemoteCandidates(transport::RtcTransport& transport)
{
    for (auto candidate : _ipv6RemoteCandidates)
    {
        logger::info("adding ipv6 candidate %s to %s",
            "ApiChannel",
            candidate.address.toString().c_str(),
            transport.getLoggableId().c_str());
        transport.addRemoteIceCandidate(candidate);
    }
}

} // namespace emulator
