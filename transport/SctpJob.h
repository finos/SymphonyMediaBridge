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
class SerialJobManager;
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
    SctpTimerJob(jobmanager::SerialJobManager& serialJobManager,
        transport::Transport& transport,
        sctp::SctpAssociation& sctpAssociation);

    void run() override;
    static void start(jobmanager::SerialJobManager& serialJobManager,
        transport::Transport& transport,
        sctp::SctpAssociation& sctpAssociation,
        int64_t nextTimeoutNs);

protected:
    jobmanager::SerialJobManager& _serialJobManager;
    transport::Transport& _transport;
    sctp::SctpAssociation& _sctpAssociation;
};

} // namespace transport