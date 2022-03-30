#pragma once

#include "bridge/engine/EngineStats.h"
#include "concurrency/MpmcPublish.h"
#include <inttypes.h>

namespace bridge
{

namespace Stats
{

struct ConnectionsStats
{
    uint32_t tcp4;
    uint32_t udp4;
    uint32_t tcp6;
    uint32_t udp6;

    ConnectionsStats() : tcp4(0), udp4(0), tcp6(0), udp6(0) {}

    uint32_t tcpTotal() const { return tcp4 + tcp6; }
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

    struct ConnectionsStats connections;
};

struct MixerManagerStats
{
    SystemStats _systemStats;
    uint32_t _conferences = 0;
    uint32_t _videoStreams = 0;
    uint32_t _audioStreams = 0;
    uint32_t _dataStreams = 0;
    EngineStats::EngineStats _engineStats;
    uint32_t _jobQueueLength = 0;

    uint32_t _receivePoolSize = 0;
    uint32_t _sendPoolSize = 0;
    uint32_t _udpSharedEndpointsSendQueue = 0;
    uint32_t _udpSharedEndpointsReceiveKbps = 0;
    uint32_t _udpSharedEndpointsSendKbps = 0;

    MixerManagerStats() {}

    std::string describe();
};

// Maintains state for collecting cpu and network statistics on demand.
// Depending on whether stats are available, the collectProcStat call may block for a couple of seconds.
// SystemStatsCollector is thread safe.
class SystemStatsCollector
{
public:
    SystemStats collect();

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

    LinuxCpuSample collectLinuxCpuSample() const;
    ConnectionsStats collectLinuxNetStat();
    ConnectionsStats collectNetStats();
};

} // namespace Stats

} // namespace bridge
