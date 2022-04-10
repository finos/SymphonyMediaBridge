#pragma once
#include "memory/PacketPoolAllocator.h"
#include <cstddef>

namespace transport
{

class DtlsMessageListener
{
public:
    virtual ~DtlsMessageListener() = default;

    virtual void onMessageReceived(memory::UniquePacket packet) = 0;
};

} // namespace transport
