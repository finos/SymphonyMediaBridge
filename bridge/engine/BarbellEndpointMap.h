#pragma once
#include "bridge/engine/NeighbourMembership.h"
#include "concurrency/MpmcHashmap.h"
#include "memory/Array.h"
#include "utils/FixString.h"
#include <array>

namespace bridge
{

using EndpointIdString = utils::FixString<42>;
using BarbellEndpointIdMap = concurrency::MpmcHashmap32<size_t, EndpointIdString>;

struct BarbellMapItem
{
    BarbellMapItem() {}
    BarbellMapItem(const BarbellMapItem& rhs)
    {
        std::strcpy(endpointId, rhs.endpointId);
        oldSsrcs = rhs.oldSsrcs;
        newSsrcs = rhs.newSsrcs;
    }

    BarbellMapItem& operator=(const BarbellMapItem& rhs)
    {
        std::strcpy(endpointId, rhs.endpointId);
        oldSsrcs = rhs.oldSsrcs;
        newSsrcs = rhs.newSsrcs;
        return *this;
    }

    bool hasChanged() const
    {
        if (oldSsrcs.size() != newSsrcs.size())
        {
            return true;
        }

        for (size_t i = 0; i < oldSsrcs.size(); ++i)
        {
            if (oldSsrcs[i] != newSsrcs[i])
            {
                return true;
            }
        }

        return false;
    }

    char endpointId[EndpointIdString::capacity];
    memory::Array<uint32_t, 2> oldSsrcs;
    memory::Array<uint32_t, 2> newSsrcs;
    size_t endpointIdHash = 0;
    engine::NeighbourMembershipArray neighbours;
};

} // namespace bridge
