#pragma once
#include "jobmanager/Job.h"

namespace concurrency
{
class SynchronizationContext;
}
namespace transport
{
class RtcTransport;
}
namespace bridge
{
class EngineMixer;
class SsrcOutboundContext;
class MixerManagerAsync;

class FinalizeNonSsrcRewriteOutboundContextJob : public jobmanager::CountedJob
{
public:
    FinalizeNonSsrcRewriteOutboundContextJob(bridge::EngineMixer& mixer,
        transport::RtcTransport& transport,
        bridge::SsrcOutboundContext& outboundContext,
        concurrency::SynchronizationContext& engineSyncContext,
        bridge::MixerManagerAsync& mixerManager,
        uint32_t feedbackSsrc);

    void run() override;

private:
    bridge::EngineMixer& _mixer;
    bridge::SsrcOutboundContext& _outboundContext;
    transport::RtcTransport& _transport;
    concurrency::SynchronizationContext& _engineSyncContext;
    bridge::MixerManagerAsync& _mixerManager;
    uint32_t _feedbackSsrc;
};
} // namespace bridge
