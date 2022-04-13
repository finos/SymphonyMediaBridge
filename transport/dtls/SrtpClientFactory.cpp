#include "SrtpClientFactory.h"

namespace transport
{

SrtpClientFactory::SrtpClientFactory(SslDtls& sslDtls) : _sslDtls(sslDtls) {}

std::unique_ptr<SrtpClient> SrtpClientFactory::create(SrtpClient::IEvents* eventListener)
{
    return std::make_unique<SrtpClient>(_sslDtls, eventListener);
}

} // namespace transport
