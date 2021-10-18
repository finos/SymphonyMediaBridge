#include "transport/recp/RecStreamAddedEventBuilder.h"
#include "utils/Time.h"

using namespace recp;

namespace
{

RecStreamAddedEvent* castPacket(memory::Packet* packet)
{
    // The ssrc is place right after the header
    return reinterpret_cast<RecStreamAddedEvent*>(packet);
}

uint8_t* getEndpointBuffRef(memory::Packet* packet)
{
    // The endpoint value is placed right after the endpoint len (2 byes)
    return &(packet->get()[REC_HEADER_SIZE + 6 + 2]);
}

} // namespace

RecStreamAddedEventBuilder& RecStreamAddedEventBuilder::setSsrc(uint32_t ssrc)
{
    castPacket(getPacket())->ssrc = ssrc;
    return *this;
}

RecStreamAddedEventBuilder& RecStreamAddedEventBuilder::setIsScreenSharing(bool isScreenSharing)
{
    castPacket(getPacket())->isScreenSharing = isScreenSharing ? 1 : 0;
    return *this;
}

RecStreamAddedEventBuilder& RecStreamAddedEventBuilder::setRtpPayloadType(uint8_t payloadType)
{
    castPacket(getPacket())->rtpPayload = payloadType;
    return *this;
}

RecStreamAddedEventBuilder& RecStreamAddedEventBuilder::setBridgeCodecNumber(uint8_t codecNumber)
{
    castPacket(getPacket())->codec = codecNumber;
    return *this;
}

RecStreamAddedEventBuilder& RecStreamAddedEventBuilder::setEndpoint(const std::string& endpoint)
{
    assert(std::numeric_limits<uint16_t>::max() >= endpoint.size());
    auto packet = getPacket();
    const uint16_t oldSize = castPacket(packet)->endpointLen.get();
    const uint16_t oldEndPos = RecStreamAddedEventBuilder::MinSize + oldSize;
    const uint16_t newEndPos = RecStreamAddedEventBuilder::MinSize + static_cast<uint16_t>(endpoint.size());
    const size_t oldEndPosPlusPadding = (oldEndPos + 3u) & ~3u;
    const size_t newEndPosPlusPadding = (newEndPos + 3u) & ~3u;
    const size_t bytesAfterEndpoint = packet->getLength() - std::min(oldEndPosPlusPadding, packet->getLength());

    // Move the fields after the endpoint size to the right position
    std::memmove(packet->get() + newEndPosPlusPadding, packet->get() + oldEndPosPlusPadding, bytesAfterEndpoint);
    // clean new padding (We will add always a padding to 32 to 32bits even if we don't have bytes after endpoint)
    std::memset(packet->get() + newEndPos, 0, newEndPosPlusPadding - newEndPos);
    packet->setLength(newEndPosPlusPadding + bytesAfterEndpoint);

    castPacket(packet)->endpointLen = static_cast<uint16_t>(endpoint.size());
    std::memcpy(getEndpointBuffRef(packet), endpoint.c_str(), endpoint.size());
    return *this;
}

RecStreamAddedEventBuilder& RecStreamAddedEventBuilder::setWallClock(std::chrono::system_clock::time_point wallClock)
{
    // The wall clock is not part of the RecStreamAddedEventBuilder::MinSize because it is an extension that was
    // not used in the 1st protocol version. To ensure retro compatibility it was added after endpoint string,
    // which has a dynamic size, and the use of this property should be considered optional

    const uint32_t endpointEndPos = RecStreamAddedEventBuilder::MinSize + castPacket(getPacket())->endpointLen;
    // Wall clock position must be 32 bits aligned (we don't need to clean the padding, padding should be inserted and
    // cleaned by setEndpoint() method)
    const uint32_t wallClockPosition = endpointEndPos + 3u & ~3u;
    const nwuint64_t ntpTimestamp(utils::Time::toNtp(wallClock));
    auto packet = getPacket();
    packet->setLength(wallClockPosition + sizeof(nwuint64_t));
    std::memcpy(packet->get() + wallClockPosition, &ntpTimestamp, sizeof(ntpTimestamp));
    return *this;
}
