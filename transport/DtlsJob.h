#pragma once
#include "jobmanager/Job.h"

namespace transport
{
class Transport;
class SrtpClient;
} // namespace transport

namespace jobmanager
{
class SerialJobManager;
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
    DtlsTimerJob(jobmanager::SerialJobManager& serialJobManager, Transport& transport, SrtpClient& srtpClient);

    void static start(jobmanager::SerialJobManager& serialJobManager,
        Transport& transport,
        SrtpClient& srtpClient,
        uint64_t initialDelay);

    void run() override;

protected:
    jobmanager::SerialJobManager& _serialJobManager;
    Transport& _transport;
    SrtpClient& _srtpClient;
};

} // namespace transport