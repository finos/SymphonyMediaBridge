#pragma once

#include "jobmanager/Job.h"
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
        memory::Packet* packet,
        transport::RecordingTransport& transport,
        const uint32_t extendedSequenceNumber);

    ~RecordingAudioForwarderSendJob() override;

    void run() override;

private:
    SsrcOutboundContext& _outboundContext;
    memory::Packet* _packet;
    transport::RecordingTransport& _transport;
    uint32_t _extendedSequenceNumber;
};

} // namespace bridge
