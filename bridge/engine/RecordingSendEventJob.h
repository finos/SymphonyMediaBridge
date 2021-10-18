#pragma once

#include "bridge/engine/PacketCache.h"
#include "bridge/engine/RecordingOutboundContext.h"
#include "bridge/engine/UnackedPacketsTracker.h"
#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"
#include <memory>

namespace transport
{
class RecordingTransport;
}

namespace bridge
{

struct RtpMap;
class SsrcInboundContext;

class RecordingSendEventJob : public jobmanager::CountedJob
{
public:
    RecordingSendEventJob(std::atomic_uint32_t& ownerJobsCounter,
        memory::Packet* packet,
        memory::PacketPoolAllocator& allocator,
        transport::RecordingTransport& transport,
        PacketCache& recEventPacketCache,
        UnackedPacketsTracker& unackedPacketsTracker);

    ~RecordingSendEventJob() override;

    void run() override;

private:
    memory::Packet* _packet;
    memory::PacketPoolAllocator& _allocator;
    transport::RecordingTransport& _transport;
    PacketCache& _recEventPacketCache;
    UnackedPacketsTracker& _unackedPacketsTracker;
};

} // namespace bridge
