#include "DtlsJob.h"
#include "Transport.h"
#include "dtls/SrtpClient.h"
#include "jobmanager/SerialJobManager.h"

namespace transport
{

class DtlsTimerTriggerJob : public DtlsTimerJob
{
public:
    DtlsTimerTriggerJob(jobmanager::SerialJobManager& serialJobManager, Transport& transport, SrtpClient& srtpClient)
        : DtlsTimerJob(serialJobManager, transport, srtpClient)
    {
    }

    void run() override
    {
        if (_transport.isRunning())
        {
            _serialJobManager.addJob<DtlsTimerJob>(_serialJobManager, _transport, _srtpClient);
        }
    }
};

DtlsTimerJob::DtlsTimerJob(jobmanager::SerialJobManager& serialJobManager,
    transport::Transport& transport,
    transport::SrtpClient& srtpClient)
    : CountedJob(transport.getJobCounter()),
      _serialJobManager(serialJobManager),
      _transport(transport),
      _srtpClient(srtpClient)
{
}

void DtlsTimerJob::run()
{
    const auto timeoutNs = _srtpClient.processTimeout();
    if (timeoutNs >= 0 && _transport.isRunning())
    {
        _serialJobManager.getJobManager().replaceTimedJob<DtlsTimerTriggerJob>(_transport.getId(),
            TIMER_ID,
            timeoutNs / 1000,
            _serialJobManager,
            _transport,
            _srtpClient);
    }
}

void DtlsTimerJob::start(jobmanager::SerialJobManager& serialJobManager,
    Transport& transport,
    SrtpClient& srtpClient,
    uint64_t initialDelay)
{
    serialJobManager.getJobManager().replaceTimedJob<DtlsTimerTriggerJob>(transport.getId(),
        TIMER_ID,
        initialDelay / 1000,
        serialJobManager,
        transport,
        srtpClient);
}

} // namespace transport