#include <cstdint>

#pragma once

struct EndpointMetrics
{
    EndpointMetrics(uint32_t rQueueCount, uint32_t sQueueCount)
        : rxQueue(rQueueCount)
        , txQueue(sQueueCount)
    { }

    EndpointMetrics() : EndpointMetrics(0, 0) { }

    EndpointMetrics& operator+=(const EndpointMetrics& rhs)
    {
        rxQueue += rhs.rxQueue;
        txQueue += rhs.txQueue;
        return *this;
    }

    uint32_t rxQueue;
    uint32_t txQueue;
};

inline EndpointMetrics operator+(const EndpointMetrics& lhs, const EndpointMetrics& rhs)
{
    EndpointMetrics m(lhs);
    m += rhs;
    return m;
}
