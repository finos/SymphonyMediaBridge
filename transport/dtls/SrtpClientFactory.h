#pragma once

#include "SrtpClient.h"

namespace transport
{
class SslDtls;

class SrtpClientFactory
{
public:
    explicit SrtpClientFactory(SslDtls& sslDtls);

    std::unique_ptr<SrtpClient> create(SrtpClient::IEvents* eventListener = nullptr);

private:
    SslDtls& _sslDtls;
};

} // namespace transport
