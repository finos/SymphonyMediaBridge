#include "bridge/engine/AddPacketCacheJob.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "transport/Transport.h"

namespace bridge
{

AddPacketCacheJob::AddPacketCacheJob(transport::Transport& transport,
    bridge::SsrcOutboundContext& outboundContext,
    bridge::PacketCache* packetCache)
    : CountedJob(transport.getJobCounter()),
      _outboundContext(outboundContext),
      _packetCache(packetCache)
{
}

void AddPacketCacheJob::run()
{
    if (_outboundContext.packetCache.isSet() && _outboundContext.packetCache.get())
    {
        return;
    }
    _outboundContext.packetCache.set(_packetCache);
}

} // namespace bridge
