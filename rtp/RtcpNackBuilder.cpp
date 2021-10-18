#include "rtp/RtcpNackBuilder.h"
#include "rtp/RtcpFeedback.h"
#include <cassert>

namespace rtp
{

rtp::RtcpNackBuilder::RtcpNackBuilder(const uint32_t reporterSsrc, const uint32_t mediaSsrc)
    : _rtcpFeedback(reinterpret_cast<RtcpFeedback*>(_data.data())),
      _nextFreePid(0)
{
    static_assert(dataSize >= sizeof(RtcpFeedback) + maxPids * 4,
        "_data must be able to contain header + max feedbackControlInfos");

    memset(_data.data(), 0, _data.size());
    memset(_pids.data(), 0, _pids.size() * sizeof(uint16_t));
    memset(_blps.data(), 0, _blps.size() * sizeof(uint16_t));

    _rtcpFeedback->_header.fmtCount = static_cast<uint8_t>(TransportLayerFeedbackType::PacketNack);
    _rtcpFeedback->_header.version = 2;
    _rtcpFeedback->_header.packetType = RTPTRANSPORT_FB;
    _rtcpFeedback->_header.length = 2;
    _rtcpFeedback->_reporterSsrc = reporterSsrc;
    _rtcpFeedback->_mediaSsrc = mediaSsrc;
}

bool RtcpNackBuilder::appendSequenceNumber(const uint16_t sequenceNumber)
{
    for (size_t i = 0; i < _nextFreePid; ++i)
    {
        assert(i < _pids.size());

        if (sequenceNumber == _pids[i])
        {
            return true;
        }

        if (sequenceNumber > _pids[i] && sequenceNumber <= _pids[i] + 16)
        {
            assert(i < _blps.size());

            const auto shiftAmount = (sequenceNumber - 1) - _pids[i];
            _blps[i] = _blps[i] | (0x1 << shiftAmount);
            return true;
        }
    }

    if (_nextFreePid >= maxPids)
    {
        return false;
    }

    _pids[_nextFreePid] = sequenceNumber;
    ++_nextFreePid;
    assert(_nextFreePid <= _pids.size());

    return true;
}

const uint8_t* RtcpNackBuilder::build(size_t& outSize)
{
    auto offset = sizeof(RtcpFeedback);
    auto feedbackControlInfo = _data.data() + offset;
    auto headerLengthField = _rtcpFeedback->_header.length.get();

    for (size_t i = 0; i < _nextFreePid; ++i)
    {
        const auto pid = _pids[i];
        const auto blp = _blps[i];
        feedbackControlInfo[0] = pid >> 8;
        feedbackControlInfo[1] = pid & 0xFF;
        feedbackControlInfo[2] = blp >> 8;
        feedbackControlInfo[3] = blp & 0xFF;

        ++headerLengthField;
        offset += 4;
        feedbackControlInfo = _data.data() + offset;
    }

    _rtcpFeedback->_header.length = headerLengthField;
    outSize = offset;
    return _data.data();
}

} // namespace rtp
