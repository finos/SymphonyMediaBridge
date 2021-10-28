#pragma once

#include <atomic>
#include <cstdint>

namespace codec
{

class OpusDecoder
{
public:
    OpusDecoder();
    OpusDecoder(const OpusDecoder&) = delete;
    ~OpusDecoder();

    bool isInitialized() const { return _initialized; }

    bool decode(const unsigned char* payloadStart,
        const size_t payloadLength,
        const uint16_t sequenceNumber,
        uint32_t decodeBufferSize,
        int16_t* decodedData,
        uint32_t& bytesProduced);

private:
    struct OpaqueDecoderState;

    bool _initialized;
    bool _hasReceivedFirstPacket;
    OpaqueDecoderState* _state;
    uint16_t _sequenceNumber;
};

} // namespace codec
