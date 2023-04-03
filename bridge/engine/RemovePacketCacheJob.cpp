#include "bridge/engine/RemovePacketCacheJob.h"
#include "bridge/MixerManagerAsync.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "transport/Transport.h"

namespace bridge
{

RemovePacketCacheJob::RemovePacketCacheJob(bridge::EngineMixer& mixer,
    transport::Transport& transport,
    bridge::SsrcOutboundContext& outboundContext,
    bridge::MixerManagerAsync& mixerManager)
    : CountedJob(transport.getJobCounter()),
      _mixer(mixer),
      _outboundContext(outboundContext),
      _mixerManager(mixerManager),
      _endpointIdHash(transport.getEndpointIdHash())
{
}

void RemovePacketCacheJob::run()
{
    if (_outboundContext.packetCache.isSet() && _outboundContext.packetCache.get())
    {
        const auto posted = _mixerManager.asyncFreeVideoPacketCache(_mixer, _outboundContext.ssrc, _endpointIdHash);
        if (posted)
        {
            _outboundContext.packetCache.clear();
        }
    }
}

} // namespace bridge
