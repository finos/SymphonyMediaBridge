#pragma once

#include <cstdint>
#include <stddef.h>

namespace codec
{

class OpusDecoder
{
public:
    OpusDecoder();
    ~OpusDecoder();

    bool isInitialized() const { return _initialized; }
    bool hasDecoded() const { return _hasDecodedPacket; }

    uint32_t getExpectedSequenceNumber() const { return _sequenceNumber + 1; }

    int32_t decode(uint32_t extendedSequenceNumber,
        const unsigned char* payloadStart,
        int32_t payloadLength,
        unsigned char* decodedData,
        const size_t framesInDecodedPacket);

    int32_t conceal(unsigned char* decodedData);
    int32_t conceal(const unsigned char* payloadStart, int32_t payloadLength, unsigned char* decodedData);

    int32_t getLastPacketDuration();

    void onUnusedPacketReceived(uint32_t extendedSequenceNumber);

private:
    struct OpaqueDecoderState;

    bool _initialized;
    OpaqueDecoderState* _state;
    uint32_t _sequenceNumber;
    bool _hasDecodedPacket;
};

} // namespace codec
