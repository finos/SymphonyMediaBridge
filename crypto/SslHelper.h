#pragma once
#include <cinttypes>
#include <string>

struct hmac_ctx_st;
struct evp_md_ctx_st;
struct evp_cipher_ctx_st;

namespace crypto
{
class HMAC
{
public:
    HMAC(const void* key, int keyLength);
    ~HMAC();

    void add(const void* data, int length);

    void compute(uint8_t* sha) const;
    void reset(const void* key, int keyLength);
    void reset();

    template <typename IntType>
    void add(const IntType& data)
    {
        add(&data, sizeof(IntType));
    }

private:
    hmac_ctx_st* _ctx;
    uint8_t _key[1024];
    int _keyLength;
};

class MD5
{
public:
    MD5();
    ~MD5();

    void add(const void* data, int length);
    void compute(uint8_t md5[16]) const;

    template <typename IntType>
    void add(const IntType& data)
    {
        add(&data, sizeof(IntType));
    }

    void reset();

private:
    struct evp_md_ctx_st* _ctx;
};

class Crc32Polynomial
{
public:
    explicit Crc32Polynomial(uint32_t polynomial);
    inline uint32_t operator[](uint8_t pos) const { return _table[pos]; }

private:
    uint32_t _table[256];
};
class Crc32
{
public:
    explicit Crc32(const Crc32Polynomial& polynomial);

    void add(const void* data, int length);
    uint32_t compute() const;
    void reset();

    template <typename IntType>
    void add(const IntType& data)
    {
        add(&data, sizeof(IntType));
    }

private:
    const Crc32Polynomial& _polynomial;
    uint32_t _crc;
};

class AES
{
public:
    AES(const void* key, uint16_t keyLength);
    ~AES();

    bool encrypt(const unsigned char* plaintext,
        uint16_t plaintextLength,
        unsigned char* ciphertext,
        uint16_t& ciphertextLength,
        const unsigned char* iv,
        uint16_t ivLength);
    bool decrypt(const unsigned char* ciphertext,
        uint16_t ciphertextLength,
        unsigned char* plaintext,
        uint16_t& plaintextLength,
        const unsigned char* iv,
        uint16_t ivLength);

    bool gcmEncrypt(const unsigned char* plaintext,
        uint16_t plaintextLength,
        unsigned char* ciphertext,
        uint16_t& ciphertextLength,
        const unsigned char* iv,
        uint16_t ivLength,
        const unsigned char* aad,
        uint16_t aadLength);
    bool gcmDecrypt(const unsigned char* ciphertext,
        uint16_t ciphertextLength,
        unsigned char* plaintext,
        uint16_t& plaintextLength,
        const unsigned char* iv,
        uint16_t ivLength,
        const unsigned char* aad,
        uint16_t aadLength);

    void reset(const void* key, uint16_t keyLength);
    void reset();

private:
    evp_cipher_ctx_st* _encryptCtx;
    evp_cipher_ctx_st* _decryptCtx;
    uint8_t _key[256];
};

std::string toHexString(const void* src, uint16_t len);
} // namespace crypto
