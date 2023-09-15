#pragma once

#include "bridge/MixerManagerAsync.h"
#include <gmock/gmock.h>

namespace test
{

class MixerManagerAsyncMock : public bridge::MixerManagerAsync
{
public:
    MOCK_METHOD(bool, post, (utils::Function && task), (override));

    MOCK_METHOD(void,
        audioStreamRemoved,
        (bridge::EngineMixer & mixer, const bridge::EngineAudioStream& audioStream),
        (override));

    MOCK_METHOD(void, engineMixerRemoved, (bridge::EngineMixer & mixer), (override));
    MOCK_METHOD(void,
        freeVideoPacketCache,
        (bridge::EngineMixer & mixer, uint32_t ssrc, size_t endpointIdHash),
        (override));

    MOCK_METHOD(void,
        allocateVideoPacketCache,
        (bridge::EngineMixer & mixer, uint32_t ssrc, size_t endpointIdHash),
        (override));

    MOCK_METHOD(void,
        allocateRecordingRtpPacketCache,
        (bridge::EngineMixer & mixer, uint32_t ssrc, size_t endpointIdHash),
        (override));

    MOCK_METHOD(void,
        videoStreamRemoved,
        (bridge::EngineMixer & engineMixer, const bridge::EngineVideoStream& videoStream),
        (override));

    MOCK_METHOD(void,
        sctpReceived,
        (bridge::EngineMixer & mixer, memory::UniquePacket msgPacket, size_t endpointIdHash),
        (override));

    MOCK_METHOD(void,
        dataStreamRemoved,
        (bridge::EngineMixer & mixer, const bridge::EngineDataStream& dataStream),
        (override));

    MOCK_METHOD(void,
        freeRecordingRtpPacketCache,
        (bridge::EngineMixer & mixer, uint32_t ssrc, size_t endpointIdHash),
        (override));

    MOCK_METHOD(void, barbellRemoved, (bridge::EngineMixer & mixer, const bridge::EngineBarbell& barbell), (override));
    MOCK_METHOD(void,
        recordingStreamRemoved,
        (bridge::EngineMixer & mixer, const bridge::EngineRecordingStream& recordingStream),
        (override));

    MOCK_METHOD(void,
        removeRecordingTransport,
        (bridge::EngineMixer & mixer, bridge::EndpointIdString streamId, size_t endpointIdHash),
        (override));

    MOCK_METHOD(void,
        inboundSsrcContextRemoved,
        (bridge::EngineMixer & mixer, uint32_t ssrc, codec::OpusDecoder* opusDecoder),
        (override));

    MOCK_METHOD(void, mixerTimedOut, (bridge::EngineMixer & mixer), (override));
    MOCK_METHOD(void,
        engineRecordingStopped,
        (bridge::EngineMixer & mixer, const bridge::RecordingDescription& recordingDesc),
        (override));
};

}; // namespace test
