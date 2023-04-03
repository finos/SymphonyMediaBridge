#pragma once
#include "jobmanager/Job.h"

namespace transport
{
class Transport;
}

namespace bridge
{
class SsrcOutboundContext;
class PacketCache;

class AddPacketCacheJob : public jobmanager::CountedJob
{
public:
    AddPacketCacheJob(transport::Transport& transport, SsrcOutboundContext& outboundContext, PacketCache* packetCache);

    void run() override;

private:
    SsrcOutboundContext& _outboundContext;
    PacketCache* _packetCache;
};

} // namespace bridge
