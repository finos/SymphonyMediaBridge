#pragma once

#include "logger/Logger.h"
#include "utils/ByteOrder.h"
#include "utils/MersienneRandom.h"
#include <cstddef>
#include <cstdint>

namespace crypto
{
class HMAC;
}
namespace sctp
{
struct SctpConfig;
class SctpPacket;
class SctpPacketW;

class DatagramTransport
{
public:
    virtual bool sendSctpPacket(const void* data, size_t length) = 0;
};

// Pipe all incoming packets to the server port onPacketReceived.
// It will use IEvents to sort out if this packet belongs
// to a session ot not. The exception is the init packet that may belong to
// a session and you can sort it out using the tag.
// All timestamps in nanoseconds
class SctpServerPort
{
public:
    class IEvents
    {
    public:
        // configure the stream counts in the cookie and return true if this request should entail a new session setup
        virtual bool onSctpInitReceived(SctpServerPort* serverPort,
            uint16_t srcPort,
            const SctpPacket& sctpPacket,
            uint64_t timestamp,
            uint16_t& inboundStreams,
            uint16_t& outboundStreams) = 0;

        // create an SctpAssociation with the cookie if acceptable
        virtual void onSctpCookieEchoReceived(SctpServerPort* serverPort,
            uint16_t srcPort,
            const SctpPacket& packet,
            uint64_t timestamp) = 0;

        // packet aimed for an Association
        virtual void onSctpReceived(SctpServerPort* serverPort,
            uint16_t srcPort,
            const SctpPacket& sctpPacket,
            uint64_t timestamp) = 0;
    };

public:
    SctpServerPort(size_t logId,
        DatagramTransport* transport,
        IEvents* listener,
        uint16_t localPort,
        const SctpConfig& config,
        uint64_t timestamp);

    void onPacketReceived(const void* data, size_t length, uint64_t timestamp);
    uint16_t getPort() const { return _localPort; }

    const uint8_t* getCurrentCookieSignKey() const { return _key1; };
    size_t getSignKeyLength() const { return sizeof(_key1); }

    void send(SctpPacketW& packet);

    const SctpConfig& getConfig() const { return _config; }

private:
    void rotateKeys();
    void onInitReceived(const SctpPacket& packet, uint64_t timestamp);
    void onCookieEchoReceived(const SctpPacket& packet, uint64_t timestamp);

private:
    logger::LoggableId _loggableId;
    const SctpConfig& _config;
    uint16_t _localPort;
    DatagramTransport* _transport;
    IEvents* _listener;
    uint8_t _key1[20];
    uint8_t _key2[20];
    uint64_t _nextKeyRotation;
    utils::MersienneRandom<uint32_t> _randomGenerator;
};

class SctpCookie
{
public:
    struct TagPair
    {
        TagPair() : local(0), peer(0) {}
        TagPair(const TagPair&) = default;
        TagPair(uint32_t local_, uint32_t peer_) : local(local_), peer(peer_) {}

        TagPair& operator=(const TagPair&) = default;

        nwuint32_t local;
        nwuint32_t peer;
    };

    SctpCookie();
    SctpCookie(const SctpCookie&) = default;
    SctpCookie(uint64_t timestamp_, uint32_t localTag, uint32_t peerTag, uint32_t peerTSN_, uint32_t peerRwnd);
    SctpCookie& operator=(const SctpCookie&) = default;

    nwuint16_t outboundStreams;
    nwuint16_t inboundStreams;
    nwuint64_t timestamp;
    TagPair tieTag;
    TagPair tag;
    nwuint32_t peerTSN;
    nwuint32_t peerReceiveWindow;

    void sign(const uint8_t* key, size_t keyLength, uint16_t remotePort);
    bool verifySignature(const uint8_t* key, size_t keyLength, uint16_t remotePort) const;

private:
    void addSignedItems(crypto::HMAC& signer, uint16_t remotePort) const;
    uint8_t hmac[20];
};

} // namespace sctp
