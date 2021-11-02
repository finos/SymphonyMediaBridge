#pragma once

#include "concurrency/MpmcQueue.h"
#include "concurrency/ScopedMutexGuard.h"
#include "logger/Logger.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/dtls/DtlsMessageListener.h"
#include "transport/dtls/SslDtls.h"
#include "transport/dtls/SslWriteBioListener.h"
#include <atomic>
#include <memory>
#include <openssl/srtp.h>
#include <openssl/ssl.h>
#include <srtp2/srtp.h>
#include <string>

namespace transport
{

class SslDtls;

// It seems sslDtls object is not thread safe when making progress in state machine during connection setup.
// Timers, incoming packets, handshake init have to be synchronized
class SrtpClient : public DtlsMessageListener
{
public:
    enum class State
    {
        IDLE = 0,
        READY,
        CONNECTING,
        CONNECTED,
        FAILED
    };

    class IEvents
    {
    public:
        virtual void onDtlsStateChange(SrtpClient* srtpClient, State state) = 0;
    };

    SrtpClient(SslDtls& sslDtls, IEvents* eventListener, memory::PacketPoolAllocator& allocator);
    ~SrtpClient() override;

    void setSslWriteBioListener(SslWriteBioListener* sslWriteBioListener);
    bool isInitialized() const { return _isInitialized; }
    const logger::LoggableId& getLoggableId() const { return _loggableId; }

    void setRemoteDtlsFingerprint(const std::string& fingerprintType,
        const std::string& fingerprintHash,
        const bool isDtlsClient);

    bool unprotect(memory::Packet* packet);
    bool protect(memory::Packet* packet);
    void removeLocalSsrc(const uint32_t ssrc);
    bool setRemoteRolloverCounter(const uint32_t ssrc, const uint32_t rolloverCounter);
    bool setLocalRolloverCounter(const uint32_t ssrc, const uint32_t rolloverCounter);

    bool isDtlsConnected() const { return (_state == State::CONNECTED); }
    bool isDtlsClient() const { return _isDtlsClient; }

    void onMessageReceived(const char* buffer, const size_t length) override;

    int64_t nextTimeout();
    int64_t processTimeout();

    State getState() const { return _state; }

    bool unprotectApplicationData(memory::Packet* packet);
    void sendApplicationData(const void* data, size_t length);

private:
    void dtlsHandShake();
    void logSslError(const char* msg, int sslCode);
    bool _isInitialized;
    std::atomic<State> _state;
    logger::LoggableId _loggableId;
    SslDtls& _sslDtls;

    SSL* _ssl;
    BIO* _readBio;
    BIO* _writeBio;
    std::string _remoteDtlsFingerprintType;
    std::string _remoteDtlsFingerprintHash;
    std::atomic_bool _isDtlsClient;
    srtp_t _remoteSrtp;
    srtp_t _localSrtp;

    bool _nullCipher;

    IEvents* _eventSink;

    DBGCHECK_SINGLETHREADED_MUTEX(_mutexGuard);

    memory::PacketPoolAllocator& _allocator;
    concurrency::MpmcQueue<memory::Packet*> _pendingPackets;

    void sslRead();
    bool compareFingerprint();
    bool createSrtp();
    static const char* getErrorMessage(int sslErrorCode);
};

const char* toString(const SrtpClient::State state);
} // namespace transport
