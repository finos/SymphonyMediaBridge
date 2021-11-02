#pragma once
#include "memory/PacketPoolAllocator.h"
#include "transport/Transport.h"

namespace memory
{
class Packet;
}

namespace transport
{

class SendJob : public jobmanager::CountedJob
{
public:
    SendJob(Transport& transport, memory::Packet* packet, memory::PacketPoolAllocator& sendAllocator)
        : CountedJob(transport.getJobCounter()),
          _transport(transport),
          _packet(packet),
          _sendAllocator(sendAllocator)
    {
    }

    ~SendJob()
    {
        if (_packet)
        {
            _sendAllocator.free(_packet);
        }
    }

    void run() override
    {
        _transport.protectAndSend(_packet, _sendAllocator);
        _packet = nullptr;
    }

private:
    Transport& _transport;
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _sendAllocator;
};

} // namespace transport
