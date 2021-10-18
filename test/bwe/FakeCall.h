#pragma once

#include "memory/PacketPoolAllocator.h"
#include <vector>

namespace bwe
{
class Estimator;
}
namespace fakenet
{
class NetworkLink;
class MediaSource;

class Call
{
public:
    Call(memory::PacketPoolAllocator& allocator,
        bwe::Estimator& estimator,
        NetworkLink* firstLink,
        bool audio,
        uint64_t duration);

    ~Call();

    void addLink(NetworkLink* link);
    void addSource(MediaSource* source);

    bool run(uint64_t period);
    uint64_t getTime() { return _timeCursor; }

    double getEstimate() const;
    bwe::Estimator& _bwe;

private:
    memory::PacketPoolAllocator& _allocator;
    std::vector<MediaSource*> _mediaSources;
    std::vector<NetworkLink*> _links;

    uint64_t _timeCursor;
    uint64_t _endTime;
};
} // namespace fakenet