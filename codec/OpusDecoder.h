#pragma once

#include <atomic>
#include <cstdint>
#include <unistd.h>

namespace codec
{

class OpusDecoder
{
public:
    OpusDecoder();
    ~OpusDecoder();

    bool isInitialized() const { return _initialized; }

    uint32_t getSequenceNumber() const { return _sequenceNumber; }

    int32_t decode(const unsigned char* payloadStart,
        int32_t payloadLength,
        unsigned char* decodedData,
        const size_t framesInDecodedPacket,
        const bool decodeFec);

    int32_t getLastPacketDuration();

private:
    struct OpaqueDecoderState;

    bool _initialized;
    OpaqueDecoderState* _state;
    uint32_t _sequenceNumber;
    std::atomic_flag _lock = ATOMIC_FLAG_INIT;
};

} // namespace codec
