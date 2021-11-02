#pragma once
#include "jobmanager/Job.h"
#include "memory/PacketPoolAllocator.h"
namespace transport
{
class Transport;
class SrtpClient;
} // namespace transport

namespace jobmanager
{
class JobQueue;
}

namespace sctp
{
class SctpAssociation;
}
namespace transport
{
class SctpTimerJob : public jobmanager::CountedJob
{
public:
    static const uint32_t TIMER_ID = 0xFFFF7E03;
    SctpTimerJob(jobmanager::JobQueue& jobQueue,
        transport::Transport& transport,
        sctp::SctpAssociation& sctpAssociation);

    void run() override;
    static void start(jobmanager::JobQueue& jobQueue,
        transport::Transport& transport,
        sctp::SctpAssociation& sctpAssociation,
        int64_t nextTimeoutNs);

protected:
    jobmanager::JobQueue& _jobQueue;
    transport::Transport& _transport;
    sctp::SctpAssociation& _sctpAssociation;
};

} // namespace transport
