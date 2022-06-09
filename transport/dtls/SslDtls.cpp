#include "SslDtls.h"
#include "SslWriteBioListener.h"
#include "logger/Logger.h"
#include <cassert>
#include <cstdint>
#include <mutex>
#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <random>
#include <srtp2/srtp.h>

namespace
{

const auto keySize = 2048;
const auto mtu = 1500;

int verify(int, X509_STORE_CTX*)
{
    return 1;
}

EVP_PKEY* generateRsaKey()
{
    auto bigNum = BN_new();
    if (!bigNum)
    {
        return nullptr;
    }

    if (BN_set_word(bigNum, RSA_F4) == 0)
    {
        return nullptr;
    }

    auto rsaKey = RSA_new();
    if (!rsaKey)
    {
        return nullptr;
    }

    if (RSA_generate_key_ex(rsaKey, keySize, bigNum, nullptr) == 0)
    {
        return nullptr;
    }

    auto evpPkey = EVP_PKEY_new();
    if (!evpPkey)
    {
        return nullptr;
    }

    if (EVP_PKEY_assign_RSA(evpPkey, rsaKey) == 0)
    {
        return nullptr;
    }

    BN_free(bigNum);
    return evpPkey;
}

X509* generateCertificate(SSL_CTX* sslContext, EVP_PKEY* evpPkey)
{
    auto certificate = X509_new();
    if (!certificate)
    {
        return nullptr;
    }

    if (X509_set_version(certificate, 2) == 0)
    {
        return nullptr;
    }

    std::mt19937_64 generator(reinterpret_cast<std::mt19937_64::result_type>(&sslContext));
    std::uniform_int_distribution<int64_t> distribution;

    if (ASN1_INTEGER_set(X509_get_serialNumber(certificate), distribution(generator)) == 0)
    {
        return nullptr;
    }

    if (X509_gmtime_adj(X509_get_notBefore(certificate), -1 * 31536000) == 0 ||
        X509_gmtime_adj(X509_get_notAfter(certificate), 31536000) == 0)
    {
        return nullptr;
    }

    if (X509_set_pubkey(certificate, evpPkey) == 0)
    {
        return nullptr;
    }

    auto certificateName = X509_get_subject_name(certificate);
    if (!certificateName)
    {
        return nullptr;
    }

    if (X509_NAME_add_entry_by_txt(certificateName, "O", MBSTRING_ASC, (const unsigned char*)"smb", -1, -1, 0) == 0 ||
        X509_NAME_add_entry_by_txt(certificateName, "CN", MBSTRING_ASC, (const unsigned char*)"smb", -1, -1, 0) == 0)
    {
        return nullptr;
    }

    if (X509_set_issuer_name(certificate, certificateName) == 0)
    {
        return nullptr;
    }

    if (X509_sign(certificate, evpPkey, EVP_sha1()) == 0)
    {
        return nullptr;
    }

    return certificate;
}

int32_t writeBioNew(BIO* bio)
{
    BIO_set_init(bio, 1);
    BIO_set_data(bio, nullptr);
    BIO_set_shutdown(bio, 0);
    return 1;
}

int32_t writeBioFree(BIO* bio)
{
    if (!bio)
    {
        return 0;
    }
    BIO_set_data(bio, nullptr);
    return 1;
}

int32_t writeBioWrite(BIO* bio, const char* buffer, int32_t length)
{
    auto writeBioListener = reinterpret_cast<transport::SslWriteBioListener*>(BIO_get_data(bio));
    if (writeBioListener && length > 0)
    {
        return writeBioListener->sendDtls(buffer, static_cast<uint32_t>(length));
    }
    return 0;
}

long writeBioCtrl(BIO* bio, int32_t cmd, long num, void* ptr)
{
    switch (cmd)
    {
    case BIO_CTRL_FLUSH:
        return 1;

    case BIO_CTRL_DGRAM_QUERY_MTU:
        return mtu;

    case BIO_CTRL_WPENDING:
    case BIO_CTRL_PENDING:
        return 0L;

    default:
        return 0L;
    }
}

} // namespace

namespace transport
{

uint32_t SslDtls::_instanceCounter = 0;
std::mutex _sslInitMutex;

SslDtls::SslDtls() : _sslContext(nullptr), _evpPkeyRsa(nullptr), _certificate(nullptr), _writeBioMethods(nullptr)
{
    {
        std::lock_guard<std::mutex> lock(_sslInitMutex);
        if (0 == _instanceCounter++)
        {
            SSL_library_init();
            SSL_load_error_strings();
            OpenSSL_add_all_algorithms();
            const auto srtpInitResult = srtp_init();
            assert(srtpInitResult == srtp_err_status_ok);
        }
    }

    _sslContext = SSL_CTX_new(DTLS_method());

    SSL_CTX_set_verify(_sslContext, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, ::verify);
    SSL_CTX_set_tlsext_use_srtp(_sslContext, "SRTP_AES128_CM_SHA1_80:SRTP_AES128_CM_SHA1_32");

    _evpPkeyRsa = generateRsaKey();
    if (!_evpPkeyRsa)
    {
        logger::error("Failed to create certificate key", "SslDtls");
        return;
    }

    _certificate = generateCertificate(_sslContext, _evpPkeyRsa);
    if (!_certificate)
    {
        logger::error("Failed to create certificate", "SslDtls");
        return;
    }

    auto result = SSL_CTX_use_certificate(_sslContext, _certificate);
    assert(result);

    result = SSL_CTX_use_PrivateKey(_sslContext, _evpPkeyRsa);
    assert(result);

    result = SSL_CTX_check_private_key(_sslContext);
    assert(result);

    uint32_t fingerprintSize;
    unsigned char fingerprint[EVP_MAX_MD_SIZE];
    result = X509_digest(_certificate, EVP_sha256(), fingerprint, &fingerprintSize);
    assert(result);
    _localFingerprint = makeFingerprintString(fingerprint, fingerprintSize);

    result = SSL_CTX_set_cipher_list(_sslContext, "HIGH:!aNULL:!MD5:!RC4");
    assert(result);

    _writeBioMethods = BIO_meth_new(BIO_TYPE_BIO, "SrtpClient write BIO");
    assert(_writeBioMethods);

    BIO_meth_set_write(_writeBioMethods, writeBioWrite);
    BIO_meth_set_ctrl(_writeBioMethods, writeBioCtrl);
    BIO_meth_set_create(_writeBioMethods, writeBioNew);
    BIO_meth_set_destroy(_writeBioMethods, writeBioFree);
}

SslDtls::~SslDtls()
{
    if (_certificate)
    {
        X509_free(_certificate);
    }

    if (_evpPkeyRsa)
    {
        EVP_PKEY_free(_evpPkeyRsa);
    }

    if (_sslContext)
    {
        SSL_CTX_free(_sslContext);
    }

    BIO_meth_free(_writeBioMethods);

    std::lock_guard<std::mutex> lock(_sslInitMutex);
    if (0 == --_instanceCounter)
    {
        srtp_shutdown();
    }
}

} // namespace transport
