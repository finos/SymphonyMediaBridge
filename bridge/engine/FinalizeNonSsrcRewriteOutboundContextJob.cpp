#include "bridge/engine/FinalizeNonSsrcRewriteOutboundContextJob.h"
#include "bridge/engine/EngineMixer.h"
#include "bridge/engine/RemovePacketCacheJob.h"
#include "bridge/engine/SsrcOutboundContext.h"
#include "transport/RtcTransport.h"

namespace bridge
{
FinalizeNonSsrcRewriteOutboundContextJob::FinalizeNonSsrcRewriteOutboundContextJob(bridge::EngineMixer& mixer,
    transport::RtcTransport& transport,
    bridge::SsrcOutboundContext& outboundContext,
    concurrency::SynchronizationContext& engineSyncContext,
    bridge::MixerManagerAsync& mixerManager,
    uint32_t feedbackSsrc)
    : CountedJob(transport.getJobCounter()),
      _mixer(mixer),
      _outboundContext(outboundContext),
      _transport(transport),
      _engineSyncContext(engineSyncContext),
      _mixerManager(mixerManager),
      _feedbackSsrc(feedbackSsrc)
{
}

void FinalizeNonSsrcRewriteOutboundContextJob::run()
{
    const size_t endpointIdHash = _transport.getEndpointIdHash();
    const uint32_t ssrc = _outboundContext.ssrc;

    auto packet = EngineMixer::createGoodBye(ssrc, _outboundContext.allocator);
    if (packet)
    {
        _transport.protectAndSend(std::move(packet));
    }

    if (_outboundContext.packetCache.isSet() && _outboundContext.packetCache.get())
    {
        // Run RemovePacketCacheJob inside of this job
        RemovePacketCacheJob(_mixer, _transport, _outboundContext, _mixerManager).run();
    }

    _transport.removeSrtpLocalSsrc(ssrc);

    _engineSyncContext.post(utils::bind(&EngineMixer::onOutboundContextFinalized,
        &_mixer,
        endpointIdHash,
        ssrc,
        _feedbackSsrc,
        _outboundContext.rtpMap.isVideo()));
}

} // namespace bridge
