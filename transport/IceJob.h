#pragma once
#include "jobmanager/Job.h"
#include "utils/SocketAddress.h"
namespace jobmanager
{
class JobManager;
class SerialJobManager;
} // namespace jobmanager
namespace memory
{
class Packet;
}

namespace ice
{
class IceSession;
}
namespace transport
{
class Transport;

class IceJob : public jobmanager::CountedJob
{
public:
    static const uint32_t TIMER_ID = 0xFFFF7E00;
    IceJob(Transport& transport, jobmanager::SerialJobManager& serialJobManager, ice::IceSession& session);

protected:
    Transport& _transport;
    jobmanager::SerialJobManager& _serialJobManager;
    ice::IceSession& _session;
};

// runs on transport serial jobmanager
class IceTimerJob : public IceJob
{
public:
    IceTimerJob(Transport& transport, jobmanager::SerialJobManager& serialJobManager, ice::IceSession& session)
        : IceJob(transport, serialJobManager, session)
    {
    }
    void run() override;
};

// runs on job manager on timeout to trigger a job on serial jobmanager for transport
class IceTimerTriggerJob : public IceJob
{
public:
    IceTimerTriggerJob(Transport& transport, jobmanager::SerialJobManager& serialJobManager, ice::IceSession& session)
        : IceJob(transport, serialJobManager, session)
    {
    }
    void run() override;
};

class IceStartJob : public IceJob
{
public:
    IceStartJob(Transport& transport, jobmanager::SerialJobManager& serialJobManager, ice::IceSession& session)
        : IceJob(transport, serialJobManager, session)
    {
    }
    void run() override;
};

} // namespace transport