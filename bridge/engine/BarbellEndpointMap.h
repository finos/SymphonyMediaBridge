#pragma once
#include "concurrency/MpmcHashmap.h"
#include "memory/Array.h"
#include "utils/FixString.h"
#include <array>

namespace bridge
{

using EndpointIdString = FixString<42>;
using BarbellEndpointIdMap = concurrency::MpmcHashmap32<size_t, EndpointIdString>;

struct BarbellMapItem
{
    BarbellMapItem() { endpointId[0] = 0; }
    BarbellMapItem(const BarbellMapItem& rhs) : endpointIdHash(rhs.endpointIdHash)
    {
        std::strcpy(endpointId, rhs.endpointId);
        for (auto ssrc : rhs.ssrcs)
        {
            ssrcs.push_back(ssrc);
        }
    }

    BarbellMapItem& operator=(const BarbellMapItem& rhs)
    {
        endpointIdHash = rhs.endpointIdHash;
        std::strcpy(endpointId, rhs.endpointId);
        ssrcs.clear();
        for (auto ssrc : rhs.ssrcs)
        {
            ssrcs.push_back(ssrc);
        }
        return *this;
    }

    char endpointId[EndpointIdString::capacity];
    size_t endpointIdHash = 0;
    memory::Array<uint32_t, 2> ssrcs;
};

} // namespace bridge
