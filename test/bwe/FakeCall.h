#pragma once

#include "memory/PacketPoolAllocator.h"
#include "test/CsvWriter.h"
#include "test/integration/emulator/TimeTurner.h"
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
        uint64_t duration,
        const char* bweDumpFile = nullptr);

    ~Call();

    void addLink(NetworkLink* link);
    void addSource(MediaSource* source);

    bool run(uint64_t period);
    uint64_t getTime() { return _timeCursor.getAbsoluteTime(); }

    double getEstimate() const;
    bwe::Estimator& _bwe;

private:
    memory::PacketPoolAllocator& _allocator;
    std::vector<MediaSource*> _mediaSources;
    std::vector<NetworkLink*> _links;

    emulator::TimeTurner _timeCursor;
    uint64_t _startTime;
    uint64_t _endTime;

    std::unique_ptr<CsvWriter> _csvWriter;
};
} // namespace fakenet