#include "SctpJob.h"
#include "Transport.h"
#include "jobmanager/JobQueue.h"
#include "memory/PacketPoolAllocator.h"
#include "sctp/SctpAssociation.h"
namespace transport
{

SctpTimerJob::SctpTimerJob(jobmanager::JobQueue& jobQueue,
    transport::Transport& transport,
    sctp::SctpAssociation& sctpAssociation)
    : CountedJob(transport.getJobCounter()),
      _jobQueue(jobQueue),
      _transport(transport),
      _sctpAssociation(sctpAssociation)
{
}

void SctpTimerJob::run()
{
    if (_transport.isRunning())
    {
        const auto nextTimeoutNs = _sctpAssociation.processTimeout(utils::Time::getAbsoluteTime());
        if (nextTimeoutNs >= 0)
        {
            start(_jobQueue, _transport, _sctpAssociation, nextTimeoutNs);
        }
    }
}
class SctpTimerTriggerJob : public SctpTimerJob
{
public:
    SctpTimerTriggerJob(jobmanager::JobQueue& jobQueue,
        transport::Transport& transport,
        sctp::SctpAssociation& sctpAssociation)
        : SctpTimerJob(jobQueue, transport, sctpAssociation)
    {
    }

    void run() override
    {
        if (_transport.isRunning())
        {
            _jobQueue.addJob<SctpTimerJob>(_jobQueue, _transport, _sctpAssociation);
        }
    }
};

void SctpTimerJob::start(jobmanager::JobQueue& jobQueue,
    transport::Transport& transport,
    sctp::SctpAssociation& sctpAssociation,
    const int64_t nextTimeoutNs)
{
    jobQueue.getJobManager().replaceTimedJob<SctpTimerTriggerJob>(transport.getId(),
        TIMER_ID,
        nextTimeoutNs / 1000,
        jobQueue,
        transport,
        sctpAssociation);
}
} // namespace transport
