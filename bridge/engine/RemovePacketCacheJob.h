#pragma once
#include "jobmanager/Job.h"

namespace concurrency
{
class SynchronizationContext;
}
namespace transport
{
class Transport;
}
namespace bridge
{
class EngineMixer;
class SsrcOutboundContext;
class MixerManagerAsync;

class RemovePacketCacheJob : public jobmanager::CountedJob
{
public:
    RemovePacketCacheJob(bridge::EngineMixer& mixer,
        transport::Transport& transport,
        bridge::SsrcOutboundContext& outboundContext,
        bridge::MixerManagerAsync& mixerManager);

    void run() override;

private:
    bridge::EngineMixer& _mixer;
    bridge::SsrcOutboundContext& _outboundContext;
    bridge::MixerManagerAsync& _mixerManager;
    size_t _endpointIdHash;
};

} // namespace bridge
