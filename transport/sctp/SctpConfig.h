#pragma once
#include <cstdint>

namespace sctp
{

struct SctpConfig
{
    struct // in ms
    {
        uint64_t initial = 2000;
        uint64_t min = 500; // recommended 1s
        uint64_t max = 10000;
        double alpha = 0.125;
        double beta = 0.25;
    } RTO;

    struct SignalRetransmit
    {
        uint32_t timeout = 400;
        int maxRetransmits = 8;
        uint32_t maxTimeout = 3000;
    } init;

    struct
    {
        uint32_t interval = 30000;
        int burst = 1;
    } heartbeat;

    uint32_t cookieLifeTime = 60000;

    struct
    {
        size_t slowStartThreshold = 32 * 1024;
        int maxBurst = 4;
        int maxRetransmits = 10;
    } flow;

    struct
    {
        uint32_t initial = 1024 * 64;
        uint32_t min = 1024;
        uint32_t max = 1024 * 1024;
    } receiveWindow;

    struct
    {
        uint32_t initial = 1024;
        uint32_t max = 4096;
    } mtu;

    size_t transmitBufferSize = 512 * 1024;
    size_t receiveBufferSize = 512 * 1024;
};

} // namespace sctp