#pragma once
#include <cstddef>
namespace memory
{
class Packet;
}

namespace transport
{

class DtlsMessageListener
{
public:
    virtual ~DtlsMessageListener() = default;

    virtual void onMessageReceived(const char* buffer, const size_t length) = 0;
};

} // namespace transport
