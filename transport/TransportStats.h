#pragma once
#include "utils/StdExtensions.h"
#include <cinttypes>

namespace transport
{
struct TransportStats
{
    TransportStats()
    {
        std::memset(&lossGroup, 0, std::size(lossGroup) * sizeof(uint16_t));
        std::memset(&rttGroup, 0, std::size(rttGroup) * sizeof(uint16_t));
        std::memset(&bandwidthEstimateGroup, 0, 10 * sizeof(uint16_t));
    }

    TransportStats& operator+=(const TransportStats& b)
    {
        for (size_t i = 0; i < std::size(lossGroup); ++i)
        {
            lossGroup[i] += b.lossGroup[i];
            rttGroup[i] += b.rttGroup[i];
        }
        for (size_t i = 0; i < std::size(bandwidthEstimateGroup); ++i)
        {
            bandwidthEstimateGroup[i] += b.bandwidthEstimateGroup[i];
        }
        return *this;
    }

    void addLossGroup(double ratio)
    {
        double limit = 0.01;
        for (size_t i = 0; i < std::size(lossGroup); ++i)
        {
            if (ratio < limit)
            {
                ++lossGroup[i];
                return;
            }
            limit = limit * 2;
        }
        ++lossGroup[std::size(lossGroup) - 1];
    }

    void addRttGroup(uint32_t rttMs)
    {
        uint32_t limit = 100;
        for (size_t i = 0; i < std::size(rttGroup); ++i)
        {
            if (rttMs < limit)
            {
                ++rttGroup[i];
                return;
            }
            limit = limit * 2;
        }
        ++rttGroup[std::size(rttGroup) - 1];
    }

    void addBandwidthGroup(uint32_t bandwidthKbps)
    {
        uint32_t limit = 125;
        for (size_t i = 0; i < std::size(bandwidthEstimateGroup); ++i)
        {
            if (bandwidthKbps < limit)
            {
                ++bandwidthEstimateGroup[i];
                return;
            }
            limit = limit * 2;
        }
        ++bandwidthEstimateGroup[std::size(bandwidthEstimateGroup) - 1];
    }

    uint16_t lossGroup[6]; // < 0,1,2,4,8, 100%
    uint16_t rttGroup[6]; // < 0.1, 0.2, 0.4, 0.8, 1.6, longer
    uint16_t bandwidthEstimateGroup[10]; // < 125k, 250k, 500k, 1M, 2M, 4M, 8M, 16M, 32M, more
};
} // namespace transport