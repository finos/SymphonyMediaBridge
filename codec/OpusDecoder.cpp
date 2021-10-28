#include "codec/OpusDecoder.h"
#include "codec/Opus.h"
#include "utils/CheckedCast.h"
#include <cassert>
#include <opus/opus.h>
#include <stddef.h>

namespace codec
{

struct OpusDecoder::OpaqueDecoderState
{
    ::OpusDecoder* _state;
};

OpusDecoder::OpusDecoder()
    : _initialized(false),
      _hasReceivedFirstPacket(false),
      _state(new OpaqueDecoderState{nullptr}),
      _sequenceNumber(0)
{
    int32_t opusError = 0;
    _state->_state = opus_decoder_create(Opus::sampleRate, Opus::channelsPerFrame, &opusError);
    if (opusError != OPUS_OK)
    {
        return;
    }

    _initialized = true;
}

OpusDecoder::~OpusDecoder()
{
    if (_state->_state)
    {
        opus_decoder_destroy(_state->_state);
    }
    delete _state;
}

// return true if there is more output frames
bool OpusDecoder::decode(const unsigned char* payloadStart,
    const size_t payloadLength,
    const uint16_t sequenceNumber,
    uint32_t decodeBufferSize,
    int16_t* decodedData,
    uint32_t& bytesProduced)
{
    if (!_initialized)
    {
        assert(false);
        return -1;
    }
    assert(_state->_state);

    int heal = 0;
    if (!_hasReceivedFirstPacket)
    {
        _sequenceNumber = sequenceNumber - 1;
        _hasReceivedFirstPacket = true;
    }

    const auto seqDiff = static_cast<int16_t>(sequenceNumber - _sequenceNumber);
    if (seqDiff == 1 || seqDiff > 10)
    {
        _sequenceNumber = sequenceNumber;
        heal = 0;
    }
    else if (seqDiff > 1)
    {
        heal = 1;
        ++_sequenceNumber;
        int32_t lastPacketDuration = 0;
        opus_decoder_ctl(_state->_state, OPUS_GET_LAST_PACKET_DURATION(&lastPacketDuration));
        decodeBufferSize = lastPacketDuration * Opus::channelsPerFrame * Opus::bytesPerSample;
    }
    else
    {
        // old packet
        bytesProduced = 0;
        return false;
    }

    auto framesProduced = opus_decode(_state->_state,
        payloadStart,
        payloadLength,
        decodedData,
        utils::checkedCast<int32_t>(decodeBufferSize / (Opus::channelsPerFrame * Opus::bytesPerSample)),
        heal);

    bytesProduced = framesProduced * Opus::channelsPerFrame * Opus::bytesPerSample;
    return heal == 1;
}

} // namespace codec
