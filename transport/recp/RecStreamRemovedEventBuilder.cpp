#include "transport/recp/RecStreamRemovedEventBuilder.h"

using namespace recp;

namespace
{

nwuint32_t& getSsrcRef(memory::Packet* packet)
{
    //The ssrc is place right after the header
    return reinterpret_cast<nwuint32_t&>(packet->get()[REC_HEADER_SIZE]);
}

} //namespace <anonymous>

RecStreamRemovedEventBuilder& RecStreamRemovedEventBuilder::setSsrc(uint32_t ssrc)
{
    auto packet = getPacket();
    if (packet)
    {
        getSsrcRef(getPacket()) = ssrc;
    }

    return *this;
}
