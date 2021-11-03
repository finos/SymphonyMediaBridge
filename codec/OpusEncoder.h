#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace codec
{

class OpusEncoder
{
public:
    OpusEncoder();
    OpusEncoder(const OpusEncoder&) = delete;
    ~OpusEncoder();

    bool isInitialized() const { return _initialized; }

    int32_t encode(const int16_t* decodedData,
        const size_t frames,
        unsigned char* payloadStart,
        const size_t payloadMaxFrames);

private:
    struct OpaqueEncoderState;

    bool _initialized;
    OpaqueEncoderState* _state;
};

} // namespace codec
