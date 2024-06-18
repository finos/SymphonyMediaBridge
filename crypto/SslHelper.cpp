#include "crypto/SslHelper.h"
#include <array>
#include <cassert>
#include <cstring>
#if OPENSSL_VERSION_MAJOR >= 3
#include <openssl/core_names.h>
#include <openssl/params.h>
#else
#endif

namespace
{
unsigned char reverse(unsigned char b)
{
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}
} // namespace

namespace crypto
{
std::string toHexString(const void* srcData, uint16_t len)
{
    auto src = reinterpret_cast<const uint8_t*>(srcData);
    const char hexmap[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string s;
    for (int i = 0; i < len; ++i)
    {
        s += hexmap[src[i] >> 4];
        s += hexmap[src[i] & 0x0F];
    }
    return s;
}

HMAC::HMAC() : _ctx(nullptr)
{
#if OPENSSL_VERSION_MAJOR >= 3
    _mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    assert(_mac);
    _ctx = EVP_MAC_CTX_new(_mac);
#else
    _ctx = HMAC_CTX_new();
#endif
    assert(_ctx);
}

HMAC::HMAC(const void* key, int keyLength) : HMAC()
{
    init(key, keyLength);
}

bool HMAC::init(const void* key, int keyLength)
{
    assert(keyLength <= 1024);
    assert(keyLength >= 0);
    if (key != nullptr && keyLength > 0)
    {
#if OPENSSL_VERSION_MAJOR >= 3
        _key.resize(static_cast<size_t>(keyLength));
        memcpy(_key.data(), key, _key.size());

        return macInit();
#else
        const auto success = HMAC_Init_ex(_ctx, key, keyLength, EVP_sha1(), nullptr);
        assert(success);
        assert(HMAC_size(_ctx) <= 20);
        return success;
#endif
    }
    else
    {
        _key.clear();
        assert(false);
        return false;
    }
}

#if OPENSSL_VERSION_MAJOR >= 3
bool HMAC::macInit()
{
    char sha1DigestString[] = "sha1";
    std::array<OSSL_PARAM, 4> params{
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, sha1DigestString, sizeof(sha1DigestString)),
        OSSL_PARAM_construct_end()};
    const auto success = EVP_MAC_init(_ctx, _key.data(), _key.size(), params.data());
    assert(success);
    return success;
}
#endif

/**
 * Resets calculation and prepares for another run off add, add, compute.
 */
bool HMAC::reset()
{
#if OPENSSL_VERSION_MAJOR >= 3
    if (!_key.empty())
    {
        return macInit();
    }
    else
    {
        const auto success = EVP_MAC_init(_ctx, nullptr, 0, nullptr);
        assert(success);
        return success;
    }
#else
    const auto success = HMAC_Init_ex(_ctx, nullptr, 0, nullptr, nullptr);
    assert(success);
    assert(HMAC_size(_ctx) <= 20);
    return success;
#endif
}

HMAC::~HMAC()
{
#if OPENSSL_VERSION_MAJOR >= 3
    EVP_MAC_CTX_free(_ctx);
    EVP_MAC_free(_mac);
#else
    HMAC_CTX_free(_ctx);
#endif
}

void HMAC::add(const void* data, int length)
{
    assert(length > 0);
#if OPENSSL_VERSION_MAJOR >= 3
    const auto success = EVP_MAC_update(_ctx, reinterpret_cast<const uint8_t*>(data), length);
#else
    const auto success = HMAC_Update(_ctx, reinterpret_cast<const uint8_t*>(data), length);
#endif
    assert(success);
}

/**
 * @brief computes 20B output
 */
void HMAC::compute(uint8_t* sha) const
{
#if OPENSSL_VERSION_MAJOR >= 3
    size_t outLen = 20;
    const auto success = EVP_MAC_final(_ctx, sha, &outLen, outLen);
#else
    uint32_t outLen = 0;
    const auto success = HMAC_Final(_ctx, sha, &outLen);
#endif
    assert(success);
    assert(outLen == 20);
}

MD5::MD5() : _ctx(EVP_MD_CTX_new())
{
    reset();
}

MD5::~MD5()
{
    EVP_MD_CTX_free(_ctx);
}

void MD5::add(const void* data, int length)
{
    EVP_DigestUpdate(_ctx, data, length);
}

void MD5::compute(uint8_t md5[16]) const
{
    EVP_DigestFinal_ex(_ctx, md5, nullptr);
}

void MD5::reset()
{
    EVP_DigestInit_ex(_ctx, EVP_md5(), nullptr);
}

Crc32Polynomial::Crc32Polynomial(uint32_t polynomial)
{
    uint32_t revPolynomial = 0;
    for (size_t i = 0; i < sizeof(uint32_t); ++i)
    {
        revPolynomial = (revPolynomial << 8) | reverse(polynomial & 0xFFu);
        polynomial >>= 8;
    }

    unsigned char b = 0;
    do
    {
        uint32_t remainder = b;
        for (uint32_t bit = 8; bit > 0; --bit)
        {
            if (remainder & 1)
            {
                remainder = (remainder >> 1) ^ revPolynomial;
            }
            else
            {
                remainder = (remainder >> 1);
            }
        }
        _table[static_cast<size_t>(b)] = remainder;
    } while (0 != ++b);
}

Crc32::Crc32(const Crc32Polynomial& polynomial) : _polynomial(polynomial), _crc(0xFFFFFFFFul) {}

void Crc32::reset()
{
    _crc = 0xFFFFFFFFul;
}

void Crc32::add(const void* data, int length)
{
    int i;
    auto p = reinterpret_cast<const uint8_t*>(data);
    for (i = 0; i < length; i++)
    {
        _crc = _polynomial[*p++ ^ (_crc & 0xff)] ^ (_crc >> 8);
    }
}

uint32_t Crc32::compute() const
{
    return ~_crc;
}

AES::AES(const void* key, uint16_t keyLength) : _encryptCtx(EVP_CIPHER_CTX_new()), _decryptCtx(EVP_CIPHER_CTX_new())
{
    reset(key, keyLength);
}

void AES::reset(const void* key, uint16_t keyLength)
{
    reset();
    assert(keyLength <= 1024);
    if (key != nullptr)
    {
        std::memcpy(_key, key, keyLength);
        EVP_CIPHER_CTX_init(_encryptCtx);
        EVP_EncryptInit_ex(_encryptCtx, EVP_aes_256_gcm(), nullptr, _key, nullptr);
        EVP_CIPHER_CTX_init(_decryptCtx);
        EVP_DecryptInit_ex(_decryptCtx, EVP_aes_256_gcm(), nullptr, _key, nullptr);
    }
}

void AES::reset()
{
    EVP_CIPHER_CTX_reset(_encryptCtx);
    EVP_CIPHER_CTX_reset(_decryptCtx);
}

AES::~AES()
{
    EVP_CIPHER_CTX_free(_encryptCtx);
    EVP_CIPHER_CTX_free(_decryptCtx);
}

// Note: IV has to be unique for each plaintext encrypted to avoid security breaches
bool AES::encrypt(const unsigned char* plaintext,
    uint16_t plaintextLength,
    unsigned char* ciphertext,
    uint16_t& ciphertextLength,
    const unsigned char* iv,
    uint16_t ivLength)
{
    return gcmEncrypt(plaintext, plaintextLength, ciphertext, ciphertextLength, iv, ivLength, nullptr, 0);
}

// Note: IV MUST be unique for each plaintext encrypted with the same key
bool AES::decrypt(const unsigned char* ciphertext,
    uint16_t ciphertextLength,
    unsigned char* plaintext,
    uint16_t& plaintextLength,
    const unsigned char* iv,
    uint16_t ivLength)
{
    return gcmDecrypt(ciphertext, ciphertextLength, plaintext, plaintextLength, iv, ivLength, nullptr, 0);
}

// Note: IV MUST be unique for each plaintext encrypted with the same key
bool AES::gcmEncrypt(const unsigned char* plaintext,
    uint16_t plaintextLength,
    unsigned char* ciphertext,
    uint16_t& ciphertextLength,
    const unsigned char* iv,
    uint16_t ivLength,
    const unsigned char* aad,
    uint16_t aadLength)
{
    auto tmpLength = 0;

    // allows reusing of context for multiple encryption cycles
    if (!EVP_EncryptInit_ex(_encryptCtx, nullptr, nullptr, nullptr, iv))
    {
        return false;
    }

    if (!EVP_CIPHER_CTX_ctrl(_encryptCtx, EVP_CTRL_GCM_SET_IVLEN, ivLength, nullptr))
    {
        return false;
    }

    if (!!aad && aadLength > 0)
    {
        if (!EVP_EncryptUpdate(_encryptCtx, nullptr, &tmpLength, aad, aadLength))
        {
            return false;
        }
    }

    // update ciphertext, cipherLength is filled with the length of ciphertext generated
    int32_t ciphertextLengthTmp = ciphertextLength;
    if (!EVP_EncryptUpdate(_encryptCtx, ciphertext, &ciphertextLengthTmp, plaintext, plaintextLength))
    {
        return false;
    }

    ciphertextLength = static_cast<uint16_t>(ciphertextLengthTmp);

    // update ciphertext with the final remaining bytes
    if (!EVP_EncryptFinal_ex(_encryptCtx, ciphertext + ciphertextLength, &tmpLength))
    {
        return false;
    }

    ciphertextLength += tmpLength;

    // Get the tag and append to the cipher
    if (!!aad && aadLength > 0)
    {
        unsigned char tag[EVP_GCM_TLS_TAG_LEN];
        if (!EVP_CIPHER_CTX_ctrl(_encryptCtx, EVP_CTRL_GCM_GET_TAG, EVP_GCM_TLS_TAG_LEN, tag))
        {
            return false;
        }

        std::memcpy(ciphertext + ciphertextLength, tag, EVP_GCM_TLS_TAG_LEN);
        ciphertextLength += EVP_GCM_TLS_TAG_LEN;
    }

    return true;
}

// Note: IV has to be the same used in the encryption
bool AES::gcmDecrypt(const unsigned char* ciphertext,
    uint16_t ciphertextLength,
    unsigned char* plaintext,
    uint16_t& plaintextLength,
    const unsigned char* iv,
    uint16_t ivLength,
    const unsigned char* aad,
    uint16_t aadLength)
{
    // plaintext will always be equal to or lesser than length of ciphertext
    const auto bufferSize = plaintextLength;
    int32_t tmpLength = 0;

    if (!EVP_DecryptInit_ex(_decryptCtx, nullptr, nullptr, nullptr, iv))
    {
        return false;
    }

    if (!EVP_CIPHER_CTX_ctrl(_decryptCtx, EVP_CTRL_GCM_SET_IVLEN, ivLength, nullptr))
    {
        return false;
    }

    if (!!aad && aadLength > 0)
    {
        if (!EVP_EncryptUpdate(_decryptCtx, nullptr, &tmpLength, aad, aadLength))
        {
            return false;
        }
    }

    int32_t ciphertextLengthTmp = ciphertextLength;
    if (!EVP_DecryptUpdate(_decryptCtx, plaintext, &ciphertextLengthTmp, ciphertext, ciphertextLength))
    {
        return false;
    }

    plaintextLength = static_cast<uint16_t>(ciphertextLengthTmp);

    if (EVP_DecryptFinal_ex(_decryptCtx, plaintext + plaintextLength, &tmpLength))
    {
        return false;
    }

    plaintextLength += tmpLength;

    // mark the end of the decrypted c-string since it's usually bigger than the final length
    if (plaintextLength < bufferSize)
    {
        plaintext[plaintextLength] = '\0';
    }

    return true;
}
} // namespace crypto
