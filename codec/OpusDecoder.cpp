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

/** @return number of samples decoded
 * */
int32_t OpusDecoder::decode(uint32_t extendedSequenceNumber,
    const unsigned char* payloadStart,
    int32_t payloadLength,
    unsigned char* decodedData,
    const size_t framesInDecodedPacket)
{
    if (!_initialized)
    {
        assert(false);
        return -1;
    }
    assert(_state->_state);

    _sequenceNumber = extendedSequenceNumber;
    _hasDecodedPacket = true;

    return opus_decode(_state->_state,
        payloadStart,
        payloadLength,
        reinterpret_cast<int16_t*>(decodedData),
        utils::checkedCast<int32_t>(framesInDecodedPacket),
        0);
}

// re-construct packet before the most previously lost
int32_t OpusDecoder::conceal(unsigned char* decodedData)
{
    if (!_initialized)
    {
        assert(false);
        return -1;
    }
    assert(_state->_state);

    return opus_decode(_state->_state, nullptr, 0, reinterpret_cast<int16_t*>(decodedData), getLastPacketDuration(), 1);
}

// re-construct the packet previous to the received packet
int32_t OpusDecoder::conceal(const unsigned char* payloadStart, int32_t payloadLength, unsigned char* decodedData)
{
    if (!_initialized)
    {
        assert(false);
        return -1;
    }
    assert(_state->_state);

    return opus_decode(_state->_state,
        payloadStart,
        payloadLength,
        reinterpret_cast<int16_t*>(decodedData),
        getLastPacketDuration(),
        1);
}

int32_t OpusDecoder::getLastPacketDuration()
{
    if (!_initialized)
    {
        assert(false);
        return -1;
    }

    int32_t lastPacketDuration = 0;
    opus_decoder_ctl(_state->_state, OPUS_GET_LAST_PACKET_DURATION(&lastPacketDuration));
    return lastPacketDuration;
}

} // namespace codec
