#include "DtlsJob.h"
#include "Transport.h"
#include "dtls/SrtpClient.h"
#include "jobmanager/JobQueue.h"

namespace transport
{

class DtlsTimerTriggerJob : public DtlsTimerJob
{
public:
    DtlsTimerTriggerJob(jobmanager::JobQueue& jobQueue, Transport& transport, SrtpClient& srtpClient)
        : DtlsTimerJob(jobQueue, transport, srtpClient)
    {
    }

    void run() override
    {
        if (_transport.isRunning())
        {
            _jobQueue.addJob<DtlsTimerJob>(_jobQueue, _transport, _srtpClient);
        }
    }
};

DtlsTimerJob::DtlsTimerJob(jobmanager::JobQueue& jobQueue,
    transport::Transport& transport,
    transport::SrtpClient& srtpClient)
    : CountedJob(transport.getJobCounter()),
      _jobQueue(jobQueue),
      _transport(transport),
      _srtpClient(srtpClient)
{
}

void DtlsTimerJob::run()
{
    const auto timeoutNs = _srtpClient.processTimeout();
    if (timeoutNs >= 0 && _transport.isRunning())
    {
        _jobQueue.getJobManager().replaceTimedJob<DtlsTimerTriggerJob>(_transport.getId(),
            TIMER_ID,
            timeoutNs / 1000,
            _jobQueue,
            _transport,
            _srtpClient);
    }
}

void DtlsTimerJob::start(jobmanager::JobQueue& jobQueue,
    Transport& transport,
    SrtpClient& srtpClient,
    uint64_t initialDelay)
{
    jobQueue.getJobManager().replaceTimedJob<DtlsTimerTriggerJob>(transport.getId(),
        TIMER_ID,
        initialDelay / 1000,
        jobQueue,
        transport,
        srtpClient);
}

} // namespace transport
