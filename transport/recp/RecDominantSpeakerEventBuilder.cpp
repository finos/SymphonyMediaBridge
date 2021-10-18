#include "transport/recp/RecDominantSpeakerEventBuilder.h"

using namespace recp;

namespace
{
nwuint16_t& getEndpointLenRef(memory::Packet* packet)
{
    return reinterpret_cast<nwuint16_t&>(packet->get()[REC_HEADER_SIZE]);
}

uint8_t* getEndpointBuffRef(memory::Packet* packet)
{
    // The endpoint value is placed right after the endpoint len (2 byes)
    return &(packet->get()[REC_HEADER_SIZE + 2]);
}

} // namespace

RecDominantSpeakerEventBuilder& RecDominantSpeakerEventBuilder::setDominantSpeakerEndpoint(const std::string& endpoint)
{
    assert(std::numeric_limits<uint16_t>::max() >= endpoint.size());
    auto packet = getPacket();
    packet->setLength(MIN_DOMINANT_SPEAKER_SIZE + endpoint.size());
    getEndpointLenRef(packet) = static_cast<uint16_t>(endpoint.size());
    std::memcpy(getEndpointBuffRef(packet), endpoint.c_str(), endpoint.size());
    return *this;
}
