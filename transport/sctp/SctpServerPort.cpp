
#include "SctpServerPort.h"
#include "SctpConfig.h"
#include "SctpTimer.h"
#include "Sctprotocol.h"
#include "crypto/SslHelper.h"
#include "logger/Logger.h"
#include "utils/MersienneRandom.h"
namespace sctp
{

SctpCookie::SctpCookie()
    : outboundStreams(0),
      inboundStreams(0),
      timestamp(0),
      tag(0, 0),
      peerTSN(0),
      peerReceiveWindow(0)
{
    hmac[0] = 0;
}

SctpCookie::SctpCookie(uint64_t timestamp_, uint32_t localTag, uint32_t peerTag, uint32_t peerTSN_, uint32_t peerRwnd)
    : outboundStreams(1),
      inboundStreams(1),
      timestamp(timestamp_),
      tag(localTag, peerTag),
      peerTSN(peerTSN_),
      peerReceiveWindow(peerRwnd)
{
    hmac[0] = 0;
}

void SctpCookie::sign(const uint8_t* key, size_t keyLength, uint16_t remotePort)
{
    crypto::HMAC signer(key, keyLength);
    addSignedItems(signer, remotePort);
    signer.compute(hmac);
}

bool SctpCookie::verifySignature(const uint8_t* key, size_t keyLength, uint16_t remotePort) const
{
    crypto::HMAC signer(key, keyLength);
    addSignedItems(signer, remotePort);
    uint8_t computedHmac[20];
    signer.compute(computedHmac);
    return 0 == std::memcmp(computedHmac, hmac, sizeof(hmac));
}

void SctpCookie::addSignedItems(crypto::HMAC& signer, uint16_t remotePort) const
{
    signer.add(tag.local.get());
    signer.add(tag.peer.get());
    signer.add(tieTag.local.get());
    signer.add(tieTag.peer.get());
    signer.add(remotePort);
    signer.add(timestamp.get());
    signer.add(outboundStreams.get());
    signer.add(inboundStreams.get());
    signer.add(peerTSN.get());
    signer.add(peerReceiveWindow.get());
}

SctpServerPort::SctpServerPort(size_t logId,
    DatagramTransport* transport,
    IEvents* listener,
    uint16_t localPort,
    const SctpConfig& config,
    uint64_t timestamp)
    : _loggableId("SctpServerPort", logId),
      _config(config),
      _localPort(localPort),
      _transport(transport),
      _listener(listener),
      _nextKeyRotation(timestamp)
{
    assert(_listener);
    rotateKeys();
    rotateKeys();
}

void SctpServerPort::rotateKeys()
{
    std::memcpy(_key2, _key1, sizeof(_key2));
    const auto dummyFragment = _randomGenerator.next();
    for (size_t i = 0; i < sizeof(_key1); i += sizeof(dummyFragment))
    {
        auto keyFragment = _randomGenerator.next();
        for (size_t j = 0; j < sizeof(keyFragment); ++j)
        {
            _key1[j + i] = keyFragment;
            keyFragment >>= 8;
        }
    }
    _nextKeyRotation += _config.cookieLifeTime * timer::ms;
}

void SctpServerPort::send(SctpPacketW& packet)
{
    packet.commitCheckSum();
    _transport->sendSctpPacket(packet.get(), packet.size());
}

// packets without recognised tag are sent here
void SctpServerPort::onPacketReceived(const void* data, size_t length, uint64_t timestamp)
{
    SctpPacket sctpPacket(data, length);
    if (!sctpPacket.isValid())
    {
        logger::warn("received malformed SCTP", _loggableId.c_str());
        return; // ignore it
    }
    auto& header = sctpPacket.getHeader();
    if (header.destinationPort != _localPort)
    {
        logger::warn("SCTP destination port invalid %u", _loggableId.c_str(), header.destinationPort.get());
        return;
    }
    if (sctpPacket.getChunk(ChunkType::INIT))
    {
        onInitReceived(sctpPacket, timestamp);
    }
    else if (sctpPacket.getChunk(ChunkType::COOKIE_ECHO))
    {
        onCookieEchoReceived(sctpPacket, timestamp);
    }
    else
    {
        _listener->onSctpReceived(this, header.sourcePort, sctpPacket, timestamp);
    }
}

void SctpServerPort::onInitReceived(const SctpPacket& sctpPacket, uint64_t timestamp)
{
    const auto& header = sctpPacket.getHeader();
    auto chunk = sctpPacket.getChunk<InitChunk>(ChunkType::INIT);
    if (chunk)
    {
        for (auto& param : chunk->params())
        {
            if (param.type == ChunkParameterType::SupportedExtensions)
            {
                auto& supportedParam = reinterpret_cast<const SupportedExtensionsParameter&>(param);
                for (size_t i = 0; i < supportedParam.getCount(); ++i)
                {
                    logger::debug("supported chunk type %u", _loggableId.c_str(), supportedParam.data()[i]);
                }
            }
            else if (param.type == ChunkParameterType::AuthChunkList)
            {
                for (size_t i = 0; i < param.dataSize(); ++i)
                {
                    logger::debug("chunk type %u must be signed", _loggableId.c_str(), param.data()[i]);
                }
            }
            else if (param.type == ChunkParameterType::HMACAlgorithm)
            {
                for (size_t i = 0; i < param.dataSize(); ++i)
                {
                    const uint16_t algo = param.data()[i];
                    logger::debug("use HMAC algorithm %u", _loggableId.c_str(), algo);
                }
            }
            logger::debug("INIT contains param %x, %u", _loggableId.c_str(), param.type.get(), param.length.get());
        }
        auto localTag = _randomGenerator.next();
        uint16_t inboundStreams = 0;
        uint16_t outboundStreams = 0;
        if (_listener
                ->onSctpInitReceived(this, header.sourcePort, sctpPacket, timestamp, inboundStreams, outboundStreams))
        {
            if (inboundStreams == 0 && outboundStreams == 0)
            {
                logger::warn("SCTP INIT rejected. Stream count 0,0", _loggableId.c_str());
                return; // no streams to add
            }

            SctpCookie cookie(timestamp, localTag, chunk->initTag, chunk->initTSN, chunk->advertisedReceiverWindow);
            cookie.inboundStreams = inboundStreams;
            cookie.outboundStreams = outboundStreams;
            const auto& inboundHeader = sctpPacket.getHeader();
            cookie.sign(_key1, sizeof(_key1), inboundHeader.sourcePort);

            SctpPacketW packet(chunk->initTag, _localPort, header.sourcePort);

            auto& initAck = packet.appendChunk<InitAckChunk>();
            initAck.advertisedReceiverWindow = _config.receiveWindow.initial;
            initAck.inboundStreams = cookie.inboundStreams;
            initAck.outboundStreams = cookie.outboundStreams;
            initAck.initTag = localTag;
            initAck.initTSN = localTag;
            initAck.add(CookieParameter<SctpCookie>(cookie));
            initAck.add(ChunkParameter(ForwardTsnSupport));
            auto& supported = appendParameter<SupportedExtensionsParameter>(initAck);
            supported.add(ChunkType::FORWARDTSN);
            initAck.commitAppendedParameter();
            packet.commitAppendedChunk();

            logger::debug("sending INIT_ACK", _loggableId.c_str());
            send(packet);
        }
        else
        {
            logger::warn("SCTP not accepted %u...", _loggableId.c_str(), header.sourcePort.get());
        }
    }
}

void SctpServerPort::onCookieEchoReceived(const SctpPacket& packet, uint64_t timestamp)
{
    logger::debug("SCTP COOKIE ECHO received", _loggableId.c_str());

    auto* echoChunk = packet.getChunk<CookieEchoChunk>(ChunkType::COOKIE_ECHO);
    if (echoChunk)
    {
        if (echoChunk->header.length != CookieEchoChunk::BASE_HEADER_SIZE + sizeof(SctpCookie))
        {
            logger::error("cookie echoed is invalid size %u", _loggableId.c_str(), echoChunk->header.length.get());
            return;
        }
        auto& header = packet.getHeader();
        const auto cookie = echoChunk->getCookie<SctpCookie>();

        if (timestamp - cookie.timestamp > _config.cookieLifeTime * timer::ms ||
            (!cookie.verifySignature(_key1, sizeof(_key1), header.sourcePort) &&
                !cookie.verifySignature(_key2, sizeof(_key2), header.sourcePort)))
        {
            const uint64_t timeDiff = (timestamp - cookie.timestamp) / timer::ms;
            logger::error("cookie echo did not pass validation, time diff %" PRIu64 "ms, source port %u",
                _loggableId.c_str(),
                timeDiff,
                header.sourcePort.get());
            return;
        }
        _listener->onSctpCookieEchoReceived(this, header.sourcePort.get(), packet, timestamp);
    }
}
} // namespace sctp
