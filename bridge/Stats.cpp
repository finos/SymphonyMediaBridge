#include "bridge/Stats.h"
#include "concurrency/ScopedSpinLocker.h"
#include "utils/ScopedFileHandle.h"
#include "utils/Time.h"
#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/resource.h>
#endif

namespace nlohmann
{
template <class T, std::size_t N>
json to_json(const T (&data)[N])
{
    auto v = json::array();
    for (size_t i = 0; i < N; ++i)
    {
        v.push_back(data[i]);
    }
    return v;
}
} // namespace nlohmann
namespace
{

// runs a system command and returns its output line-by-line
std::vector<std::string> exec(const std::string& command)
{
    std::vector<std::string> result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    std::array<char, 1024> buffer;
    while (pipe && fgets(buffer.data(), buffer.size(), pipe.get()))
    {
        result.push_back(buffer.data());
    }
    return result;
}

} // namespace

namespace bridge
{

namespace Stats
{

#ifdef __APPLE__
struct timeval operator-(struct timeval a, const struct timeval& b)
{
    a.tv_sec -= b.tv_sec;
    if (a.tv_usec < b.tv_usec)
    {
        a.tv_usec = a.tv_usec + 1000000 - b.tv_usec;
        --a.tv_sec;
    }
    else
    {
        a.tv_usec -= b.tv_usec;
    }

    return a;
}

struct timeval operator+(struct timeval a, const struct timeval& b)
{
    a.tv_sec += b.tv_sec;
    if (a.tv_usec + b.tv_usec >= 1000000)
    {
        a.tv_sec++;
        a.tv_usec = a.tv_usec + b.tv_usec - 1000000;
    }
    else
    {
        a.tv_usec += b.tv_usec;
    }

    return a;
}

uint64_t toMicroSeconds(const struct timeval& a)
{
    return static_cast<uint64_t>(a.tv_sec) * 1000000 + a.tv_usec;
}
#endif

SystemStats::SystemStats() {}

std::string MixerManagerStats::describe()
{
    nlohmann::json result;
    result["current_timestamp"] = utils::Time::getAbsoluteTime() / 1000000ULL;
    result["conferences"] = _conferences;
    result["participants"] = std::max({_videoStreams, _audioStreams, _dataStreams});
    result["audiochannels"] = _audioStreams;
    result["videochannels"] = _videoStreams;
    result["threads"] = _systemStats.totalNumberOfThreads;
    result["cpu_usage"] = _systemStats.processCPU;
    result["cpu_engine"] = _systemStats.engineCpu;
    result["cpu_rtce"] = _systemStats.rtceCpu;
    result["cpu_workers"] = _systemStats.workerCpu;
    result["cpu_manager"] = _systemStats.managerCpu;

    result["total_memory"] = _systemStats.processMemory;
    result["used_memory"] = _systemStats.processMemory;
    result["packet_rate_download"] = _engineStats.activeMixers.inbound.total().packetsPerSecond;
    result["bit_rate_download"] = _engineStats.activeMixers.inbound.total().bitrateKbps;
    result["packet_rate_upload"] = _engineStats.activeMixers.outbound.total().packetsPerSecond;
    result["bit_rate_upload"] = _engineStats.activeMixers.outbound.total().bitrateKbps;
    result["total_udp_connections"] = _systemStats.connections.udpTotol();
    result["total_tcp_connections"] = _systemStats.connections.tcpTotal();

    result["inbound_audio_streams"] = _engineStats.activeMixers.inbound.audio.activeStreamCount;
    result["outbound_audio_streams"] = _engineStats.activeMixers.outbound.audio.activeStreamCount;
    result["inbound_video_streams"] = _engineStats.activeMixers.inbound.video.activeStreamCount;
    result["outbound_video_streams"] = _engineStats.activeMixers.outbound.video.activeStreamCount;

    result["job_queue"] = _jobQueueLength;
    result["loss_upload"] = _engineStats.activeMixers.outbound.total().getSendLossRatio();
    result["loss_download"] = _engineStats.activeMixers.inbound.total().getReceiveLossRatio();

    result["pacing_queue"] = _engineStats.activeMixers.pacingQueue;
    result["rtx_pacing_queue"] = _engineStats.activeMixers.rtxPacingQueue;

    result["shared_udp_send_queue"] = _udpSharedEndpointsSendQueue;
    result["shared_udp_receive_rate"] = _udpSharedEndpointsReceiveKbps;
    result["shared_udp_send_rate"] = _udpSharedEndpointsSendKbps;

    result["send_pool"] = _sendPoolSize;
    result["receive_pool"] = _receivePoolSize;

    result["loss_upload_hist"] = nlohmann::to_json(_engineStats.activeMixers.outbound.transport.lossGroup);
    result["loss_download_hist"] = nlohmann::to_json(_engineStats.activeMixers.inbound.transport.lossGroup);
    result["bwe_download_hist"] = nlohmann::to_json(_engineStats.activeMixers.inbound.transport.bandwidthEstimateGroup);
    result["rtt_download_hist"] = nlohmann::to_json(_engineStats.activeMixers.inbound.transport.rttGroup);

    result["engine_slips"] = _engineStats.timeSlipCount;

    return result.dump(4);
}

SystemStatsCollector::ProcStat operator-(SystemStatsCollector::ProcStat a, const SystemStatsCollector::ProcStat& b)
{
    a.cstime -= b.cstime;
    a.cutime -= b.cutime;
    a.stime -= b.stime;
    a.utime -= b.utime;

    return a;
}

SystemStatsCollector::SystemCpu operator-(SystemStatsCollector::SystemCpu a, const SystemStatsCollector::SystemCpu& b)
{
    a.idle -= b.idle;
    a.iowait -= b.iowait;
    a.irq -= b.irq;
    a.nicetime -= b.nicetime;
    a.softirq -= b.softirq;
    a.stime -= b.stime;
    a.utime -= b.utime;

    return a;
}

bool SystemStatsCollector::readProcStat(FILE* file, ProcStat& stat) const
{
    if (!file)
    {
        return false;
    }

    ProcStat sample;

    auto procInfoRead = (11 ==
        fscanf(file,
            "%d %28s %*c %*lu %*lu %*lu %*lu %*lu"
            " %*lu %*lu %*lu %*lu %*lu %lu %lu %ld"
            " %ld %ld %ld %ld %*lu %*llu %lu %ld",
            &sample.pid,
            static_cast<char*>(sample.name),
            &sample.utime,
            &sample.stime,
            &sample.cutime,
            &sample.cstime,
            &sample.priority,
            &sample.nice,
            &sample.threads,
            &sample.virtualmem,
            &sample.pagedmem));

    if (procInfoRead)
    {
        stat = sample;
    }
    return procInfoRead;
}

bool SystemStatsCollector::readSystemStat(FILE* h, SystemCpu& stat) const
{
    char cpuName[50];
    SystemCpu sample;
    auto infoRead = (8 ==
        fscanf(h,
            "%48s %lu %lu %lu %lu %lu %lu %lu",
            static_cast<char*>(cpuName),
            &sample.utime,
            &sample.stime,
            &sample.nicetime,
            &sample.idle,
            &sample.iowait,
            &sample.irq,
            &sample.softirq));

    if (infoRead)
    {
        stat = sample;
    }
    return infoRead;
}

SystemStats SystemStatsCollector::collect()
{
    concurrency::ScopedSpinLocker lock(_collectingStats, std::chrono::nanoseconds(0));
    SystemStats result;
    if (!lock.hasLock())
    {
        _stats.read(result);
        return result;
    }

    SystemStats prevStats;
    _stats.read(prevStats);
    int64_t statsAgeNs = utils::Time::getAbsoluteTime() - prevStats.timestamp;
    if (statsAgeNs < static_cast<int64_t>(utils::Time::sec) && statsAgeNs >= 0)
    {
        return prevStats;
    }

    SystemStats stats;

#ifdef __APPLE__
    auto sample0 = collectMacCpuSample();
    auto netStat = collectNetStats();

    auto toSleep = utils::Time::sec - (utils::Time::getAbsoluteTime() - sample0.timestamp);
    utils::Time::nanoSleep(toSleep);

    auto sample1 = collectMacCpuSample();

    stats.processCPU =
        static_cast<double>(toMicroSeconds((sample1.utime - sample0.utime) + (sample1.stime - sample0.stime)) * 1000) /
        (1 + sample1.timestamp - sample0.timestamp);
    stats.processCPU /= std::thread::hardware_concurrency();
    stats.systemCpu = 0;
    stats.processMemory = sample1.pagedmem;
#else
    const auto cpuCount = std::thread::hardware_concurrency();
    const auto taskIds = getTaskIds();
    auto start = utils::Time::getAbsoluteTime();
    auto sample0 = collectLinuxCpuSample(taskIds);
    auto netStat = collectNetStats();

    auto toSleep = 1000000000UL - (utils::Time::getAbsoluteTime() - start);
    utils::Time::nanoSleep(toSleep);

    auto sample1 = collectLinuxCpuSample(taskIds);

    auto diffProc = sample1.procSample - sample0.procSample;
    auto systemDiff = sample1.systemSample - sample0.systemSample;

    size_t workerCount = 0;
    double workerCpu = 0;
    for (size_t i = 0; i < sample1.threadSamples.size(); ++i)
    {
        const auto taskSample = sample1.threadSamples[i] - sample0.threadSamples[i];
        if (taskSample.empty())
        {
            break;
        }

        if (!std::strcmp(taskSample.name, "(Worker)"))
        {
            workerCpu += static_cast<double>(taskSample.utime + taskSample.stime);
            ++workerCount;
        }
        else if (!std::strcmp(taskSample.name, "(Rtce)"))
        {
            stats.rtceCpu =
                cpuCount * static_cast<double>(taskSample.utime + taskSample.stime) / (1 + systemDiff.totalJiffies());
        }
        else if (!std::strcmp(taskSample.name, "(Engine)"))
        {
            stats.engineCpu =
                cpuCount * static_cast<double>(taskSample.utime + taskSample.stime) / (1 + systemDiff.totalJiffies());
        }
        else if (!std::strcmp(taskSample.name, "(MixerManager)"))
        {
            stats.managerCpu =
                cpuCount * static_cast<double>(taskSample.utime + taskSample.stime) / (1 + systemDiff.totalJiffies());
        }
    }

    if (workerCount > 0)
    {
        stats.workerCpu = workerCpu * cpuCount / (workerCount * (1 + systemDiff.totalJiffies()));
    }

    stats.processCPU = static_cast<double>(diffProc.utime + diffProc.stime) / (1 + systemDiff.totalJiffies());
    stats.systemCpu = 1.0 - systemDiff.idleRatio();
    stats.totalNumberOfThreads = sample1.procSample.threads;
    stats.processMemory = sample1.procSample.pagedmem * getpagesize() / 1024;
#endif
    stats.timestamp = utils::Time::getAbsoluteTime();
    stats.connections = netStat;
    _stats.write(stats);
    return stats;
}

#ifdef __APPLE__
SystemStatsCollector::MacCpuSample SystemStatsCollector::collectMacCpuSample() const
{
    MacCpuSample sample;
    sample.timestamp = utils::Time::getAbsoluteTime();
    struct rusage procInfo;
    if (!getrusage(RUSAGE_SELF, &procInfo))
    {
        sample.utime = procInfo.ru_utime;
        sample.stime = procInfo.ru_stime;
        sample.pagedmem = procInfo.ru_maxrss / 1024;
    }
    return sample;
}

#else
SystemStatsCollector::LinuxCpuSample SystemStatsCollector::collectLinuxCpuSample(const std::vector<int>& taskIds) const
{
    LinuxCpuSample sample;

    utils::ScopedFileHandle hProcStat(fopen("/proc/self/stat", "r"));
    utils::ScopedFileHandle hCpuStat(fopen("/proc/stat", "r"));

    if (hProcStat.get() && hCpuStat.get() && readProcStat(hProcStat.get(), sample.procSample) &&
        readSystemStat(hCpuStat.get(), sample.systemSample))
    {
    }

    size_t threadCount = 0;
    for (int taskId : taskIds)
    {
        char fileName[250];
        std::sprintf(fileName, "/proc/self/task/%d/stat", taskId);
        utils::ScopedFileHandle hTaskStat(fopen(fileName, "r"));
        if (threadCount == sample.threadSamples.size() ||
            !(hTaskStat.get() && readProcStat(hTaskStat.get(), sample.threadSamples[threadCount++])))
        {
            break;
        }
    }
    return sample;
}

std::vector<int> SystemStatsCollector::getTaskIds() const
{
    std::vector<int> result;
    auto folderHandle = opendir("/proc/self/task");
    for (auto entry = readdir(folderHandle); entry != nullptr; entry = readdir(folderHandle))
    {
        if (std::isdigit(entry->d_name[0]))
        {
            result.push_back(std::stoi(entry->d_name));
        }
    }
    closedir(folderHandle);
    return result;
}
#endif
ConnectionsStats SystemStatsCollector::collectNetStats()
{

#ifdef __APPLE__
    ConnectionsStats result;
    const std::string processId = std::to_string(getpid());
    auto netStatOutput = exec("netstat -nav | grep " + processId);

    for (const auto& netStatLine : netStatOutput)
    {
        if (netStatLine.rfind("tcp", 0) == 0)
        {
            result.tcp4++;
        }
        else if (netStatLine.rfind("udp", 0) == 0)
        {
            result.udp4++;
        }
    }
    return result;
#else
    return collectLinuxNetStat();
#endif
}

namespace
{

void readSocketInfo(FILE* file, uid_t myUid, uint32_t& count)
{
    if (!file)
    {
        count = 0;
        return;
    }

    uint32_t port = 0;
    char ignore[513];
    uid_t uid;
    const char* formatString = "%*d: %*32[^:]:%x %*32[^:]:%*x %*x %*8[^:]:%*8s %*x:%*x %*x %u";
    fgets(ignore, sizeof(ignore), file);
    for (int i = 0; i < 500; ++i)
    {
        int items = fscanf(file, formatString, &port, &uid);
        fgets(ignore, sizeof(ignore), file);
        if (items >= 2 && uid == myUid)
        {
            ++count;
        }
        else if (items < 2)
        {
            break;
        }
    }
}

} // namespace

ConnectionsStats SystemStatsCollector::collectLinuxNetStat()
{
    utils::ScopedFileHandle hTcp4Stat(fopen("/proc/self/net/tcp", "r"));
    utils::ScopedFileHandle hTcp6Stat(fopen("/proc/self/net/tcp6", "r"));
    utils::ScopedFileHandle hUdp4Stat(fopen("/proc/self/net/udp", "r"));
    utils::ScopedFileHandle hUdp6Stat(fopen("/proc/self/net/udp6", "r"));

    const auto myUid = getuid();

    ConnectionsStats result;
    readSocketInfo(hTcp4Stat.get(), myUid, result.tcp4);
    readSocketInfo(hUdp4Stat.get(), myUid, result.udp4);
    readSocketInfo(hTcp6Stat.get(), myUid, result.tcp6);
    readSocketInfo(hUdp6Stat.get(), myUid, result.udp6);

    return result;
}

} // namespace Stats

} // namespace bridge
