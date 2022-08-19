#pragma once

#include "bridge/engine/EngineStats.h"
#include "concurrency/MpmcPublish.h"
#include <array>
#include <inttypes.h>

namespace bridge
{

namespace Stats
{

struct ConnectionsStats
{
    struct Protocols
    {
        uint32_t http = 0;
        uint32_t rtp = 0;

        uint32_t total() const { return http + rtp; }
    };

    Protocols tcp4;
    uint32_t udp4 = 0;
    Protocols tcp6;
    uint32_t udp6 = 0;

    ConnectionsStats() {}

    uint32_t tcpTotal() const { return tcp4.total() + tcp6.total(); }
    uint32_t udpTotol() const { return udp4 + udp6; }
};

struct SystemStats
{
    SystemStats();

    uint64_t processMemory = 0;
    double processCPU = 0;
    double systemCpu = 0;
    uint32_t totalNumberOfThreads = 0;
    uint64_t timestamp = 0;

    double workerCpu = 0;
    double rtceCpu = 0;
    double engineCpu = 0;
    double managerCpu = 0;

    struct ConnectionsStats connections;
};

struct MixerManagerStats
{
    SystemStats systemStats;
    uint32_t conferences = 0;
    uint32_t videoStreams = 0;
    uint32_t audioStreams = 0;
    uint32_t dataStreams = 0;
    uint32_t largestConference = 0;
    EngineStats::EngineStats engineStats;
    uint32_t jobQueueLength = 0;

    uint32_t receivePoolSize = 0;
    uint32_t sendPoolSize = 0;
    uint32_t udpSharedEndpointsSendQueue = 0;
    uint32_t udpSharedEndpointsReceiveKbps = 0;
    uint32_t udpSharedEndpointsSendKbps = 0;

    std::string describe();
};

// Maintains state for collecting cpu and network statistics on demand.
// Depending on whether stats are available, the collectProcStat call may block for a couple of seconds.
// SystemStatsCollector is thread safe.
class SystemStatsCollector
{
public:
    SystemStats collect(uint16_t httpPort, uint16_t tcpRtpPort);

private:
    struct ProcStat
    {
        uint32_t pid = 0;
        unsigned long utime = 0;
        unsigned long stime = 0;
        long cutime = 0;
        long cstime = 0;
        unsigned long virtualmem = 0;
        long pagedmem = 0;
        long priority = 0;
        long nice = 0;
        long threads = 0;
        char name[30];

        bool empty() const { return pid == 0 && threads == 0 && utime == 0; }
    };
    friend ProcStat operator-(ProcStat a, const ProcStat& b);

    struct SystemCpu
    {
        unsigned long utime = 0;
        unsigned long nicetime = 0;
        unsigned long stime = 0;
        unsigned long idle = 0;
        unsigned long iowait = 0;
        unsigned long irq = 0;
        unsigned long softirq = 0;

        double idleRatio() const { return static_cast<double>(idle) / (totalJiffies() + 1); }
        uint64_t totalJiffies() const { return (idle + utime + nicetime + stime + iowait + irq + softirq); }
    };
    friend SystemStatsCollector::SystemCpu operator-(SystemStatsCollector::SystemCpu a,
        const SystemStatsCollector::SystemCpu& b);

    struct LinuxCpuSample
    {
        ProcStat procSample;
        SystemCpu systemSample;

        std::array<ProcStat, 64> threadSamples;
    };

    bool readProcStat(FILE* file, ProcStat& stat) const;
    bool readSystemStat(FILE* h, SystemCpu& stat) const;

    std::atomic_flag _collectingStats = ATOMIC_FLAG_INIT;

    concurrency::MpmcPublish<SystemStats, 4> _stats;
#ifdef __APPLE__
    struct MacCpuSample
    {
        struct timeval utime = {0, 0};
        struct timeval stime = {0, 0};
        uint64_t timestamp = 0;
        uint64_t pagedmem = 0;
    };

    MacCpuSample collectMacCpuSample() const;
#endif

    LinuxCpuSample collectLinuxCpuSample(const std::vector<int>& taskIds) const;
    ConnectionsStats collectLinuxNetStat(uint16_t httpPort, uint16_t tcpRtpPort);
    ConnectionsStats collectNetStats(uint16_t httpPort, uint16_t tcpRtpPort);
    std::vector<int> getTaskIds() const;
};

} // namespace Stats

} // namespace bridge
