#include "codec/OpusDecoder.h"
#include "codec/Opus.h"
#include "concurrency/ScopedSpinLocker.h"
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

OpusDecoder::OpusDecoder() : _initialized(false), _state(new OpaqueDecoderState{nullptr}), _sequenceNumber(0)
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

int32_t OpusDecoder::decode(const unsigned char* payloadStart,
    int32_t payloadLength,
    unsigned char* decodedData,
    const size_t framesInDecodedPacket,
    const bool decodeFec)
{
    if (!_initialized)
    {
        assert(false);
        return -1;
    }
    assert(_state->_state);

    concurrency::ScopedSpinLocker locker(_lock);

    return opus_decode(_state->_state,
        payloadStart,
        payloadLength,
        reinterpret_cast<int16_t*>(decodedData),
        utils::checkedCast<int32_t>(framesInDecodedPacket),
        decodeFec ? 1 : 0);
}

int32_t OpusDecoder::getLastPacketDuration()
{
    if (!_initialized)
    {
        assert(false);
        return -1;
    }

    concurrency::ScopedSpinLocker locker(_lock);

    int32_t lastPacketDuration = 0;
    opus_decoder_ctl(_state->_state, OPUS_GET_LAST_PACKET_DURATION(&lastPacketDuration));
    return lastPacketDuration;
}

} // namespace codec
