#include "bwe/BandwidthEstimator.h"
#include "logger/Logger.h"
#include "logger/PacketLogger.h"
#include "math/Matrix.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/SendTimeDial.h"
#include "test/CsvWriter.h"
#include "test/bwe/BwBurstTracker.h"
#include "test/transport/NetworkLink.h"
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>

using namespace math;

namespace rtp
{
uint32_t nsToSecondsFp6_18(uint64_t timestampNs);
}

class CsvLogger
{
public:
    CsvLogger(const std::string& path, const std::string& name)
        : _prevBw(0),
          _prevDelay(0),
          _logStart(0),
          _sendTimeStart(0),
          _summary((path + name + ".csv").c_str()),
          _full((path + name + "All.csv").c_str()),
          _count(0)
    {
        _summary.writeLine("time, bwo, q, bw, co, delay, size, seqno, stime");
        _full.writeLine("time, bwo, q, bw, co, delay, size, seqno, stime");
    }

    void logToCsv(bwe::BandwidthEstimator& estimator,
        uint32_t size,
        uint64_t sendTime,
        uint64_t receiveTime,
        uint16_t sequenceNumber)
    {
        if (_logStart == 0)
        {
            _logStart = receiveTime;
            _sendTimeStart = sendTime;
        }

        auto bw = estimator.getEstimate(receiveTime);
        auto state = estimator.getState();
        auto delay = estimator.getDelay();
        auto localTimestamp = uint32_t((receiveTime - _logStart) / 100000);
        auto localSendTime = uint32_t((sendTime - _sendTimeStart) / 100000);
        _full.writeLine("%u,%.3f,%.1f,%.3f,%.3f,%.3f,%u,%u,%u",
            localTimestamp,
            bw,
            state(0) / 8.0,
            state(1),
            state(2),
            delay,
            size,
            sequenceNumber,
            localSendTime);

        if ((_count % 1000 == 0) || std::fabs(delay - _prevDelay) > 10 || delay > 70 ||
            std::fabs((bw - _prevBw) / _prevBw) > 0.15)
        {
            _summary.writeLine("%u,%.3f,%.1f,%.3f,%.3f,%.3f,%u,%u,%u",
                localTimestamp,
                bw,
                state(0) / 8.0,
                state(1),
                state(2),
                delay,
                size,
                sequenceNumber,
                localSendTime);

            _prevBw = bw;
            _prevDelay = delay;
        }

        ++_count;
    }

private:
    double _prevBw;
    double _prevDelay;
    uint64_t _logStart;
    uint64_t _sendTimeStart;
    CsvWriter _summary;
    CsvWriter _full;
    uint32_t _count;
};

namespace
{
struct SsrcTrack
{
    uint64_t prevReceiveTime;
    double avgReceiveTime;
    uint32_t count;
};

uint32_t identifyAudioSsrc(logger::PacketLogReader& reader)
{
    logger::PacketLogItem item;
    std::map<uint32_t, SsrcTrack> ssrcs;
    for (int i = 0; reader.getNext(item); ++i)
    {
        if (item.size >= 300)
        {
            if (ssrcs.end() != ssrcs.find(item.ssrc))
            {
                ssrcs.erase(item.ssrc);
            }
            continue;
        }

        auto it = ssrcs.find(item.ssrc);
        if (ssrcs.end() == it)
        {
            ssrcs[item.ssrc] = SsrcTrack{item.receiveTimestamp, 0.02, 1};
            continue;
        }

        if (item.receiveTimestamp - it->second.prevReceiveTime > utils::Time::ms * 15)
        {
            it->second.prevReceiveTime = item.receiveTimestamp;
            ++it->second.count;

            if (it->second.count > 300)
            {
                return item.ssrc;
            }
        }
    }

    if (ssrcs.size() > 0)
    {
        return ssrcs.begin()->first;
    }
    return 0;
}
} // namespace

TEST(BweReRun, DISABLED_fromTrace)
{
    bwe::Config config;
    config.congestion.cap.ratio = 0.5;

    /* std::array<std::string, 58> traces = {"Transport-39_tcp",
         "Transport-58_tcp",
         "Transport-86_tcp_1ploss",
         "Transport-105_tcp_1ploss",
         "Transport-22-4G-2.3Mbps",
         "Transport-1094-4G",
         "Transport-3644-wifi",
         "Transport-62-4G",
         "Transport-3887-wifi",
         "Transport-3629",
         "Transport-14-wifi",
         "Transport-4-wifi",
         "Transport-6-4G-1-5Mbps",
         "Transport-30-3G-1Mbps",
         "Transport-32_Idre",
         "Transport-30_oka",
         "Transport-48_50_3G",
         "Transport-48_60_3G",
         "Transport-48_80_3G",
         "Transport-22-3G",
         "Transport-62-4G",
         "Transport-4735-4G",
         "Transport-42-clkdrift",
         "Transport-44-clkdrift"};*/
    std::array<std::string, 2> traces = {"Transport-5", "Transport-7"};
    for (const auto& trace : traces)
    {
        if (trace.empty())
        {
            break;
        }

        const char* outputFolder = "./_ssdata/";
        bwe::BandwidthEstimator estimator(config);
        bwe::BwBurstTracker burstTracker;
        rtp::SendTimeDial sendTimeDial;
        logger::PacketLogReader reader(::fopen(("./_bwelogs/" + trace).c_str(), "r"));
        uint32_t audioSsrc = identifyAudioSsrc(reader);
        reader.rewind();

        CsvWriter csvOut((outputFolder + trace + ".csv").c_str());
        CsvWriter csvOutAll((outputFolder + trace + "All.csv").c_str());
        logger::PacketLogItem item;
        double prevBw = 0;
        double prevDelay = 0;
        uint64_t start = 0;
        const char* legend = "time, bwo, burst, bw, q, co, delay, size, seqno, stime, rate, slowrate";
        csvOut.writeLine("%s", legend);
        csvOutAll.writeLine("%s", legend);
        logger::info("processing %s", "bweRerun", trace.c_str());
        double prevRtime = 0;

        utils::RateTracker<25> rate(100 * utils::Time::ms);
        const char* formatLine = "%.2f, %.3f,%.1f,%.3f,%.3f,%.3f,%.3f,%u,%u,%.6f,%.2f, %.2f";

        for (int i = 0; reader.getNext(item) && (start == 0 || item.receiveTimestamp - start < utils::Time::sec * 200);
             ++i)
        {
            rate.update(item.size * 8, item.receiveTimestamp);
            burstTracker.onPacketReceived(item.size, item.receiveTimestamp);
            if (item.ssrc == audioSsrc)
            {
                estimator.onUnmarkedTraffic(item.size, item.receiveTimestamp);
                continue;
            }
            start = (start == 0 ? item.receiveTimestamp : start);
            auto localTimestamp = double(item.receiveTimestamp - start) / 1000000;
            auto sendTime = sendTimeDial.toAbsoluteTime(item.transmitTimestamp, item.receiveTimestamp);

            estimator.update(item.size, sendTime, item.receiveTimestamp);
            auto bw = estimator.getEstimate(item.receiveTimestamp);
            auto state = estimator.getState();
            auto delay = estimator.getDelay();
            csvOutAll.writeLine(formatLine,
                localTimestamp,
                bw,
                burstTracker.getBandwidthPercentile(0.50),
                state(1),
                state(0),
                state(2),
                delay,
                item.size,
                item.sequenceNumber,
                double(item.transmitTimestamp >> 18) + double(item.transmitTimestamp & 0x3FFFF) / 262144,
                std::min(5000.0, estimator.getReceiveRate(item.receiveTimestamp)),
                rate.get(item.receiveTimestamp, utils::Time::ms * 2000) * utils::Time::ms);

            if (((i % 1000) == 0) || std::fabs(delay - prevDelay) > 10 || delay > 70 ||
                std::fabs((bw - prevBw) / prevBw) > 0.15)
            {
                csvOut.writeLine(formatLine,
                    localTimestamp,
                    bw,
                    burstTracker.getBandwidthPercentile(0.50),
                    state(1),
                    state(0),
                    state(2),
                    delay,
                    item.size,
                    item.sequenceNumber,
                    double(item.transmitTimestamp >> 18) + double(item.transmitTimestamp & 0x3FFFF) / 262144,
                    std::min(5000.0, estimator.getReceiveRate(item.receiveTimestamp)),
                    rate.get(item.receiveTimestamp, utils::Time::ms * 2000) * utils::Time::ms);

                prevBw = bw;
                prevDelay = delay;
            }
            prevRtime = localTimestamp;
        }
        logger::info("%" PRIu64 "s finished at %.3fkbps",
            "",
            (item.receiveTimestamp - start) / utils::Time::sec,
            prevBw);
    }
}

class BweRerunLimit : public testing::TestWithParam<uint32_t>
{
};

TEST_P(BweRerunLimit, DISABLED_limitedLink)
{
    bwe::Config config;
    /*
        std::array<std::string, 56> trace = {"Transport-113",
            "Transport-116",
            "Transport-119",
            "Transport-122",
            "Transport-125",
            "Transport-128",
            "Transport-131",
            "Transport-134",
            "Transport-136",
            "Transport-138",
            "Transport-140",
            "Transport-148",
            "Transport-150",
            "Transport-152",
            "Transport-154",
            "Transport-156",
            "Transport-158",
            "Transport-181",
            "Transport-186",
            "Transport-191",
            "Transport-196",
            "Transport-198",
            "Transport-202",
            "Transport-204",
            "Transport-206",
            "Transport-208",
            "Transport-210",
            "Transport-212",
            "Transport-214",
            "Transport-223",
            "Transport-225",
            "Transport-231",
            "Transport-233",
            "Transport-235",
            "Transport-237",
            "Transport-239",
            "Transport-241",
            "Transport-243",
            "Transport-245",
            "Transport-247",
            "Transport-249",
            "Transport-251",
            "Transport-253",
            "Transport-255",
            "Transport-257",
            "Transport-259",
            "Transport-261",
            "Transport-263",
            "Transport-265",
            "Transport-267",
            "Transport-269",
            "Transport-271",
            "Transport-273",
            "Transport-275",
            "Transport-277",
            "Transport-317"};*/

    std::array<std::string, 56> trace = {"Transport-5", "Transport-20", "Transport-17"};
    memory::PacketPoolAllocator allocator(8092, "rerun");
    fakenet::NetworkLink link(GetParam(), 1950 * 1024, 3000);
    link.setLossRate(0);
    uint64_t wallClock = 0;
    const char* formatLine = "%u, %.1f,%.1f,%.f,%.3f,%.3f,%u,%u,%.6f, %.2f";
    const char* legend = "time, bwo, bw, q, co, delay, size, seqno, stime, rate";

    for (size_t t = 0; t < trace.size() && !trace[t].empty(); ++t)
    {
        bwe::BandwidthEstimator estimator(config);
        rtp::SendTimeDial sendTimeDial;
        std::string path = "./_bwelogs";
        logger::PacketLogReader reader(::fopen((path + "/" + trace[t]).c_str(), "r"));
        logger::PacketLogItem item;

        CsvWriter csvLog(("./_ssdatall/" + trace[t] + std::to_string(GetParam()) + ".csv").c_str());

        csvLog.writeLine("%s", legend);

        logger::info("processing %s", "bweRerun", trace[t].c_str());
        for (int i = 0; reader.getNext(item); ++i)
        {
            if (i == 0)
            {
                wallClock = sendTimeDial.toAbsoluteTime(item.transmitTimestamp, utils::Time::minute);
                auto packet = memory::makeUniquePacket(allocator, &item, sizeof(item));
                packet->setLength(item.size);

                link.push(std::move(packet), wallClock);
                continue;
            }

            auto sendTime = sendTimeDial.toAbsoluteTime(item.transmitTimestamp, wallClock + utils::Time::sec * 10);
            assert(sendTime >= wallClock);
            for (;;)
            {
                auto minAdvance = std::min(utils::Time::diff(wallClock, sendTime), link.timeToRelease(wallClock));
                assert(minAdvance >= 0);
                wallClock += minAdvance;
                for (auto packet = link.pop(wallClock); packet; packet = link.pop(wallClock))
                {
                    if (packet->getLength() < 1500)
                    {
                        auto* packetItem = reinterpret_cast<logger::PacketLogItem*>(packet->get());

                        auto packetSendTime = sendTimeDial.toAbsoluteTime(packetItem->transmitTimestamp,
                            wallClock + utils::Time::sec * 10);
                        estimator.update(packetItem->size, packetSendTime, wallClock);

                        auto bw = estimator.getEstimate(item.receiveTimestamp);
                        auto state = estimator.getState();
                        auto delay = estimator.getDelay();
                        csvLog.writeLine(formatLine,
                            wallClock / utils::Time::ms,
                            bw,
                            state(1),
                            state(0) / 8,
                            state(2),
                            delay,
                            packetItem->size,
                            packetItem->sequenceNumber,
                            double(item.transmitTimestamp >> 18) + double(item.transmitTimestamp & 0x3FFFF) / 262144,
                            std::min(9000.0, estimator.getReceiveRate(wallClock)));
                    }
                }
                if (wallClock == sendTime)
                {
                    auto packet = memory::makeUniquePacket(allocator, &item, sizeof(item));
                    packet->setLength(item.size);
                    link.push(std::move(packet), wallClock);

                    break;
                }
            }
        }

        double lastEstimate = estimator.getEstimate(wallClock);
        for (wallClock += link.timeToRelease(wallClock);; wallClock += link.timeToRelease(wallClock))
        {
            auto packet = link.pop(wallClock);
            if (!packet)
            {
                break;
            }
            auto* packetItem = reinterpret_cast<logger::PacketLogItem*>(packet->get());
            auto sendTime =
                sendTimeDial.toAbsoluteTime(packetItem->transmitTimestamp, wallClock + utils::Time::sec * 10);
            estimator.update(packetItem->size, sendTime, wallClock);
            auto bw = estimator.getEstimate(item.receiveTimestamp);
            auto state = estimator.getState();
            auto delay = estimator.getDelay();
            csvLog.writeLine(formatLine,
                wallClock / utils::Time::ms,
                bw,
                state(1),
                state(0) / 8,
                state(2),
                delay,
                packetItem->size,
                packetItem->sequenceNumber,
                double(item.transmitTimestamp >> 18) + double(item.transmitTimestamp & 0x3FFFF) / 262144,
                std::min(9000.0, estimator.getReceiveRate(wallClock)));

            lastEstimate = estimator.getEstimate(wallClock);
        }
        logger::info("finished at %.3fkbps", "", lastEstimate);
    }
}

INSTANTIATE_TEST_SUITE_P(BwessRerun,
    BweRerunLimit,
    testing::Values(4500, 4800, 4900, 5000, 5100, 5200, 5600, 6500, 7000, 10000, 20000));
