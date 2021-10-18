#include "SrtpClientFactory.h"

namespace transport
{

SrtpClientFactory::SrtpClientFactory(SslDtls& sslDtls, memory::PacketPoolAllocator& allocator)
    : _sslDtls(sslDtls),
      _allocator(allocator)
{
}

std::unique_ptr<SrtpClient> SrtpClientFactory::create(SrtpClient::IEvents* eventListener)
{
    return std::make_unique<SrtpClient>(_sslDtls, eventListener, _allocator);
}

} // namespace transport
