#include "codec/OpusEncoder.h"
#include "codec/Opus.h"
#include "utils/CheckedCast.h"
#include <opus/opus.h>

namespace codec
{

struct OpusEncoder::OpaqueEncoderState
{
    ::OpusEncoder* _state;
};

OpusEncoder::OpusEncoder() : _initialized(false), _state(new OpaqueEncoderState{nullptr})
{
    int32_t opusError = 0;
    _state->_state = opus_encoder_create(Opus::sampleRate, Opus::channelsPerFrame, OPUS_APPLICATION_VOIP, &opusError);
    if (opusError != OPUS_OK)
    {
        return;
    }

    opus_encoder_ctl(_state->_state, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_NARROWBAND));
    opus_encoder_ctl(_state->_state, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(_state->_state, OPUS_SET_INBAND_FEC(1));

    _initialized = true;
}

OpusEncoder::~OpusEncoder()
{
    if (_state->_state)
    {
        opus_encoder_destroy(_state->_state);
    }
    delete _state;
}

int32_t OpusEncoder::encode(const int16_t* decodedData,
    const size_t frames,
    unsigned char* payloadStart,
    const size_t payloadMaxFrames)
{
    if (!_initialized)
    {
        assert(false);
        return -1;
    }
    assert(_state->_state);

    return opus_encode(_state->_state,
        decodedData,
        utils::checkedCast<int32_t>(frames),
        payloadStart,
        utils::checkedCast<int32_t>(payloadMaxFrames));
}

} // namespace codec
