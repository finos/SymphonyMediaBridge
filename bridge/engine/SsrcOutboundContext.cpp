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

    if (static_cast<int32_t>(sequenceNumber - sequenceNumberStart) > 0 &&
        static_cast<int32_t>(sequenceNumber + offset.sequenceNumber - lastSent.sequenceNumber) > -MAX_REWIND)
    {
        return true;
    }

    return false;
}

} // namespace bridge
