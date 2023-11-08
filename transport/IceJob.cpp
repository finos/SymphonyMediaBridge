#include "IceJob.h"
#include "Transport.h"
#include "ice/IceSession.h"
#include "jobmanager/JobQueue.h"
namespace transport
{
IceJob::IceJob(Transport& transport, ice::IceSession& session)
    : CountedJob(transport.getJobCounter()),
      _transport(transport),
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
        _transport.getJobQueue().getJobManager().replaceTimedJob<IceTimerTriggerJob>(_transport.getId(),
            TIMER_ID,
            timeoutNs / 1000,
            _transport,
            _session);
    }
    else
    {
        logger::info("exit ICE timer", _transport.getLoggableId().c_str());
    }
}

void IceTimerTriggerJob::run()
{
    if (_transport.isRunning())
    {
        _transport.getJobQueue().addJob<IceTimerJob>(_transport, _session);
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
    _transport.getJobQueue().getJobManager().replaceTimedJob<IceTimerTriggerJob>(_transport.getId(),
        TIMER_ID,
        _session.nextTimeout(timestamp) / 1000,
        _transport,
        _session);
}
} // namespace transport
