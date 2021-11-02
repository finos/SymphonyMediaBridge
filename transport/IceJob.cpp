#include "IceJob.h"
#include "Transport.h"
#include "ice/IceSession.h"
#include "jobmanager/JobQueue.h"
namespace transport
{
IceJob::IceJob(Transport& transport, jobmanager::JobQueue& jobQueue, ice::IceSession& session)
    : CountedJob(transport.getJobCounter()),
      _transport(transport),
      _jobQueue(jobQueue),
      _session(session)
{
}

void IceTimerJob::run()
{
    const auto timestamp = utils::Time::getAbsoluteTime();
    const auto timeoutNs = _session.processTimeout(timestamp);
    if (timeoutNs >= 0 && _transport.isRunning() && _session.getState() > ice::IceSession::State::IDLE &&
        _session.getState() < ice::IceSession::State::FAILED)
    {
        _jobQueue.getJobManager().replaceTimedJob<IceTimerTriggerJob>(_transport.getId(),
            TIMER_ID,
            timeoutNs / 1000,
            _transport,
            _jobQueue,
            _session);
    }
    else
    {
        logger::info("exit ICE timer", _transport.getLoggableId().c_str());
    }
}

void IceTimerTriggerJob::run()
{
    if (_transport.isRunning() && _session.getState() > ice::IceSession::State::IDLE &&
        _session.getState() < ice::IceSession::State::FAILED)
    {
        _jobQueue.addJob<IceTimerJob>(_transport, _jobQueue, _session);
    }
}

void IceStartJob::run()
{
    logger::info("starting ICE probes. Role %s on %zu remote candidates",
        _transport.getLoggableId().c_str(),
        _session.getRole() == ice::IceRole::CONTROLLING ? "controlling" : "controlled",
        _session.getRemoteCandidates().size());

    auto timestamp = utils::Time::getAbsoluteTime();
    _session.probeRemoteCandidates(_session.getRole(), timestamp);
    _jobQueue.getJobManager().replaceTimedJob<IceTimerTriggerJob>(_transport.getId(),
        TIMER_ID,
        _session.nextTimeout(timestamp) / 1000,
        _transport,
        _jobQueue,
        _session);
}
} // namespace transport
