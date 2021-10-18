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
    ~OpusEncoder();

    bool isInitialized() const { return _initialized; }

    int32_t encode(const uint8_t* decodedData,
        const size_t frames,
        unsigned char* payloadStart,
        const size_t payloadMaxFrames);

private:
    struct OpaqueEncoderState;

    bool _initialized;
    OpaqueEncoderState* _state;
    std::atomic_flag _lock = ATOMIC_FLAG_INIT;
};

} // namespace codec
