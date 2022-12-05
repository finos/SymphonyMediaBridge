#pragma once
#include "jobmanager/Job.h"
#include <memory>

namespace transport
{
class RtcTransport;
}

namespace bridge
{
class Mixer;
class MixerManager;

class FinalizeEngineMixerRemoval final : public jobmanager::MultiStepWithTimeoutJob
{
public:
    FinalizeEngineMixerRemoval(MixerManager& mixerManager, std::shared_ptr<Mixer> mixer);

    void onTimeout() override;
    bool runTick() override;

private:
    MixerManager& _mixerManager;
    std::shared_ptr<Mixer> _mixer;
};

class FinalizeTransportJob final : public jobmanager::MultiStepWithTimeoutJob
{
public:
    FinalizeTransportJob(const std::shared_ptr<transport::RtcTransport>& transport);

    void onTimeout() override;
    bool runTick() override;

private:
    std::shared_ptr<transport::RtcTransport> _transport;
};

} // namespace bridge
