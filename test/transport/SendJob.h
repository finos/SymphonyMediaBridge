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
    SendJob(Transport& transport, memory::PacketPtr packet)
        : CountedJob(transport.getJobCounter()),
          _transport(transport),
          _packet(std::move(packet))
    {
    }

    void run() override { _transport.protectAndSend(std::move(_packet)); }

private:
    Transport& _transport;
    memory::PacketPtr _packet;
};

} // namespace transport
