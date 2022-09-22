#include "bridge/engine/SsrcOutboundContext.h"

namespace bridge
{
const int32_t MAX_REWIND = 512;

bool SsrcOutboundContext::shouldSend(uint32_t ssrc, uint32_t sequenceNumber) const
{
    if (rewrite.empty())
    {
        return true;
    }

    if (ssrc != originalSsrc)
    {
        return true;
    }

    const bool packetArrivedAfterSsrcSwitch = (static_cast<int32_t>(sequenceNumber - rewrite.sequenceNumberStart) > 0);
    const bool packetNotTooOld = (static_cast<int32_t>(sequenceNumber + rewrite.offset.sequenceNumber -
                                      rewrite.lastSent.sequenceNumber) > -MAX_REWIND);

    if (packetArrivedAfterSsrcSwitch && packetNotTooOld)
    {
        return true;
    }

    return false;
}

} // namespace bridge
