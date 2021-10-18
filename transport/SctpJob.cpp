#include "SctpJob.h"
#include "Transport.h"
#include "jobmanager/SerialJobManager.h"
#include "memory/PacketPoolAllocator.h"
#include "sctp/SctpAssociation.h"
namespace transport
{

SctpTimerJob::SctpTimerJob(jobmanager::SerialJobManager& serialJobManager,
    transport::Transport& transport,
    sctp::SctpAssociation& sctpAssociation)
    : CountedJob(transport.getJobCounter()),
      _serialJobManager(serialJobManager),
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
            start(_serialJobManager, _transport, _sctpAssociation, nextTimeoutNs);
        }
    }
}
class SctpTimerTriggerJob : public SctpTimerJob
{
public:
    SctpTimerTriggerJob(jobmanager::SerialJobManager& serialJobManager,
        transport::Transport& transport,
        sctp::SctpAssociation& sctpAssociation)
        : SctpTimerJob(serialJobManager, transport, sctpAssociation)
    {
    }

    void run() override
    {
        if (_transport.isRunning())
        {
            _serialJobManager.addJob<SctpTimerJob>(_serialJobManager, _transport, _sctpAssociation);
        }
    }
};

void SctpTimerJob::start(jobmanager::SerialJobManager& serialJobManager,
    transport::Transport& transport,
    sctp::SctpAssociation& sctpAssociation,
    const int64_t nextTimeoutNs)
{
    serialJobManager.getJobManager().replaceTimedJob<SctpTimerTriggerJob>(transport.getId(),
        TIMER_ID,
        nextTimeoutNs / 1000,
        serialJobManager,
        transport,
        sctpAssociation);
}
} // namespace transport
