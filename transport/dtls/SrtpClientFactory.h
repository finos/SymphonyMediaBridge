#pragma once

#include "SrtpClient.h"

namespace transport
{
class SslDtls;

class SrtpClientFactory
{
public:
    explicit SrtpClientFactory(SslDtls& sslDtls, memory::PacketPoolAllocator& allocator);

    std::unique_ptr<SrtpClient> create(SrtpClient::IEvents* eventListener = nullptr);

private:
    SslDtls& _sslDtls;
    memory::PacketPoolAllocator& _allocator;
};

} // namespace transport
