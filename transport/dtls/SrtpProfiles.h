#pragma once
#include <cstdint>

namespace srtp
{
enum Profile // ordered after preference
{
    NULL_CIPHER = 0,
    AES128_CM_SHA1_80,
    AES128_CM_SHA1_32,
    AES_256_CM_SHA1_80,
    AES_256_CM_SHA1_32,
    AES_192_CM_SHA1_80,
    AES_192_CM_SHA1_32,
    AEAD_AES_128_GCM,
    AEAD_AES_256_GCM,
    PROFILE_LAST
};

struct AesKey
{
    srtp::Profile profile = srtp::Profile::NULL_CIPHER;
    unsigned char keySalt[46];

    uint32_t getLength() const { return getKeyLength() + getSaltLength(); };
    uint32_t getKeyLength() const;
    uint32_t getSaltLength() const;
};

enum class Mode
{
    UNDEFINED = 0,
    NULL_CIPHER,
    DTLS,
    SDES
};

} // namespace srtp
