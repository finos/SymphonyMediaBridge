#pragma once

#include "concurrency/MpmcHashmap.h"
#include "jobmanager/Job.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "memory/PacketPoolAllocator.h"
#include <atomic>
#include <cstdint>
#include <memory>

namespace transport
{
class Transport;
}

namespace memory
{
class Packet;
}

namespace bridge
{

class SsrcOutboundContext;

class EncodeJob : public jobmanager::CountedJob
{
public:
    EncodeJob(memory::AudioPacket* packet,
        memory::AudioPacketPoolAllocator& allocator,
        SsrcOutboundContext& outboundContext,
        transport::Transport& transport,
        uint64_t rtpTimestamp,
        int32_t audioLevelExtensionId,
        int32_t absSendTimeExtensionId);

    virtual ~EncodeJob();

    void run() override;

private:
    memory::AudioPacket* _packet;
    memory::AudioPacketPoolAllocator& _audioPacketPoolAllocator;
    SsrcOutboundContext& _outboundContext;
    transport::Transport& _transport;
    uint64_t _rtpTimestamp;
    int32_t _audioLevelExtensionId;
    int32_t _absSendTimeExtensionId;
};

} // namespace bridge
