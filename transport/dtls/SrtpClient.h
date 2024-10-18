#pragma once

#include "concurrency/MpmcQueue.h"
#include "concurrency/ScopedMutexGuard.h"
#include "logger/Logger.h"
#include "logger/PruneSpam.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/dtls/DtlsMessageListener.h"
#include "transport/dtls/SrtpProfiles.h"
#include <openssl/ssl.h>
#include <srtp2/srtp.h>

namespace transport
{

class SslDtls;
class SslWriteBioListener;

// It seems sslDtls object is not thread safe when making progress in state machine during connection setup.
// Timers, incoming packets, handshake init have to be synchronized
// The SrtpClient must protect / unprotect at least one packet while ROC is 0. Otherwise, it will not initialize the
// new ssrc.
class SrtpClient : public DtlsMessageListener
{
public:
    enum class State
    {
        IDLE = 0,
        READY,
        CONNECTING,
        CONNECTED,
        FAILED,
        LAST
    };

    class IEvents
    {
    public:
        virtual void onSrtpStateChange(SrtpClient* srtpClient, State state) = 0;
    };

    SrtpClient(SslDtls& sslDtls, IEvents* eventListener);
    ~SrtpClient() override;

    void setSslWriteBioListener(SslWriteBioListener* sslWriteBioListener);
    bool isInitialized() const { return _isInitialized; }
    const logger::LoggableId& getLoggableId() const { return _loggableId; }

    void setRemoteDtlsFingerprint(const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool isDtlsClient);

    void getLocalKey(srtp::Profile profile, srtp::AesKey& keyOut);
    void setRemoteKey(const srtp::AesKey& key);

    bool unprotect(memory::Packet& packet);
    bool protect(memory::Packet& packet);
    void removeLocalSsrc(const uint32_t ssrc);
    static bool shouldSetRolloverCounter(uint32_t previousSequenceNumber, uint32_t sequenceNumber);
    bool setRemoteRolloverCounter(const uint32_t ssrc, const uint32_t rolloverCounter);
    bool setLocalRolloverCounter(const uint32_t ssrc, const uint32_t rolloverCounter);

    bool isConnected() const { return (_state == State::CONNECTED); }
    bool isDtlsClient() const { return _isDtlsClient; }

    void onMessageReceived(memory::UniquePacket packet) override;

    int64_t nextTimeout();
    int64_t processTimeout();

    State getState() const { return _state; }

    bool unprotectApplicationData(memory::Packet& packet);
    void sendApplicationData(const void* data, size_t length);

    srtp::Mode getMode() const { return _mode; }

    void stop();

    bool unprotectFirstRtp(memory::Packet& rtpPacket, uint32_t& rolloverCounter);

private:
    void dtlsHandShake();
    void logSslError(const char* msg, int sslCode);

    bool _isInitialized;
    std::atomic<State> _state;
    logger::LoggableId _loggableId;

    SSL* _ssl;
    BIO* _readBio;
    BIO* _writeBio;
    std::string _remoteDtlsFingerprintType;
    std::string _remoteDtlsFingerprintHash;
    std::atomic_bool _isDtlsClient;
    srtp_t _remoteSrtp;
    srtp_t _localSrtp;

    srtp::Mode _mode;

    srtp::AesKey _localKey;

    IEvents* _eventSink;
    logger::PruneSpam _rtpAntiSpam;
    logger::PruneSpam _rtcpAntiSpam;

    DBGCHECK_SINGLETHREADED_MUTEX(_mutexGuard);

    concurrency::MpmcQueue<memory::UniquePacket> _pendingPackets;

    void sslRead();
    bool compareFingerprint();
    bool createSrtp();
    bool createSrtp(const srtp::AesKey& key);
    static const char* getErrorMessage(int sslErrorCode);
};
} // namespace transport
