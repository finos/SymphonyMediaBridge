#pragma once

#include "transport/PacketCounters.h"
#include "transport/TransportStats.h"
#include <algorithm>
#include <cstdint>

namespace bridge
{

namespace EngineStats
{

struct MixerStats
{
    double audioInQueueSamples = 0;
    uint32_t maxAudioInQueueSamples = 0;
    uint32_t audioInQueues = 0;

    struct MediaStats
    {
        transport::PacketCounters audio;
        transport::PacketCounters video;

        transport::PacketCounters total() const { return audio + video; }

        transport::TransportStats transport;
    };

    MediaStats inbound;
    MediaStats outbound;

    uint32_t pacingQueue = 0;
    uint32_t rtxPacingQueue = 0;
    uint32_t rtpUdp = 0;
    uint32_t rtpTcp = 0;
    uint32_t rtcpUdp = 0;
    uint32_t rtcpTcp = 0;

    MixerStats& operator+=(const MixerStats& b)
    {
        audioInQueueSamples += b.audioInQueueSamples;
        audioInQueues += b.audioInQueues;
        maxAudioInQueueSamples = std::max(maxAudioInQueueSamples, b.maxAudioInQueueSamples);

        inbound.audio += b.inbound.audio;
        inbound.video += b.inbound.video;
        outbound.audio += b.outbound.audio;
        outbound.video += b.outbound.video;

        inbound.transport += b.inbound.transport;
        outbound.transport += b.outbound.transport;

        pacingQueue += b.pacingQueue;
        rtxPacingQueue += b.rtxPacingQueue;

        rtpUdp += b.rtpUdp;
        rtpTcp += b.rtpTcp;
        rtcpUdp += b.rtcpUdp;
        rtcpTcp += b.rtcpTcp;

        return *this;
    }

    double getAvgAudioInQueueSamples() const { return audioInQueueSamples / std::max(1u, audioInQueues); }
};

struct EngineStats
{
    double avgIdle = 100.0;
    int32_t timeSlipCount = 0;

    uint32_t pollPeriodMs = 1;

    MixerStats activeMixers;
};

} // namespace EngineStats

} // namespace bridge
