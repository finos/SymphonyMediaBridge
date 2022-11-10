#include "bridge/MixerManagerAsync.h"
#include "bridge/RecordingDescription.h"
#include "bridge/engine/EngineMixer.h"
#include "utils/Function.h"

namespace jobmanager
{
class JobManager;
}

namespace codec
{
class OpusDecoder;
}

namespace utils
{
class Function;
}

namespace bridge
{
bool MixerManagerAsync::asyncAudioStreamRemoved(EngineMixer& mixer, const EngineAudioStream& audioStream)
{
    return post(utils::bind(&MixerManagerAsync::audioStreamRemoved, this, std::ref(mixer), std::cref(audioStream)));
}

bool MixerManagerAsync::asyncEngineMixerRemoved(EngineMixer& mixer)
{
    return post(utils::bind(&MixerManagerAsync::engineMixerRemoved, this, std::ref(mixer)));
}

bool MixerManagerAsync::asyncFreeVideoPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash)
{
    return post(utils::bind(&MixerManagerAsync::freeVideoPacketCache, this, std::ref(mixer), ssrc, endpointIdHash));
}

bool MixerManagerAsync::asyncAllocateVideoPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash)
{
    return post(utils::bind(&MixerManagerAsync::allocateVideoPacketCache, this, std::ref(mixer), ssrc, endpointIdHash));
}

bool MixerManagerAsync::asyncAllocateRecordingRtpPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash)
{
    return post(
        utils::bind(&MixerManagerAsync::allocateRecordingRtpPacketCache, this, std::ref(mixer), ssrc, endpointIdHash));
}

bool MixerManagerAsync::asyncVideoStreamRemoved(EngineMixer& engineMixer, const EngineVideoStream& videoStream)
{
    return post(
        utils::bind(&MixerManagerAsync::videoStreamRemoved, this, std::ref(engineMixer), std::cref(videoStream)));
}

bool MixerManagerAsync::asyncSctpReceived(EngineMixer& mixer, memory::UniquePacket& msgPacket, size_t endpointIdHash)
{
    return post(utils::bind(&MixerManagerAsync::sctpReceived,
        this,
        std::ref(mixer),
        utils::moveParam(msgPacket),
        endpointIdHash));
}

bool MixerManagerAsync::asyncDataStreamRemoved(EngineMixer& mixer, const EngineDataStream& dataStream)
{
    return post(utils::bind(&MixerManagerAsync::dataStreamRemoved, this, std::ref(mixer), std::cref(dataStream)));
}

bool MixerManagerAsync::asyncFreeRecordingRtpPacketCache(EngineMixer& mixer, uint32_t ssrc, size_t endpointIdHash)
{
    return post(
        utils::bind(&MixerManagerAsync::freeRecordingRtpPacketCache, this, std::ref(mixer), ssrc, endpointIdHash));
}

bool MixerManagerAsync::asyncBarbellRemoved(EngineMixer& mixer, const EngineBarbell& barbell)
{
    return post(utils::bind(&MixerManagerAsync::barbellRemoved, this, std::ref(mixer), std::cref(barbell)));
}

bool MixerManagerAsync::asyncRecordingStreamRemoved(EngineMixer& mixer, const EngineRecordingStream& recordingStream)
{
    return post(
        utils::bind(&MixerManagerAsync::recordingStreamRemoved, this, std::ref(mixer), std::cref(recordingStream)));
}

bool MixerManagerAsync::asyncRemoveRecordingTransport(EngineMixer& mixer, const char* streamId, size_t endpointIdHash)
{
    return post(utils::bind(&MixerManagerAsync::removeRecordingTransport,
        this,
        std::ref(mixer),
        utils::FixString<42>(streamId),
        endpointIdHash));
}

bool MixerManagerAsync::asyncInboundSsrcContextRemoved(EngineMixer& mixer,
    uint32_t ssrc,
    codec::OpusDecoder* opusDecoder)
{
    return post(utils::bind(&MixerManagerAsync::inboundSsrcContextRemoved, this, std::ref(mixer), ssrc, opusDecoder));
}

bool MixerManagerAsync::asyncMixerTimedOut(EngineMixer& mixer)
{
    return post(utils::bind(&MixerManagerAsync::mixerTimedOut, this, std::ref(mixer)));
}

bool MixerManagerAsync::asyncEngineRecordingStopped(EngineMixer& mixer, const RecordingDescription& recordingDesc)
{
    return post(utils::bind(&MixerManagerAsync::engineRecordingStopped, this, std::ref(mixer), recordingDesc));
}

} // namespace bridge
