#pragma once

#include <cstdint>
#include <openssl/ssl.h>

namespace transport
{

class SslWriteBioListener
{
public:
    virtual ~SslWriteBioListener() = default;

    virtual int32_t sendDtls(const char* buffer, int32_t length) = 0;
};

} // namespace transport
