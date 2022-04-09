#pragma once

#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"
#include <cstdint>


namespace memory
{
class Packet;
} // namespace memory

namespace transport
{
class RecordingTransport;
}

namespace bridge
{

class SsrcOutboundContext;

class RecordingAudioForwarderSendJob : public jobmanager::CountedJob
{
public:
    RecordingAudioForwarderSendJob(SsrcOutboundContext& outboundContext,
        memory::PacketPtr packet,
        transport::RecordingTransport& transport,
        const uint32_t extendedSequenceNumber);

    void run() override;

private:
    SsrcOutboundContext& _outboundContext;
    memory::PacketPtr _packet;
    transport::RecordingTransport& _transport;
    uint32_t _extendedSequenceNumber;
};

} // namespace bridge
