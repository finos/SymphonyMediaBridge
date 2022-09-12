#pragma once
#include "utils/Time.h"
#include <algorithm>
#include <cinttypes>
namespace transport
{
struct PacketCounters
{
    bool empty() const { return octets == 0 && packets == 0 && lostPackets == 0; }

    double getReceiveLossRatio() const
    {
        return static_cast<double>(lostPackets) / std::max(uint64_t(1), getPacketsSent());
    }

    double getSendLossRatio() const { return static_cast<double>(lostPackets) / std::max(uint64_t(1), packets); }

    uint64_t getPacketsSent() const { return packets; }
    uint64_t getPacketsReceived() const { return packets - lostPackets; }

    PacketCounters& operator+=(const PacketCounters& b)
    {
        bitrateKbps += b.bitrateKbps;
        packetsPerSecond += b.packetsPerSecond;
        packets += b.packets;
        lostPackets += b.lostPackets;
        octets += b.octets;
        activeStreamCount += b.activeStreamCount;

        return *this;
    }

    uint64_t octets = 0; // actually sent / actually received
    uint64_t packets = 0; // actually sent
    uint64_t lostPackets = 0;

    uint32_t bitrateKbps = 0;
    uint32_t packetsPerSecond = 0;
    uint64_t period = 0;
    uint32_t activeStreamCount = 0;
};

inline PacketCounters operator-(PacketCounters a, const PacketCounters& b)
{
    a.octets -= b.octets;
    a.packets -= b.packets;
    a.bitrateKbps -= b.bitrateKbps;
    a.packetsPerSecond -= b.packetsPerSecond;
    a.period -= b.period;
    a.lostPackets = (b.lostPackets > a.lostPackets ? 0 : a.lostPackets - b.lostPackets);
    return a;
}

inline PacketCounters operator+(PacketCounters a, const PacketCounters& b)
{
    a += b;
    return a;
}

} // namespace transport
