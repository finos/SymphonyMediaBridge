#pragma once
#include "concurrency/MpmcHashmap.h"
#include "memory/Array.h"
#include <cinttypes>
#include <cstddef>

namespace bridge
{
namespace engine
{
using NeighbourMembershipArray = memory::Array<uint32_t, 64>;

struct NeighbourMembership
{
    NeighbourMembership(size_t endpointIHash) : endpointIdHash(endpointIHash) {}

    template <class V>
    NeighbourMembership(size_t endpointIdHash, const V& groups) : endpointIdHash(endpointIdHash)
    {
        for (auto& m : groups)
        {
            memberships.push_back(m);
        }
    }

    size_t endpointIdHash;
    NeighbourMembershipArray memberships; // hashed group ids or c9 user id
};

using EndpointMembershipsMap = concurrency::MpmcHashmap32<size_t, engine::NeighbourMembership>;

} // namespace engine
} // namespace bridge
