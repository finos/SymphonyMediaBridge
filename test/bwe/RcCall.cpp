#include "RcCall.h"
#include "FakeAudioSource.h"
#include "FakeMedia.h"
#include "bwe/RateController.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "test/transport/NetworkLink.h"

namespace fakenet
{

RcCall::RcCall(memory::PacketPoolAllocator& allocator,
    config::Config& config,
    bwe::RateController& estimator,
    NetworkLink* upLink,
    NetworkLink* downLink,
    bool audio,
    uint64_t duration)
    : _bwe(estimator),
      _allocator(allocator),
      _globalConfig(config),
      _mixVideoSendState(90000, _globalConfig),
      _timeCursor(),
      _endTime(_timeCursor + duration),
      _timeStart(1000),
      _wallClockStartNtp(0x000000000)
{
    _timeCursor = _timeStart;

    _upLink.reset(upLink);
    _downLink.reset(downLink);
    if (audio)
    {
        addChannel(new fakenet::FakeAudioSource(_allocator, 90, 1), 48000);
    }
}

RcCall::~RcCall()
{
    while (!_upLink->empty() || !_downLink->empty())
    {
        _allocator.free(_upLink->pop());
        _allocator.free(_downLink->pop());
    }
}

void RcCall::push(std::unique_ptr<fakenet::NetworkLink>& link, memory::Packet* packet)
{
    if (!link->push(packet, _timeCursor))
    {
        _allocator.free(packet);
    }
}

void RcCall::setDownlink(NetworkLink* link)
{
    _downLink.reset(link);
}

void RcCall::addChannel(fakenet::MediaSource* source, uint32_t rtpFrequency)
{
    _channels.emplace(std::piecewise_construct,
        std::forward_as_tuple(source->getSsrc()),
        std::forward_as_tuple(source, rtpFrequency, _timeCursor - utils::Time::sec * 4, _globalConfig));

    if (rtpFrequency == 90000)
    {
        _rtxProbeSsrc.set(source->getSsrc() + 17000);
    }
}

void RcCall::sendRtpPadding(uint32_t count, uint32_t ssrc, uint16_t paddingSize)
{
    for (; count > 0; count--)
    {
        auto padPacket = memory::makePacket(_allocator);
        if (padPacket)
        {
            padPacket->clear();
            auto padRtpHeader = rtp::RtpHeader::create(*padPacket);
            padPacket->setLength(paddingSize);
            padRtpHeader->ssrc = ssrc;
            padRtpHeader->payloadType = 96;
            padRtpHeader->sequenceNumber = (_mixVideoSendState.getSentSequenceNumber() + 1) & 0xFFFF;
            // fake a really old packet
            reinterpret_cast<uint16_t*>(padRtpHeader->getPayload())[0] =
                hton<uint16_t>(_mixVideoSendState.getSentSequenceNumber() - 10000);

            _bwe.onRtpSent(_timeCursor, padRtpHeader->ssrc, padRtpHeader->sequenceNumber, padPacket->getLength());

            _mixVideoSendState.onRtpSent(_timeCursor, *padPacket);
            push(_upLink, padPacket);
        }
    }
}

void RcCall::sendRtcpPadding(uint32_t count, uint32_t ssrc, uint16_t paddingSize)
{
    for (; count > 0; --count)
    {
        auto padPacket = memory::makePacket(_allocator);
        auto* rtcpPadding = rtp::RtcpApplicationSpecific::create(padPacket->get(), ssrc, "BRPP", paddingSize);
        padPacket->setLength(rtcpPadding->header.size());
        _bwe.onRtcpPaddingSent(_timeCursor, ssrc, padPacket->getLength());
        push(_upLink, padPacket);
    }
}

void RcCall::sendSR(uint32_t ssrc, transport::RtpSenderState& sendState, uint64_t wallClock)
{
    auto rtcpPacket = memory::makePacket(_allocator);
    if (rtcpPacket)
    {
        auto* report = rtp::RtcpSenderReport::create(rtcpPacket->get());
        report->ssrc = ssrc;
        sendState.fillInReport(*report, _timeCursor, wallClock);
        rtcpPacket->setLength(report->header.size());
        _bwe.onSenderReportSent(_timeCursor,
            ssrc,
            (report->ntpSeconds.get() << 16) | (report->ntpFractions.get() >> 16),
            rtcpPacket->getLength());
        sendState.onRtcpSent(_timeCursor, &report->header);
        push(_upLink, rtcpPacket);

        uint16_t paddingSize = 0;
        auto count = _bwe.getPadding(_timeCursor, rtcpPacket->getLength(), paddingSize);
        if (_rtxProbeSsrc.isSet())
        {
            sendRtpPadding(count, _rtxProbeSsrc.get(), paddingSize);
        }
        else
        {
            sendRtcpPadding(count, ssrc, paddingSize);
        }
    }
}

void RcCall::transmitChannel(Channel& channel, uint64_t srClockOffset)
{

    for (auto* packet = channel.source->getPacket(_timeCursor); packet; packet = channel.source->getPacket(_timeCursor))
    {
        const auto* rtpHeader = rtp::RtpHeader::fromPacket(*packet);
        const uint32_t ssrc = rtpHeader->ssrc;
        uint16_t paddingSize = 0;
        auto count = _bwe.getPadding(_timeCursor, packet->getLength(), paddingSize);

        if (_rtxProbeSsrc.isSet())
        {
            sendRtpPadding(count, _rtxProbeSsrc.get(), paddingSize);
        }
        else
        {
            sendRtcpPadding(count, ssrc, paddingSize);
        }
        push(_upLink, packet);

        channel.sendState.onRtpSent(_timeCursor, *packet);
        _bwe.onRtpSent(_timeCursor, rtpHeader->ssrc, rtpHeader->sequenceNumber, packet->getLength());

        if (channel.sendState.timeToSenderReport(_timeCursor) == 0)
        {
            channel.lastSenderReport = _timeCursor;
            sendSR(channel.source->getSsrc(), channel.sendState, getWallClock() + srClockOffset);
        }
    }
}

void RcCall::processReceiverSide()
{
    for (auto* packet = _upLink->pop(_timeCursor); packet; packet = _upLink->pop(_timeCursor))
    {
        if (rtp::isRtpPacket(*packet))
        {
            const auto* rtpHeader = rtp::RtpHeader::fromPacket(*packet);
            /*logger::debug("%" PRIu64 " received  %u, seq %u, size %zu",
                "",
                _timeCursor / utils::Time::ms,
                rtpHeader->ssrc.get(),
                rtpHeader->sequenceNumber.get(),
                packet->getLength());*/
            const uint32_t ssrc = rtpHeader->ssrc;
            auto itPair = _recvStates.find(ssrc);
            if (itPair == _recvStates.end())
            {
                auto addedPair = _recvStates.emplace(std::piecewise_construct,
                    std::forward_as_tuple(ssrc),
                    std::forward_as_tuple(_globalConfig));
                itPair = addedPair.first;
            }
            if (itPair != _recvStates.end())
            {
                auto& recvState = itPair->second;
                recvState.onRtpReceived(*packet, _timeCursor);
            }
        }
        else
        {
            const auto* rtcpReport = rtp::RtcpReport::fromPacket(*packet);
            const uint32_t ssrc = rtcpReport->ssrc;
            auto itPair = _recvStates.find(ssrc);
            if (itPair != _recvStates.end() && rtcpReport->header.packetType == rtp::RtcpPacketType::SENDER_REPORT)
            {
                logger::debug("%" PRIu64 "ms received SR %u",
                    "",
                    (_timeCursor & 0xFFFFFFFFFF) / utils::Time::ms,
                    rtcpReport->ssrc.get());
                auto& recvState = itPair->second;
                recvState.onRtcpReceived(rtcpReport->header, _timeCursor, getWallClock());
            }
            else
            {
                /*logger::debug("%" PRIu64 "ms received RTCP %u, size %zu",
                    "",
                    _timeCursor / utils::Time::ms,
                    rtcpReport->ssrc.get(),
                    packet->getLength());*/
            }
        }

        _allocator.free(packet);
    }

    int64_t remainingTimeToRR = std::numeric_limits<int64_t>::max();
    for (auto& itPair : _recvStates)
    {
        remainingTimeToRR = std::min(remainingTimeToRR, itPair.second.timeToReceiveReport(_timeCursor));
    }

    if (remainingTimeToRR == 0)
    {
        auto rtcpPacket = memory::makePacket(_allocator);
        if (rtcpPacket)
        {
            auto* report = rtp::RtcpReceiverReport::create(rtcpPacket->get());
            report->ssrc = 5050;
            int index = 0;
            for (auto& itPair : _recvStates)
            {
                if (itPair.second.timeToReceiveReport(_timeCursor) > 0)
                {
                    continue;
                }
                auto& block = report->reportBlocks[index];
                block.ssrc = itPair.first;
                itPair.second.fillInReportBlock(_timeCursor, block, getWallClock());
                report->header.fmtCount = report->header.fmtCount + 1;
                ++index;
            }
            rtcpPacket->setLength(report->header.size());
            push(_downLink, rtcpPacket);
        }
    }
}

void RcCall::processReceiverReports()
{
    const auto wallclockNtp32 = utils::Time::toNtp32(getWallClock());
    for (auto* packet = _downLink->pop(_timeCursor); packet; packet = _downLink->pop(_timeCursor))
    {
        if (rtp::isRtcpPacket(*packet))
        {
            const auto* rtcpReport = rtp::RtcpReport::fromPacket(*packet);
            if (rtcpReport->header.packetType == rtp::RtcpPacketType::RECEIVER_REPORT)
            {
                const auto* rr = rtp::RtcpReceiverReport::fromPtr(packet->get(), packet->getLength());
                uint32_t rttNtp = ~0;
                for (int i = 0; rr && i < rr->header.fmtCount; ++i)
                {
                    const auto& block = rr->reportBlocks[i];
                    const uint32_t ssrc = block.ssrc;
                    auto it = _channels.find(ssrc);
                    if (it != _channels.end())
                    {
                        it->second.sendState.onReceiverBlockReceived(_timeCursor, wallclockNtp32, block);
                        rttNtp = std::min(rttNtp, it->second.sendState.getRttNtp());
                    }
                    else if (_rtxProbeSsrc.isSet() && block.ssrc.get() == _rtxProbeSsrc.get())
                    {
                        _mixVideoSendState.onReceiverBlockReceived(_timeCursor, wallclockNtp32, block);
                        rttNtp = std::min(rttNtp, _mixVideoSendState.getRttNtp());
                    }

                    _bwe.onReportBlockReceived(block.ssrc,
                        block.extendedSeqNoReceived,
                        block.loss.getCumulativeLoss(),
                        block.lastSR,
                        block.delaySinceLastSR);
                }
                if (rttNtp != ~0u)
                {
                    _rtt = rttNtp;
                }

                if (rr->header.fmtCount > 0)
                {
                    _bwe.onReportReceived(_timeCursor, rr->header.fmtCount, rr->reportBlocks, rttNtp);
                }
            }
        }
        _allocator.free(packet);
    }
}

bool RcCall::run(uint64_t period, uint32_t expectedCeilingKbps)
{
    uint64_t prevLog = _timeCursor;
    const uint64_t endTime = _timeCursor + period;

    while (_timeCursor < endTime && _timeCursor < _endTime)
    {
        uint64_t srClockOffset = 0;
        for (auto& channelIt : _channels)
        {
            transmitChannel(channelIt.second, srClockOffset);
            srClockOffset += 0x10000; // 1/65536 s to avoid SR having same ntp transmit time
        }
        if (_rtxProbeSsrc.isSet() && _mixVideoSendState.timeToSenderReport(_timeCursor) == 0)
        {
            sendSR(_rtxProbeSsrc.get(), _mixVideoSendState, getWallClock() + srClockOffset);
        }

        processReceiverSide();
        processReceiverReports();

        int64_t advTime = utils::Time::sec * 15;
        for (auto& it : _channels)
        {
            advTime = std::min(advTime, it.second.source->timeToRelease(_timeCursor));
        }
        advTime = std::min(advTime, _downLink->timeToRelease(_timeCursor));
        advTime = std::min(advTime, _upLink->timeToRelease(_timeCursor));
        assert(advTime > 0);
        _timeCursor += advTime;

        if (utils::Time::diffGE(prevLog, _timeCursor, utils::Time::sec))
        {
            prevLog = _timeCursor;
            logger::debug("%" PRIu64 "ms link bit rate %.fkbsp, estimate %.1fkbps, rtt %ums",
                "",
                (_timeCursor % utils::Time::minute) / utils::Time::ms,
                _upLink->getBitRateKbps(_timeCursor),
                _bwe.getTargetRate(),
                _rtt * 1000 / 0x10000);
        }
        if (_bwe.getTargetRate() > expectedCeilingKbps)
        {
            return true;
        }
    }

    return _timeCursor < _endTime;
}
} // namespace fakenet
