#pragma once

#include "utils/StringBuilder.h"
#include <openssl/ssl.h>
#include <string>

namespace transport
{

enum DTLSContentType : uint8_t
{
    changeCipherSpec = 20,
    alert,
    handshake,
    applicationData
};

enum ProtocolVersion : uint16_t
{
    SSLv30 = 0x300,
    TLSv10 = 0x301,
    TLSv11 = 0x302,
    TTLv12 = 0x0303,
    DTLSv10 = 0xFEFF,
    DTLSv12 = 0xFEFD
};

class SslDtls
{
public:
    SslDtls();
    ~SslDtls();

    bool isInitialized() const { return _evpPkeyRsa && _certificate && _sslContext && _writeBioMethods; }
    const std::string& getLocalFingerprint() const { return _localFingerprint; }
    SSL_CTX* getSslContext() const { return _sslContext; }
    BIO_METHOD* getWriteBioMethods() const { return _writeBioMethods; }

private:
    SSL_CTX* _sslContext;
    EVP_PKEY* _evpPkeyRsa;
    X509* _certificate;
    std::string _localFingerprint;
    BIO_METHOD* _writeBioMethods;
    static uint32_t _instanceCounter;
};

inline bool isDtlsPacket(const void* data)
{
    auto msg = reinterpret_cast<const uint8_t*>(data);
    return msg[0] >= 20 && msg[0] < 63;
}

inline std::string makeFingerprintString(const unsigned char* fingerprint, const uint32_t size)
{
    const auto elementSize = 4;
    utils::StringBuilder<EVP_MAX_MD_SIZE * elementSize> stringBuilder;

    char byteAsString[elementSize];
    memset(byteAsString, 0, elementSize);

    for (uint32_t i = 0; i < size; ++i)
    {
        if (i == size - 1)
        {
            snprintf(byteAsString, elementSize, "%.2X", fingerprint[i]);
        }
        else
        {
            snprintf(byteAsString, elementSize, "%.2X:", fingerprint[i]);
        }

        stringBuilder.append(byteAsString);
    }

    return stringBuilder.build();
}

} // namespace transport
