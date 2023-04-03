#include "SrtpProfiles.h"

namespace srtp
{
uint32_t AesKey::getKeyLength() const
{
    switch (profile)
    {
    case srtp::Profile::AES128_CM_SHA1_32:
    case srtp::Profile::AES128_CM_SHA1_80:
    case srtp::Profile::AEAD_AES_128_GCM:
        return 16;
    case srtp::Profile::AES_192_CM_SHA1_32:
    case srtp::Profile::AES_192_CM_SHA1_80:
        return 24;
    case srtp::Profile::AES_256_CM_SHA1_32:
    case srtp::Profile::AES_256_CM_SHA1_80:
    case srtp::Profile::AEAD_AES_256_GCM:
        return 32;
    default:
        return 0;
    }
}

uint32_t AesKey::getSaltLength() const
{
    switch (profile)
    {
    case srtp::Profile::AES128_CM_SHA1_32:
    case srtp::Profile::AES128_CM_SHA1_80:
    case srtp::Profile::AES_192_CM_SHA1_32:
    case srtp::Profile::AES_192_CM_SHA1_80:
    case srtp::Profile::AES_256_CM_SHA1_32:
    case srtp::Profile::AES_256_CM_SHA1_80:
        return 14;
    case srtp::Profile::AEAD_AES_128_GCM:
    case srtp::Profile::AEAD_AES_256_GCM:
        return 12;
    default:
        return 0;
    }
}
} // namespace srtp
