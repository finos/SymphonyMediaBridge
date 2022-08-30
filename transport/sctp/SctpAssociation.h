#pragma once
#include <atomic>
#include <memory>
#include <tuple>
namespace sctp
{
struct SctpConfig;
class SctpPacket;
class SctpServerPort;
struct CauseCode;

class SctpAssociation
{
public:
    virtual ~SctpAssociation() {}
    enum class State
    {
        CLOSED,
        COOKIE_WAIT,
        COOKIE_ECHOED,
        ESTABLISHED,
        SHUTDOWN_PENDING,
        SHUTDOWN_SENT,
        SHUTDOWN_RECEIVED,
        SHUTDOWN_ACK_SENT
    };

    class IEvents
    {
    public:
        virtual void onSctpStateChanged(SctpAssociation* session, State state) = 0;
        virtual void onSctpFragmentReceived(SctpAssociation* session,
            uint16_t streamId,
            uint16_t streamSequenceNumber,
            uint32_t payloadProtocol,
            const void* buffer,
            size_t length,
            uint64_t timestamp) = 0;
        virtual void onSctpEstablished(SctpAssociation* session) = 0;
        virtual void onSctpClosed(SctpAssociation* session) = 0;
        virtual void onSctpChunkDropped(SctpAssociation* session, size_t size) = 0;
    };

    virtual void onCookieEcho(const SctpPacket& sctpPacket, const uint64_t timestamp) = 0;

    virtual void connect(uint16_t inboundStreamCount, uint16_t outboundStreamCount, uint64_t timestamp) = 0;
    virtual uint16_t allocateStream() = 0;
    virtual bool sendMessage(uint16_t streamId,
        uint32_t payloadProtocol,
        const void* payloadData,
        size_t length,
        uint64_t timestamp) = 0;
    virtual size_t outboundPendingSize() const = 0;
    virtual int64_t nextTimeout(uint64_t timestamp) = 0;
    virtual int64_t processTimeout(uint64_t timestamp) = 0;
    virtual State getState() const = 0;

    virtual int64_t onPacketReceived(const SctpPacket&, uint64_t timestamp) = 0;

    virtual void sendErrorResponse(const SctpPacket& request, const CauseCode& errorCause) = 0;

    virtual std::tuple<uint16_t, uint16_t> getPortPair() const = 0;
    virtual std::tuple<uint32_t, uint32_t> getTags() const = 0;

    virtual void startMtuProbing(uint64_t timestamp) = 0;
    virtual uint32_t getScptMTU() const = 0;

    virtual void setAdvertisedReceiveWindow(uint32_t size) = 0;
    virtual void close() = 0;
    virtual size_t getStreamCount() const = 0;
};

const char* toString(SctpAssociation::State state);

std::unique_ptr<SctpAssociation> createSctpAssociation(size_t logId,
    SctpServerPort& transport,
    uint16_t remotePort,
    SctpAssociation::IEvents* listener,
    const SctpConfig& config);

/** You must call onCookieEcho on the SctpAssociation after this
 */
std::unique_ptr<SctpAssociation> createSctpAssociation(size_t logId,
    SctpServerPort& transport,
    const SctpPacket& cookieEcho,
    SctpAssociation::IEvents* listener,
    const SctpConfig& config);
} // namespace sctp
