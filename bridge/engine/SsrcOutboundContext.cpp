#include "bridge/engine/SsrcOutboundContext.h"

namespace bridge
{

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

    // for video the packet rate can be higher at key frames, allow more rewind
    const int32_t maxRewind = rtpMap.isVideo() ? 1024 * 3 : 512;

    const bool packetArrivedAfterSsrcSwitch = (static_cast<int32_t>(sequenceNumber - rewrite.sequenceNumberStart) > 0);
    const bool packetNotTooOld = (static_cast<int32_t>(sequenceNumber + rewrite.offset.sequenceNumber -
                                      rewrite.lastSent.sequenceNumber) > -maxRewind);

    if (packetArrivedAfterSsrcSwitch && packetNotTooOld)
    {
        return true;
    }

    return false;
}

} // namespace bridge
