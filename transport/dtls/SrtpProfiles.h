#pragma once
#include <cstdint>

namespace srtp
{
enum Profile
{
    // first 5 is aligned with open ssl
    NULL_CIPHER = 0,
    AES128_CM_SHA1_80 = 1,
    AES128_CM_SHA1_32 = 3,
    AEAD_AES_128_GCM = 7,
    AEAD_AES_256_GCM = 8,

    AES_256_CM_SHA1_80 = 32,
    AES_256_CM_SHA1_32,
    AES_192_CM_SHA1_80,
    AES_192_CM_SHA1_32,
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

} // namespace srtp
