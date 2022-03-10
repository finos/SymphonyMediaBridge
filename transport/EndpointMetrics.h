#include <cstdint>

#pragma once

struct EndpointMetrics
{
    EndpointMetrics(uint32_t sQueue, double rBitrate, double sBitrate)
        : sendQueue(sQueue)
        , receiveBitrate(rBitrate)
        , sendBitrate(sBitrate)
    { }

    EndpointMetrics() : EndpointMetrics(0, 0.0, 0.0) { }

    EndpointMetrics& operator+=(const EndpointMetrics& rhs)
    {
        sendQueue += rhs.sendQueue;
        receiveBitrate += rhs.receiveBitrate;
        sendBitrate += rhs.sendBitrate;
        return *this;
    }

    uint32_t sendQueue;
    double receiveBitrate;
    double sendBitrate;
};

inline EndpointMetrics operator+(const EndpointMetrics& lhs, const EndpointMetrics& rhs)
{
    EndpointMetrics m(lhs);
    m += rhs;
    return m;
}
