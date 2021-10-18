#include "utils/Time.h"
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace bridge
{

class PliScheduler
{
public:
    PliScheduler() : _keyFrameNeeded(false), _pliSendTime(0) {}

    void onPliSent(const uint64_t timestamp) { _pliSendTime = timestamp; }

    void onKeyFrameReceived()
    {
        _keyFrameNeeded = false;
        _pliSendTime = 0;
    }

    inline bool shouldSendPli(const uint64_t timestamp, const uint32_t rttMs) const
    {
        if (!_keyFrameNeeded.load())
        {
            return false;
        }

        // Since PLI triggers a key frame and those are typically 4-5x average bitrate, many packets, paced at sender
        // etc., I think adding a delay of 4 * (1000/30) ~= 130 ms is justified, assuming that link operates at
        // capacity. If we have access to link utilisation percentage, it could be scaled by that.
        const uint64_t delay = static_cast<uint64_t>(rttMs + 10 + 130) * utils::Time::ms;
        return _pliSendTime == 0 || utils::Time::diffGE(_pliSendTime, timestamp, delay);
    }

    void triggerPli() { _keyFrameNeeded = true; }

private:
    std::atomic_bool _keyFrameNeeded;
    uint64_t _pliSendTime;
};

} // namespace bridge
