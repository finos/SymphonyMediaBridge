#pragma once
#include "bridge/RtpMap.h"
#include "memory/PacketPoolAllocator.h"
#include <cstdio>
#include <inttypes.h>
namespace memory
{
class Packet;

}
namespace test
{

class VideoSource
{
public:
    VideoSource(memory::PacketPoolAllocator& allocator);
    ~VideoSource();

    bool open(const char* filename);
    bool isOpen() const { return _fHandle != nullptr; }

    memory::Packet* getNext(uint64_t now);
    void free(memory::Packet* packet);
    bridge::RtpMap getRtpMap() const { return _rtpMap; }

    memory::PacketPoolAllocator& _allocator;

private:
    memory::Packet* readPacket();

    FILE* _fHandle;
    memory::Packet* _packet;

    uint64_t _timeReference;
    struct RtpCursor
    {
        uint64_t timestamp;
        uint64_t sequenceNumber;
        RtpCursor() : timestamp(0), sequenceNumber(0) {}
    } _cursor;

    uint32_t _rtpTimestampOffset;

    bool _aborted;
    bridge::RtpMap _rtpMap;
};

}