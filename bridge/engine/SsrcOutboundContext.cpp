#include "bridge/engine/SsrcOutboundContext.h"

namespace bridge
{
const int32_t MAX_REWIND = 512;

bool SsrcOutboundContext::SsrcRewrite::shouldSend(uint32_t ssrc, uint32_t sequenceNumber) const
{
    if (empty())
    {
        return true;
    }

    if (ssrc != originalSsrc)
    {
        return true;
    }

    const bool packetArrivedAfterSsrcSwitch = (static_cast<int32_t>(sequenceNumber - sequenceNumberStart) > 0);
    const bool packetNotTooOld =
        (static_cast<int32_t>(sequenceNumber + offset.sequenceNumber - lastSent.sequenceNumber) > -MAX_REWIND);

    if (packetArrivedAfterSsrcSwitch && packetNotTooOld)
    {
        return true;
    }

    return false;
}

} // namespace bridge
