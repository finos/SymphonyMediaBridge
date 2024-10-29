#include "SrtpClient.h"
#include "SslDtls.h"
#include "concurrency/ScopedMutexGuard.h"
#include "crypto/SslHelper.h"
#include "logger/Logger.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "utils/CheckedCast.h"
#include "utils/Time.h"
#include <openssl/err.h>
#include <openssl/rand.h>
#include <srtp2/srtp.h>

namespace
{

void sslInfoCallback(const SSL* ssl, int32_t where, int32_t)
{
    if (!(where & SSL_CB_ALERT))
    {
        return;
    }

    auto srtpClient = static_cast<transport::SrtpClient*>(SSL_get_ex_data(ssl, 0));
    if (srtpClient)
    {
        logger::info("sslInfo %s", srtpClient->getLoggableId().c_str(), SSL_state_string_long(ssl));
    }
}

const auto MTU = 1500;

const size_t srtpMasterKeyLength = 16;
const size_t srtpSaltLength = 14;
constexpr size_t keyingMaterialSize = srtpMasterKeyLength * 2 + srtpSaltLength * 2;

} // namespace

namespace transport
{

SrtpClient::SrtpClient(SslDtls& sslDtls, IEvents* eventListener)
    : _isInitialized(false),
      _state(State::IDLE),
      _loggableId("SrtpClient"),
      _ssl(SSL_new(sslDtls.getSslContext())),
      _readBio(nullptr),
      _writeBio(nullptr),
      _isDtlsClient(true),
      _remoteSrtp(nullptr),
      _localSrtp(nullptr),
      _mode(srtp::Mode::UNDEFINED),
      _eventSink(eventListener),
      _rtpAntiSpam(10, 100),
      _rtcpAntiSpam(10, 100),
      _pendingPackets(32)
{
    assert(_ssl);
    if (!_ssl)
    {
        return;
    }
    SSL_set_ex_data(_ssl, 0, this);
    SSL_set_info_callback(_ssl, ::sslInfoCallback);

    _readBio = BIO_new(BIO_s_mem());
    assert(_readBio);
    if (!_readBio)
    {
        return;
    }
    BIO_set_mem_eof_return(_readBio, -1);

    _writeBio = BIO_new(sslDtls.getWriteBioMethods());
    assert(_writeBio);
    if (!_writeBio)
    {
        return;
    }
    SSL_set_bio(_ssl, _readBio, _writeBio);

    RAND_bytes(_localKey.keySalt, sizeof(_localKey.keySalt));

    _isInitialized = true;
}

SrtpClient::~SrtpClient()
{
    BIO_set_data(_writeBio, nullptr);
    SSL_free(_ssl);

    if (_localSrtp)
    {
        srtp_dealloc(_localSrtp);
    }
    if (_remoteSrtp)
    {
        srtp_dealloc(_remoteSrtp);
    }
}

void SrtpClient::setRemoteDtlsFingerprint(const std::string& fingerprintType,
    const std::string& fingerprintHash,
    const bool isDtlsClient)
{
    assert(_isInitialized);
    if (_state != State::IDLE)
    {
        logger::warn("SrtpClient already in progress. Cannot set DTLS fingerprints", _loggableId.c_str());
        return;
    }

    if (fingerprintType.empty())
    {
        _mode = srtp::Mode::NULL_CIPHER;
        _state = State::CONNECTED;
        logger::info("Setting empty fingerprint. Disabling DTLS.", _loggableId.c_str());
        if (_eventSink)
        {
            _eventSink->onSrtpStateChange(this, _state);
        }
        return;
    }

    _remoteDtlsFingerprintType = fingerprintType;
    _remoteDtlsFingerprintHash = fingerprintHash;
    _isDtlsClient = isDtlsClient;

    _mode = srtp::Mode::DTLS;
    if (_isDtlsClient)
    {
        logger::info("DTLS ready as client", _loggableId.c_str());
        SSL_set_connect_state(_ssl);
        _state = State::READY;
    }
    else
    {
        logger::info("DTLS ready as server", _loggableId.c_str());
        SSL_set_accept_state(_ssl);
        _state = State::CONNECTING;
    }

    if (_eventSink)
    {
        _eventSink->onSrtpStateChange(this, _state);
    }

    for (memory::UniquePacket packet; _state == State::CONNECTING && _pendingPackets.pop(packet);)
    {
        logger::debug("forwarding pending DTLS message", _loggableId.c_str());
        onMessageReceived(std::move(packet));
    }
}

void SrtpClient::setSslWriteBioListener(SslWriteBioListener* sslWriteBioListener)
{
    assert(_isInitialized);
    BIO_set_data(_writeBio, sslWriteBioListener);
}

void SrtpClient::dtlsHandShake()
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    assert(_isInitialized);
    if (_mode != srtp::Mode::DTLS)
    {
        logger::debug("null cipher. no handshake", _loggableId.c_str());
        return;
    }

    if (_state != State::READY)
    {
        logger::warn("not ready to do DTLS handshake. Credentials are missing", _loggableId.c_str());
        return;
    }

    _state = State::CONNECTING;
    const int sslResult = SSL_do_handshake(_ssl);
    if (sslResult == 0)
    {
        logger::warn("SSL handshake aborted %s", _loggableId.c_str(), getErrorMessage(SSL_get_error(_ssl, sslResult)));
    }
    else if (sslResult < 0)
    {
        const auto sslErrorCode = SSL_get_error(_ssl, sslResult);
        if (sslErrorCode != SSL_ERROR_WANT_READ && sslErrorCode != SSL_ERROR_WANT_WRITE)
        {
            logger::error("SSL handshake failed %d %s", _loggableId.c_str(), sslResult, getErrorMessage(sslErrorCode));
        }
    }

    if (_eventSink)
    {
        _eventSink->onSrtpStateChange(this, _state);
    }

    for (memory::UniquePacket packet; _state == State::CONNECTING && _pendingPackets.pop(packet);)
    {
        onMessageReceived(std::move(packet));
    }
}

// -1 means no more timeouts
int64_t SrtpClient::nextTimeout()
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    if (_state < State::CONNECTING)
    {
        return utils::Time::ms * 100;
    }

    struct timeval timeout;
    if (1 == DTLSv1_get_timeout(_ssl, &timeout))
    {
        const uint64_t ns = 1000000000ull;
        return timeout.tv_sec * ns + timeout.tv_usec * 1000ull;
    }
    else
    {
        return -1;
    }
}

int64_t SrtpClient::processTimeout()
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    if (_state == State::IDLE)
    {
        // waiting for credentials
        return utils::Time::ms * 100;
    }
    else if (_state == State::READY)
    {
        logger::debug("starting DTLS handshake as %s", _loggableId.c_str(), isDtlsClient() ? "client" : "server");
        dtlsHandShake();
    }

    auto rc = DTLSv1_handle_timeout(_ssl);
    if (rc == -1)
    {
        // If too many timeouts had expired without progress or an error occurs, it returns -1.
        // If the function returns -1, checking if SSL_get_error returns SSL_ERROR_WANT_WRITE
        // may be used to determine if the retransmit failed due to a non-fatal error at the write BIO.
        // However, the operation may not be retried until the next timeout fires
        const auto err = SSL_get_error(_ssl, rc);
        const bool isFatalError = (err != SSL_ERROR_WANT_WRITE);

        logger::error("DTLS timeout error %s ,isFatal: %s",
            _loggableId.c_str(),
            getErrorMessage(err),
            isFatalError ? "t" : "f");
        if (isFatalError)
        {
            _state = State::FAILED;
            if (_eventSink)
            {
                _eventSink->onSrtpStateChange(this, _state);
            }

            return -1;
        }
    }
    return nextTimeout();
}

bool SrtpClient::unprotect(memory::Packet& packet)
{
    assert(_isInitialized);

    if (_mode == srtp::Mode::NULL_CIPHER)
    {
        return true;
    }

    if (!_localSrtp || !_remoteSrtp || _state != State::CONNECTED)
    {
        return false;
    }

    // srtp_unprotect assumes data is word aligned
    assert(memory::isAligned<uint32_t>(packet.get()));

    DBGCHECK_SINGLETHREADED(_mutexGuard);

    auto bufferLength = utils::checkedCast<int32_t>(packet.getLength());
    if (rtp::isRtpPacket(packet))
    {
        const auto result = srtp_unprotect(_remoteSrtp, packet.get(), &bufferLength);
        if (result != srtp_err_status_ok)
        {
            if (_rtpAntiSpam.canLog())
            {
                const auto header = rtp::RtpHeader::fromPacket(packet);
                logger::warn("srtp unprotect error: %d, ssrc %u, seq %u, ts %u",
                    _loggableId.c_str(),
                    static_cast<int32_t>(result),
                    header != nullptr ? header->ssrc.get() : 0,
                    header != nullptr ? header->sequenceNumber.get() : 0,
                    header != nullptr ? header->timestamp.get() : 0);
            }
            return false;
        }
    }
    else if (rtp::isRtcpPacket(packet))
    {
        const auto result = srtp_unprotect_rtcp(_remoteSrtp, packet.get(), &bufferLength);
        if (result != srtp_err_status_ok)
        {
            if (_rtcpAntiSpam.canLog())
            {
                auto header = rtp::RtcpHeader::fromPacket(packet);
                logger::warn("srtp unprotect error type %u, %d",
                    _loggableId.c_str(),
                    header ? header->packetType : 0,
                    result);
                if (header->packetType == rtp::RtcpPacketType::SENDER_REPORT)
                {
                    auto sr = reinterpret_cast<rtp::RtcpSenderReport*>(header);
                    logger::warn("failed to decrypt SR %u", _loggableId.c_str(), static_cast<uint32_t>(sr->ssrc));
                }
            }
            return false;
        }
    }
    else
    {
        assert(false);
        logger::error("packet is neither RTP nor RTCP. unprotect", _loggableId.c_str());
        return false;
    }

    packet.setLength(utils::checkedCast<size_t>(bufferLength));
    return true;
}

bool SrtpClient::protect(memory::Packet& packet)
{
    assert(_isInitialized);

    if (_mode == srtp::Mode::NULL_CIPHER)
    {
        return true;
    }

    if (!_localSrtp || !_remoteSrtp || _state != State::CONNECTED)
    {
        return false;
    }

    // srtp_protect assumes data is word aligned
    assert(memory::isAligned<uint32_t>(packet.get()));

    DBGCHECK_SINGLETHREADED(_mutexGuard);

    auto bufferLength = utils::checkedCast<int32_t>(packet.getLength());
    assert(bufferLength > 0);
    if (rtp::isRtpPacket(packet))
    {
        const auto result = srtp_protect(_localSrtp, packet.get(), &bufferLength);
        if (result != srtp_err_status_ok)
        {
            const auto rtpHeader = rtp::RtpHeader::fromPacket(packet);
            logger::warn("Srtp protect error: %d rtp ssrc %u, type %u, seqno %u, timestamp %u",
                _loggableId.c_str(),
                static_cast<int32_t>(result),
                rtpHeader->ssrc.get(),
                static_cast<uint>(rtpHeader->payloadType),
                rtpHeader->sequenceNumber.get(),
                rtpHeader->timestamp.get());

            return false;
        }
    }
    else if (rtp::isRtcpPacket(packet))
    {
        const auto result = srtp_protect_rtcp(_localSrtp, packet.get(), &bufferLength);
        if (result != srtp_err_status_ok)
        {
            auto header = rtp::RtcpHeader::fromPacket(packet);
            logger::info("rtcp type %u", _loggableId.c_str(), header->packetType);
            if (header->packetType == rtp::RtcpPacketType::SENDER_REPORT)
            {
                auto sr = reinterpret_cast<rtp::RtcpSenderReport*>(header);
                logger::info("SR pkts %u", _loggableId.c_str(), static_cast<uint32_t>(sr->packetCount));
            }

            return false;
        }
    }
    else
    {
        assert(false);
        logger::error("packet is neither RTP nor RTCP. protect", _loggableId.c_str());
        return false;
    }

    packet.setLength(utils::checkedCast<size_t>(bufferLength));
    return true;
}

void SrtpClient::removeLocalSsrc(const uint32_t ssrc)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    const auto result = srtp_remove_stream(_localSrtp, hton(ssrc));
    if (result == srtp_err_status_ok)
    {
        logger::info("Remove ssrc %u from local srtp context", _loggableId.c_str(), ssrc);
    }
}

bool SrtpClient::shouldSetRolloverCounter(uint32_t previousSequenceNumber, uint32_t sequenceNumber)
{
    if (!(sequenceNumber & 0xFFFF0000u) && !(previousSequenceNumber & 0xFFFF0000u))
    {
        return false; // cannot set ROC 0
    }

    if (previousSequenceNumber >> 15 == sequenceNumber >> 15)
    {
        // seqno wraps on 16 bit, but to avoid bugs after long gaps it is better to set it also after 32768
        return false;
    }

    return true;
}

/**
 * ROC is automatically deduced from continuous inbound stream. But if there are gaps or reordering in the sequence,
 * the roc must be set explicitly.
 */
bool SrtpClient::setRemoteRolloverCounter(const uint32_t ssrc, const uint32_t rolloverCounter)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    if (_mode <= srtp::Mode::NULL_CIPHER)
    {
        return true;
    }

    const auto result = srtp_set_stream_roc(_remoteSrtp, ssrc, rolloverCounter);
    if (result != srtp_err_status_ok)
    {
        logger::error("Unable to set rolloverCounter %u for ssrc %u: %d",
            _loggableId.c_str(),
            rolloverCounter,
            ssrc,
            result);
        return false;
    }

    return true;
}

bool SrtpClient::setLocalRolloverCounter(const uint32_t ssrc, const uint32_t rolloverCounter)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    if (_mode <= srtp::Mode::NULL_CIPHER)
    {
        return true;
    }

    const auto result = srtp_set_stream_roc(_localSrtp, ssrc, rolloverCounter);
    if (result != srtp_err_status_ok)
    {
        logger::error("Unable to set rolloverCounter %u for ssrc %u: %d",
            _loggableId.c_str(),
            rolloverCounter,
            ssrc,
            result);
        return false;
    }

    return true;
}

void SrtpClient::sslRead()
{
    assert(_isInitialized);
    char data[MTU];
    memset(data, 0, MTU);

    ERR_clear_error();
    const auto result = SSL_read(_ssl, data, MTU);
    if (result < 0)
    {
        const auto sslError = SSL_get_error(_ssl, result);
        if (sslError == SSL_ERROR_SYSCALL)
        {
            logger::error("sslRead %u, %s", _loggableId.c_str(), errno, strerror(errno));
        }
        if (sslError != SSL_ERROR_WANT_READ)
        {
            logSslError("sslRead", sslError);
        }
        if (sslError == SSL_ERROR_SSL)
        {
            _state = State::FAILED;

            if (_eventSink)
            {
                _eventSink->onSrtpStateChange(this, _state);
            }
        }
    }
}

void SrtpClient::logSslError(const char* msg, int sslCode)
{
    logger::error("%s SSL_ERROR %d %s", _loggableId.c_str(), msg, sslCode, getErrorMessage(sslCode));
}

bool SrtpClient::compareFingerprint()
{
    assert(_isInitialized);
    auto peerCertificate = SSL_get_peer_certificate(_ssl);
    if (!peerCertificate)
    {
        logger::error("Unable to get peerCertificate %s",
            _loggableId.c_str(),
            ERR_reason_error_string(ERR_get_error()));
        return false;
    }

    uint32_t fingerprintSize = 0;
    unsigned char fingerprint[EVP_MAX_MD_SIZE];

    if (_remoteDtlsFingerprintType == "sha-256")
    {
        X509_digest(peerCertificate, EVP_sha256(), fingerprint, &fingerprintSize);
    }
    else
    {
        X509_digest(peerCertificate, EVP_sha1(), fingerprint, &fingerprintSize);
    }
    X509_free(peerCertificate);

    const auto peerCertificateFingerprint = makeFingerprintString(fingerprint, fingerprintSize);
    return _remoteDtlsFingerprintHash == peerCertificateFingerprint;
}

bool SrtpClient::createSrtp()
{
    assert(_isInitialized);
    auto srtpProtectionProfile = SSL_get_selected_srtp_profile(_ssl);
    if (!srtpProtectionProfile)
    {
        logger::error("No selected srtp profile", _loggableId.c_str());
        return false;
    }

    unsigned char keyingMaterial[keyingMaterialSize];

    if (SSL_export_keying_material(_ssl,
            keyingMaterial,
            keyingMaterialSize,
            "EXTRACTOR-dtls_srtp",
            strlen("EXTRACTOR-dtls_srtp"),
            nullptr,
            0,
            0) != 1)
    {
        return false;
    }

    unsigned char clientWriteKey[srtpMasterKeyLength + srtpSaltLength];
    unsigned char serverWriteKey[srtpMasterKeyLength + srtpSaltLength];

    {
        size_t offset = 0;
        std::memcpy(&(clientWriteKey[0]), &(keyingMaterial[offset]), srtpMasterKeyLength);
        offset += srtpMasterKeyLength;

        std::memcpy(&(serverWriteKey[0]), &(keyingMaterial[offset]), srtpMasterKeyLength);
        offset += srtpMasterKeyLength;

        std::memcpy(&(clientWriteKey[srtpMasterKeyLength]), &(keyingMaterial[offset]), srtpSaltLength);
        offset += srtpSaltLength;

        std::memcpy(&(serverWriteKey[srtpMasterKeyLength]), &(keyingMaterial[offset]), srtpSaltLength);
    }

    srtp_policy_t srtpPolicy;
    memset(&srtpPolicy, 0, sizeof(srtpPolicy));

    switch (srtpProtectionProfile->id)
    {
    case SRTP_AES128_CM_SHA1_80:
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&srtpPolicy.rtp);
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&srtpPolicy.rtcp);
        break;
    case SRTP_AES128_CM_SHA1_32:
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&srtpPolicy.rtp);
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&srtpPolicy.rtcp);
        break;
    default:
        assert(false);
        break;
    }

    srtpPolicy.ssrc.value = 0;
    srtpPolicy.next = nullptr;
    srtpPolicy.ssrc.type = ssrc_any_outbound;
    srtpPolicy.key = !!_isDtlsClient ? clientWriteKey : serverWriteKey;

    auto createResult = srtp_create(&_localSrtp, &srtpPolicy);
    if (createResult != srtp_err_status_ok)
    {
        logger::error("Failed to create localSrtp: %d", _loggableId.c_str(), createResult);
        return false;
    }

    srtpPolicy.ssrc.type = ssrc_any_inbound;
    srtpPolicy.key = !!_isDtlsClient ? serverWriteKey : clientWriteKey;

    createResult = srtp_create(&_remoteSrtp, &srtpPolicy);
    if (createResult != srtp_err_status_ok)
    {
        logger::error("Failed to create remoteSrtp: %d", _loggableId.c_str(), createResult);
        return false;
    }

    return true;
}

/**
 * @param remoteKey key and salt
 */
bool SrtpClient::createSrtp(const srtp::AesKey& remoteKey)
{
    srtp_policy_t srtpPolicy;
    memset(&srtpPolicy, 0, sizeof(srtpPolicy));

    _localKey.profile = remoteKey.profile;
    switch (remoteKey.profile)
    {
    case srtp::Profile::AES128_CM_SHA1_32:
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&srtpPolicy.rtp);
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&srtpPolicy.rtcp);
        break;
    case srtp::Profile::AES128_CM_SHA1_80:
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&srtpPolicy.rtp);
        srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&srtpPolicy.rtcp);
        break;
    case srtp::Profile::AES_192_CM_SHA1_32:
        srtp_crypto_policy_set_aes_cm_192_hmac_sha1_32(&srtpPolicy.rtp);
        srtp_crypto_policy_set_aes_cm_192_hmac_sha1_80(&srtpPolicy.rtcp);
        break;
    case srtp::Profile::AES_192_CM_SHA1_80:
        srtp_crypto_policy_set_aes_cm_192_hmac_sha1_80(&srtpPolicy.rtp);
        srtp_crypto_policy_set_aes_cm_192_hmac_sha1_80(&srtpPolicy.rtcp);
        break;
    case srtp::Profile::AES_256_CM_SHA1_32:
        srtp_crypto_policy_set_aes_cm_256_hmac_sha1_32(&srtpPolicy.rtp);
        srtp_crypto_policy_set_aes_cm_256_hmac_sha1_80(&srtpPolicy.rtcp);
        break;
    case srtp::Profile::AES_256_CM_SHA1_80:
        srtp_crypto_policy_set_aes_cm_256_hmac_sha1_80(&srtpPolicy.rtp);
        srtp_crypto_policy_set_aes_cm_256_hmac_sha1_80(&srtpPolicy.rtcp);
        break;
    case srtp::Profile::AEAD_AES_128_GCM:
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&srtpPolicy.rtp);
        srtp_crypto_policy_set_aes_gcm_128_16_auth(&srtpPolicy.rtcp);
        break;
    case srtp::Profile::AEAD_AES_256_GCM:
        srtp_crypto_policy_set_aes_gcm_256_16_auth(&srtpPolicy.rtp);
        srtp_crypto_policy_set_aes_gcm_256_16_auth(&srtpPolicy.rtcp);
        break;
    default:
        assert(false);
        return false;
    }

    srtpPolicy.ssrc.value = 0;
    srtpPolicy.next = nullptr;
    srtpPolicy.ssrc.type = ssrc_any_outbound;
    srtpPolicy.key = _localKey.keySalt;

    auto createResult = srtp_create(&_localSrtp, &srtpPolicy);
    if (createResult != srtp_err_status_ok)
    {
        logger::error("Failed to create localSrtp: %d", _loggableId.c_str(), createResult);
        return false;
    }

    srtpPolicy.ssrc.type = ssrc_any_inbound;
    srtpPolicy.key = const_cast<unsigned char*>(remoteKey.keySalt);

    createResult = srtp_create(&_remoteSrtp, &srtpPolicy);
    if (createResult != srtp_err_status_ok)
    {
        logger::error("Failed to create remoteSrtp: %d", _loggableId.c_str(), createResult);
        return false;
    }

    _mode = srtp::Mode::SDES;

    return true;
}

void SrtpClient::onMessageReceived(memory::UniquePacket packet)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    assert(_isInitialized);

    if (_state != State::CONNECTING && _state != State::CONNECTED)
    {
        logger::debug("DTLS received when not ready", _loggableId.c_str());
        _pendingPackets.push(std::move(packet));
        return;
    }

    if (_mode == srtp::Mode::NULL_CIPHER)
    {
        logger::debug("null cipher postponing message", _loggableId.c_str());
        return;
    }

    const int sslResult = BIO_write(_readBio, packet->get(), utils::checkedCast<int32_t>(packet->getLength()));
    if (sslResult <= 0)
    {
        logSslError("Failed to process message", SSL_get_error(_ssl, sslResult));
    }
    sslRead();

    if (_state == State::CONNECTED)
    {
        return;
    }

    if (!SSL_is_init_finished(_ssl))
    {
        return;
    }

    if (!compareFingerprint())
    {
        logger::error("Dtls fingerprint mismatch", _loggableId.c_str());
        return;
    }
    else
    {
        logger::debug("Dtls fingerprint match", _loggableId.c_str());
    }

    if (!createSrtp())
    {
        logger::error("Failed to create srtp", _loggableId.c_str());
        return;
    }

    _state = State::CONNECTED;
    if (_eventSink)
    {
        logger::info("negotiated version %s", _loggableId.c_str(), SSL_get_version(_ssl));
        _eventSink->onSrtpStateChange(this, _state);
    }
}

bool SrtpClient::unprotectApplicationData(memory::Packet& packet)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    assert(*packet.get() == transport::DTLSContentType::applicationData);
    if (_mode != srtp::Mode::DTLS)
    {
        return false;
    }
    if (_state != State::CONNECTED)
    {
        return false;
    }

    BIO_write(_readBio, packet.get(), utils::checkedCast<int32_t>(packet.getLength()));
    ERR_clear_error();
    auto bytesRead = SSL_read(_ssl, packet.get(), packet.size);
    if (bytesRead > 0)
    {
        assert(static_cast<size_t>(bytesRead) <= packet.getLength());
        packet.setLength(bytesRead);
        return true;
    }
    else
    {
        const auto sslError = SSL_get_error(_ssl, bytesRead);
        if (sslError != SSL_ERROR_WANT_READ)
        {
            logSslError("unprotectApplicationData", sslError);
        }
        if (sslError == SSL_ERROR_SSL)
        {
            _state = State::FAILED;
        }
        return false;
    }
}

void SrtpClient::sendApplicationData(const void* data, size_t length)
{
    DBGCHECK_SINGLETHREADED(_mutexGuard);
    if (_mode != srtp::Mode::DTLS)
    {
        return;
    }

    auto bytesWritten = SSL_write(_ssl, data, length);
    if (bytesWritten < 0)
    {
        const auto sslError = SSL_get_error(_ssl, bytesWritten);
        logSslError("sslWrite", sslError);
    }
}

const char* SrtpClient::getErrorMessage(int sslErrorCode)
{
    if (sslErrorCode == SSL_ERROR_SYSCALL)
    {
        return strerror(errno);
    }
    switch (sslErrorCode)
    {
    case SSL_ERROR_NONE:
        return "success";
    case SSL_ERROR_SSL:
        return "SSL";
    case SSL_ERROR_WANT_READ:
        return "want read";
    case SSL_ERROR_WANT_WRITE:
        return "want write";
    case SSL_ERROR_WANT_X509_LOOKUP:
        return "want X509 lookup";
    case SSL_ERROR_ZERO_RETURN:
        return "zero return";
    case SSL_ERROR_WANT_CONNECT:
        return "connect";
    case SSL_ERROR_WANT_ACCEPT:
        return "accept";
    case SSL_ERROR_WANT_ASYNC:
        return "async";
    case SSL_ERROR_WANT_ASYNC_JOB:
        return "async job";
    case SSL_ERROR_WANT_CLIENT_HELLO_CB:
        return "want client hello";
    }

    return "unkown";
}

void SrtpClient::getLocalKey(srtp::Profile profile, srtp::AesKey& keyOut)
{
    keyOut.profile = profile;
    if (keyOut.getKeyLength() == 0)
    {
        return;
    }

    std::memcpy(keyOut.keySalt, _localKey.keySalt, keyOut.getLength());
}

void SrtpClient::setRemoteKey(const srtp::AesKey& key)
{
    if (_state != State::IDLE)
    {
        assert(false);
        logger::warn("cannot set remote SDES key once SrtpClient has been initiated", _loggableId.c_str());
        return;
    }

    if (key.profile == srtp::Profile::NULL_CIPHER)
    {
        _state = State::CONNECTED;
        _mode = srtp::Mode::NULL_CIPHER;
        logger::debug("SRTP connected with null cipher", _loggableId.c_str());
        if (_eventSink)
        {
            _eventSink->onSrtpStateChange(this, _state);
        }
        return;
    }

    if (!createSrtp(key))
    {
        logger::error("failed to create srtp context", _loggableId.c_str());
    }
    _state = State::CONNECTED;
    logger::debug("SRTP connected with SDES", _loggableId.c_str());
    if (_eventSink)
    {
        _eventSink->onSrtpStateChange(this, _state);
    }
}

void SrtpClient::stop()
{
    _state = State::IDLE;
}

/** This method is used decrypt the first packet received in case it may have ROC > 0.
 * libsrtp must see a packet with ROC=0. This problem can happen if the seqno starts near 65535 and some packets are
 * lost. Do not use this method if you have already decrypted the stream before as it will remove the stream if it
 * exists.
 */
bool SrtpClient::unprotectFirstRtp(memory::Packet& packet, uint32_t& rolloverCounter)
{
    assert(_isInitialized);

    if (_mode == srtp::Mode::NULL_CIPHER)
    {
        return true;
    }

    if (!_localSrtp || !_remoteSrtp || _state != State::CONNECTED)
    {
        return false;
    }

    // srtp_unprotect assumes data is word aligned
    assert(memory::isAligned<uint32_t>(packet.get()));

    DBGCHECK_SINGLETHREADED(_mutexGuard);

    if (!rtp::isRtpPacket(packet))
    {
        assert(false);
        logger::error("packet is not RTP. unprotectRtpAgain", _loggableId.c_str());
        return false;
    }

    auto bufferLength = utils::checkedCast<int32_t>(packet.getLength());
    const auto result = srtp_unprotect(_remoteSrtp, packet.get(), &bufferLength);
    if (result == srtp_err_status_ok)
    {
        return true;
    }

    const auto header = rtp::RtpHeader::fromPacket(packet);
    if (!header)
    {
        assert(false);
        return false;
    }
    const uint32_t ssrc = header->ssrc;

    {
        // use remote srtp context to fake a packet with ROC=0
        alignas(sizeof(uint32_t)) uint8_t fakePacketRoc0[40]{0};
        auto fakeHeader = reinterpret_cast<rtp::RtpHeader*>(fakePacketRoc0);
        fakeHeader->ssrc = ssrc;
        fakeHeader->sequenceNumber = 65534;
        fakeHeader->version = 2;
        fakeHeader->timestamp = 12345;
        int fakeLength = 20;

        // remote context is used for decryption but we will use it for encryption once and then remove the stream
        // to be able to decrypt without replay error.
        const auto encryptStatus = srtp_protect(_remoteSrtp, fakePacketRoc0, &fakeLength);
        const auto rmStatus = srtp_remove_stream(_remoteSrtp, htonl(ssrc));
        const auto decryptStatus = srtp_unprotect(_remoteSrtp, fakePacketRoc0, &fakeLength);
        if (encryptStatus || rmStatus || decryptStatus)
        {
            logger::error("Failed to decrypt synthetic ROC0 packet on ssrc %u", _loggableId.c_str(), ssrc);
            return false;
        }
    }

    // try probable starting roc values. Each one should correspond to several minutes of streaming
    for (rolloverCounter = 1u; rolloverCounter < 5; ++rolloverCounter)
    {
        const auto rocResult = srtp_set_stream_roc(_remoteSrtp, ssrc, rolloverCounter);
        if (rocResult)
        {
            logger::error("Failed to set decrypt ROC for ssrc %u, err %u", _loggableId.c_str(), ssrc, rocResult);
            return false;
        }

        if (srtp_err_status_ok == srtp_unprotect(_remoteSrtp, packet.get(), &bufferLength))
        {
            return true;
        }
    }

    logger::warn("srtp unprotect error: %d, ssrc %u, seq %u, ts %u",
        _loggableId.c_str(),
        static_cast<int32_t>(result),
        header->ssrc.get(),
        header->sequenceNumber.get(),
        header->timestamp.get());
    rolloverCounter = 0;
    return false;
}

} // namespace transport
