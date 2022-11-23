#include <cstdint>

#pragma once

struct EndpointMetrics
{
    EndpointMetrics(uint32_t sQueue, double rKbps, double sKbs, uint64_t sendDrops)
        : sendQueue(sQueue),
          receiveKbps(rKbps),
          sendKbps(sKbs),
          sendQueueDrops(sendDrops)
    {
    }

    EndpointMetrics() : EndpointMetrics(0, 0.0, 0.0, 0) {}

    EndpointMetrics& operator+=(const EndpointMetrics& rhs)
    {
        sendQueue += rhs.sendQueue;
        receiveKbps += rhs.receiveKbps;
        sendKbps += rhs.sendKbps;
        sendQueueDrops += rhs.sendQueueDrops;
        return *this;
    }

    uint32_t sendQueue;
    double receiveKbps;
    double sendKbps;
    uint64_t sendQueueDrops;
};

inline EndpointMetrics operator+(const EndpointMetrics& lhs, const EndpointMetrics& rhs)
{
    EndpointMetrics m(lhs);
    m += rhs;
    return m;
}
