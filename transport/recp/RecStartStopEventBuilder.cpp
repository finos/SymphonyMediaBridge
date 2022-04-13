#include "transport/recp/RecStartStopEventBuilder.h"

using namespace recp;

namespace
{

const uint8_t audioFlag = 0x01;
const uint8_t videoFlag = 0x02;

uint8_t& modalityFlags(memory::Packet& packet)
{
    // direction is placed in the 2nd byte of payload (right after the direction)
    return packet.get()[REC_HEADER_SIZE];
}

template <class TFlag>
void setFlag(TFlag& flag, TFlag mask, bool enabled)
{
    if (enabled)
    {
        flag |= mask;
    }
    else
    {
        flag &= ~mask;
    }
}

uint8_t& getRecordingIdSizeRef(memory::Packet& packet)
{
    return reinterpret_cast<uint8_t&>(packet.get()[REC_HEADER_SIZE + 1]);
}

uint8_t& getUserIdSizeRef(memory::Packet& packet)
{
    return reinterpret_cast<uint8_t&>(packet.get()[REC_HEADER_SIZE + 2]);
}

uint8_t* getRecordingIdBuffRef(memory::Packet& packet)
{
    // Right after the reserved byte
    return &(packet.get()[REC_HEADER_SIZE + 4]);
}

uint8_t* getUserIdBuffRef(memory::Packet& packet)
{
    // Right after the reserved byte
    return &(packet.get()[REC_HEADER_SIZE + 4 + getRecordingIdSizeRef(packet)]);
}
} // namespace

RecStartStopEventBuilder& RecStartStopEventBuilder::setAudioEnabled(bool isAudioEnabled)
{
    auto* packet = getPacket();
    if (packet)
    {
        setFlag(modalityFlags(*packet), audioFlag, isAudioEnabled);
    }

    return *this;
}

RecStartStopEventBuilder& RecStartStopEventBuilder::setVideoEnabled(bool isVideoEnabled)
{
    auto* packet = getPacket();
    if (packet)
    {
        setFlag(modalityFlags(*packet), videoFlag, isVideoEnabled);
    }

    return *this;
}

RecStartStopEventBuilder& RecStartStopEventBuilder::setRecordingId(const std::string& recordingId)
{
    auto packet = getPacket();
    if (!packet)
    {
        return *this;
    }

    getRecordingIdSizeRef(*packet) = recordingId.size();
    auto userIdSize = getUserIdSizeRef(*packet);
    size_t packetLength = MIN_START_STOP_SIZE + recordingId.size();
    if (userIdSize > 0)
    {
        packetLength += userIdSize;
        packet->setLength(packetLength);
        // getRecordingIdBuffRef will actually contain the userIdBuff
        std::memmove(getUserIdBuffRef(*packet), getRecordingIdBuffRef(*packet), userIdSize);
    }
    else
    {
        packet->setLength(packetLength);
    }

    std::memcpy(getRecordingIdBuffRef(*packet), recordingId.c_str(), recordingId.size());
    return *this;
}

RecStartStopEventBuilder& RecStartStopEventBuilder::setUserId(const std::string& userId)
{
    auto packet = getPacket();
    if (packet)
    {
        getUserIdSizeRef(*packet) = userId.size();
        packet->setLength(MIN_START_STOP_SIZE + getRecordingIdSizeRef(*packet) + userId.size());
        std::memcpy(getUserIdBuffRef(*packet), userId.c_str(), userId.size());
    }

    return *this;
}
