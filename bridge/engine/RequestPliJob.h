#pragma once

#include "bridge/engine/SsrcInboundContext.h"
#include "jobmanager/Job.h"
#include <memory>

namespace bridge
{

class RequestPliJob : jobmanager::Job
{
public:
    RequestPliJob(SsrcInboundContext& context) : _ssrcContext(context) {}

    void run() override { _ssrcContext._pliScheduler.triggerPli(); }

private:
    SsrcInboundContext& _ssrcContext;
};

} // namespace bridge
