#include "SctpEndpoint.h"

namespace sctptest
{
using namespace sctp;

const char* c_str(sctp::ChunkType type)
{
    using namespace sctp;
    switch (type)
    {
    case ChunkType::INIT:
        return "INIT";
    case ChunkType::INIT_ACK:
        return "INIT_ACK";
    case ChunkType::COOKIE_ECHO:
        return "COOKIE_ECHO";
    case ChunkType::COOKIE_ACK:
        return "COOKIE_ACK";
    case ChunkType::SACK:
        return "SACK";
    case ChunkType::SHUTDOWN:
        return "SHUTDOWN";
    case ChunkType::ABORT:
        return "ABORT";
    case ChunkType::DATA:
        return "DATA";
    case ChunkType::HEARTBEAT:
        return "HEARTBEAT";
    case ChunkType::HEARTBEAT_ACK:
        return "HEARTBEAT_ACK";
    default:
        return "unknown";
    }
}

SctpEndpoint::SctpEndpoint(uint16_t port,
    const sctp::SctpConfig& config,
    uint64_t& timeSource,
    uint32_t bandwidthKpbs,
    uint32_t mtu)
    : _loggableId("SctpEndpoint"),
      _port(std::make_unique<sctp::SctpServerPort>(_loggableId.getInstanceId(), this, this, port, config, timeSource)),
      _sendQueue(bandwidthKpbs, std::max(bandwidthKpbs * 1000 / 4, mtu * 6u), mtu),
      _streamId(0),
      _dataSizeReceived(0),
      _receivedMessageCount(0),
      _allocator(256, "SctpEndpoint"),
      _config(config),
      _timeSource(timeSource),
      _outboundLossCount(0)
{
}

SctpEndpoint::~SctpEndpoint()
{
    for (auto* packet = _sendQueue.pop(_timeSource + 600 * timer::sec); packet;
         packet = _sendQueue.pop(_timeSource + 600 * timer::sec))
    {
        _allocator.free(packet);
    }
}

void SctpEndpoint::connect(uint16_t dstPort)
{
    _session = sctp::createSctpAssociation(_loggableId.getInstanceId(), *_port, dstPort, this, _config);
    _session->connect(0xFFFFu, 0xFFFFu, _timeSource);
}

uint64_t SctpEndpoint::getTimeout() const
{
    return std::min(_session->nextTimeout(_timeSource), std::max(int64_t(0), _sendQueue.timeToRelease(_timeSource)));
}

bool SctpEndpoint::forwardPacket(SctpEndpoint& target)
{
    using namespace sctp;
    memory::Packet* packet = _sendQueue.pop(_timeSource);
    if (!packet)
    {
        return false;
    }
    ++sentPacketCount;

    target._port->onPacketReceived(packet->get(), packet->getLength(), _timeSource);
    _allocator.free(packet);
    return true;
}

void SctpEndpoint::forwardWhenReady(SctpEndpoint& target)
{
    if (_sendQueue.empty())
    {
        return; // nothing to forward
    }

    if (_sendQueue.timeToRelease(_timeSource) > 0)
    {
        _timeSource += _sendQueue.timeToRelease(_timeSource);
    }

    forwardPacket(target);
}

void SctpEndpoint::forwardPackets(SctpEndpoint& target)
{
    using namespace sctp;
    while (forwardPacket(target)) {}
}

bool SctpEndpoint::sendSctpPacket(const void* data, size_t length)
{
    if (length > _sendQueue.getSctpMTU())
    {
        return false;
    }

    if (length > memory::Packet::size)
    {
        logger::error("sctp packet %zu exceeds MTU %zu", _loggableId.c_str(), length, memory::Packet::size);
        return false;
    }

    auto* packet = memory::makePacket(_allocator, data, length);
    if (packet && _sendQueue.push(packet, _timeSource))
    {
        return true;
    }

    ++_outboundLossCount;
    logger::debug("packet lost, total loss %u", _loggableId.c_str(), _outboundLossCount);
    _allocator.free(packet); // mtu or buffer full
    return false;
}

bool SctpEndpoint::onSctpInitReceived(sctp::SctpServerPort* serverPort,
    uint16_t srcPort,
    const sctp::SctpPacket& sctpPacket,
    uint64_t timestamp,
    uint16_t& inboundStreams,
    uint16_t& outboundStreams)
{
    if (!!_session)
    {
        _session->onPacketReceived(sctpPacket, timestamp);
        return false;
    }
    auto chunk = sctpPacket.getChunk<InitChunk>(ChunkType::INIT);

    inboundStreams = chunk->outboundStreams;
    outboundStreams = chunk->inboundStreams;
    return true;
}

void SctpEndpoint::onSctpCookieEchoReceived(sctp::SctpServerPort* serverPort,
    uint16_t srcPort,
    const sctp::SctpPacket& packet,
    uint64_t timestamp)
{
    if (!!_session)
    {
        _session->onPacketReceived(packet, timestamp);
    }
    else
    {
        _session = sctp::createSctpAssociation(_loggableId.getInstanceId(), *serverPort, packet, this, _config);
    }
}

void SctpEndpoint::onSctpReceived(sctp::SctpServerPort* serverPort,
    uint16_t srcPort,
    const sctp::SctpPacket& sctpPacket,
    uint64_t timestamp)
{
    if (!!_session)
    {
        _session->onPacketReceived(sctpPacket, timestamp);
    }
}

void SctpEndpoint::onSctpStateChanged(sctp::SctpAssociation* session, sctp::SctpAssociation::State state)
{
    auto ports = session->getPortPair();
    logger::debug("sctp state change %u-%u, state %s",
        _loggableId.c_str(),
        std::get<0>(ports),
        std::get<1>(ports),
        toString(state));
}
} // namespace sctptest
