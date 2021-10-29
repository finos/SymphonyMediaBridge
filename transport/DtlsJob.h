#pragma once
#include "jobmanager/Job.h"

namespace transport
{
class Transport;
class SrtpClient;
} // namespace transport

namespace jobmanager
{
class JobQueue;
}

namespace memory
{
class Packet;
}

namespace transport
{
class DtlsTimerJob : public jobmanager::CountedJob
{
public:
    static const uint32_t TIMER_ID = 0xFFFF7E01;
    DtlsTimerJob(jobmanager::JobQueue& jobQueue, Transport& transport, SrtpClient& srtpClient);

    void static start(jobmanager::JobQueue& jobQueue,
        Transport& transport,
        SrtpClient& srtpClient,
        uint64_t initialDelay);

    void run() override;

protected:
    jobmanager::JobQueue& _jobQueue;
    Transport& _transport;
    SrtpClient& _srtpClient;
};

} // namespace transport
