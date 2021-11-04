#include "bwe/RateController.h"
#include "config/Config.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "test/bwe/FakeAudioSource.h"
#include "test/bwe/FakeVideoSource.h"
#include "test/transport/FakeNetwork.h"
#include "transport/RtpReceiveState.h"
#include "transport/RtpSenderState.h"
#include "transport/ice/IceSession.h"
#include "transport/sctp/SctpConfig.h"
#include <gtest/gtest.h>
#include <vector>

class RateControllerTest : public ::testing::Test
{
    struct Channel
    {
        Channel(fakenet::MediaSource* mediaSource,
            uint32_t rtpFrequency,
            uint64_t lastReportTimestamp,
            config::Config& config)
            : lastSentSequenceNumber(0),
              rtxSequenceCounter(110),
              source(mediaSource),
              sendState(rtpFrequency, config),
              rtxSendState(rtpFrequency, config),
              lastSenderReport(lastReportTimestamp)
        {
        }

        ~Channel() { delete source; }

        uint32_t lastSentSequenceNumber;
        uint32_t rtxSequenceCounter;
        fakenet::MediaSource* source;
        transport::RtpSenderState sendState;
        transport::RtpSenderState rtxSendState;
        uint64_t lastSenderReport;
    };

public:
    RateControllerTest()
        : _allocator(4096 * 32, "ratemain"),
          _mixVideoSendState(90000, _globalConfig),
          _timeStart(1000),
          _wallClockStartNtp(0x000000000)
    {
        _timeCursor = _timeStart;
        _config.enabled = true;
    }

    void SetUp() override
    {
        _upLink = std::make_unique<fakenet::NetworkLink>(1500, 128000, 1480);
        _downLink = std::make_unique<fakenet::NetworkLink>(8500, 128000, 1480);
        _timeCursor = _timeStart;
        _rateControl = std::make_unique<bwe::RateController>(1, _config);
    }

    void TearDown() override
    {
        while (!_upLink->empty() || !_downLink->empty())
        {
            _allocator.free(_upLink->pop());
            _allocator.free(_downLink->pop());
        }
        _upLink.reset();
        _downLink.reset();
    }

    uint64_t getWallClock()
    {
        return _wallClockStartNtp + ((_timeCursor - _timeStart) * 0x20000000ull / (125 * utils::Time::ms));
    }

    uint64_t _timeCursor;

    std::unordered_map<uint32_t, Channel> _channels;
    std::unordered_map<uint32_t, transport::RtpReceiveState> _recvStates;

    std::unique_ptr<fakenet::NetworkLink> _upLink;
    std::unique_ptr<fakenet::NetworkLink> _downLink;
    memory::PacketPoolAllocator _allocator;
    std::unique_ptr<bwe::RateController> _rateControl;
    bwe::RateControllerConfig _config;
    config::Config _globalConfig;
    utils::Optional<uint32_t> _rtxProbeSsrc;
    transport::RtpSenderState _mixVideoSendState;

    void push(std::unique_ptr<fakenet::NetworkLink>& link, memory::Packet* packet)
    {
        if (!link->push(packet, _timeCursor))
        {
            _allocator.free(packet);
        }
    }

    void addChannel(fakenet::MediaSource* source, uint32_t rtpFrequency, uint64_t lastReportTimestamp)
    {
        _channels.emplace(std::piecewise_construct,
            std::forward_as_tuple(source->getSsrc()),
            std::forward_as_tuple(source, rtpFrequency, lastReportTimestamp, _globalConfig));

        if (rtpFrequency == 90000)
        {
            _rtxProbeSsrc.set(source->getSsrc() + 17000);
        }
    }

    void sendRtpPadding(uint32_t count, uint32_t ssrc, uint16_t paddingSize)
    {
        for (; count > 0; count--)
        {
            auto padPacket = memory::makePacket(_allocator);
            if (padPacket)
            {
                std::memset(padPacket->get(), 0, memory::Packet::size);
                padPacket->setLength(paddingSize);
                auto padRtpHeader = rtp::RtpHeader::create(padPacket->get(), memory::Packet::size);
                padRtpHeader->ssrc = ssrc;
                padRtpHeader->payloadType = 96;
                padRtpHeader->padding = 0;
                padRtpHeader->marker = 0;
                padRtpHeader->timestamp = 0;
                padRtpHeader->sequenceNumber = (_mixVideoSendState.getSentSequenceNumber() + 1) & 0xFFFF;
                // fake a really old packet
                reinterpret_cast<uint16_t*>(padRtpHeader->getPayload())[0] =
                    hton<uint16_t>(_mixVideoSendState.getSentSequenceNumber() - 10000);

                _rateControl->onRtpPaddingSent(_timeCursor,
                    padRtpHeader->ssrc,
                    padRtpHeader->sequenceNumber,
                    padPacket->getLength());

                _mixVideoSendState.onRtpSent(_timeCursor, *padPacket);
                push(_upLink, padPacket);
            }
        }
    }

    void sendRtcpPadding(uint32_t count, uint32_t ssrc, uint16_t paddingSize)
    {
        for (; count > 0; --count)
        {
            auto padPacket = memory::makePacket(_allocator);
            auto* rtcpPadding = rtp::RtcpApplicationSpecific::create(padPacket->get(), ssrc, "BRPP", paddingSize);
            padPacket->setLength(rtcpPadding->header.size());
            _rateControl->onRtcpPaddingSent(_timeCursor, ssrc, padPacket->getLength());
            push(_upLink, padPacket);
        }
    }

    void sendSR(uint32_t ssrc, transport::RtpSenderState& sendState, uint64_t wallClock)
    {
        auto rtcpPacket = memory::makePacket(_allocator);
        if (rtcpPacket)
        {
            auto* report = rtp::RtcpSenderReport::create(rtcpPacket->get());
            report->ssrc = ssrc;
            sendState.fillInReport(*report, _timeCursor, wallClock);
            rtcpPacket->setLength(report->header.size());
            _rateControl->onSenderReportSent(_timeCursor,
                ssrc,
                (report->ntpSeconds.get() << 16) | (report->ntpFractions.get() >> 16),
                rtcpPacket->getLength());
            sendState.onRtcpSent(_timeCursor, &report->header);
            push(_upLink, rtcpPacket);

            uint16_t paddingSize = 0;
            auto count = _rateControl->getPadding(_timeCursor, rtcpPacket->getLength(), paddingSize);
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

    void transmitChannel(Channel& channel, uint64_t srClockOffset)
    {

        for (auto* packet = channel.source->getPacket(_timeCursor); packet;
             packet = channel.source->getPacket(_timeCursor))
        {
            const auto* rtpHeader = rtp::RtpHeader::fromPacket(*packet);
            const uint32_t ssrc = rtpHeader->ssrc;
            uint16_t paddingSize = 0;
            auto count = _rateControl->getPadding(_timeCursor, packet->getLength(), paddingSize);

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
            _rateControl->onRtpSent(_timeCursor, rtpHeader->ssrc, rtpHeader->sequenceNumber, packet->getLength());

            if (channel.sendState.timeToSenderReport(_timeCursor) == 0)
            {
                channel.lastSenderReport = _timeCursor;
                sendSR(channel.source->getSsrc(), channel.sendState, getWallClock() + srClockOffset);
            }
        }
    }

    void processReceiverSide()
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

    void processReceiverReports()
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

                        _rateControl->onReportBlockReceived(block.ssrc,
                            block.extendedSeqNoReceived,
                            block.loss.getCumulativeLoss(),
                            block.lastSR);
                    }
                    if (rttNtp != ~0u)
                    {
                        _rtt = rttNtp;
                    }

                    if (rr->header.fmtCount > 0)
                    {
                        _rateControl->onReportReceived(_timeCursor, rr->header.fmtCount, rr->reportBlocks, rttNtp);
                    }
                }
            }
            _allocator.free(packet);
        }
    }

    void run(uint64_t period, uint32_t expectedCeilingKbps)
    {
        uint64_t prevLog = _timeCursor;
        const uint64_t endTime = _timeCursor + period;

        while (_timeCursor < endTime)
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
            EXPECT_LT(_rateControl->getTargetRate(), expectedCeilingKbps);

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
                    _rateControl->getTargetRate(),
                    _rtt * 1000 / 0x10000);
            }
        }
    }

private:
    const uint64_t _timeStart;
    const uint64_t _wallClockStartNtp;
    uint32_t _rtt;
};

TEST_F(RateControllerTest, plain1Mbps)
{
    _upLink.reset(new fakenet::NetworkLink(1000, 75000, 1480));
    _downLink.reset(new fakenet::NetworkLink(6500, 75000, 1480));

    addChannel(new fakenet::FakeAudioSource(_allocator, 120, 1), 48000, _timeCursor - utils::Time::sec * 4);
    auto videoChannel = new fakenet::FakeVideoSource(_allocator, 0, 10);
    addChannel(videoChannel, 90000, _timeCursor - utils::Time::sec * 4);
    videoChannel->setBandwidth(400);
    run(utils::Time::sec * 7, 1050);

    EXPECT_GE(_rateControl->getTargetRate(), 700);

    run(utils::Time::sec * 15, 1050);

    EXPECT_GE(_rateControl->getTargetRate(), 900);
}

TEST_F(RateControllerTest, long1Mbps)
{
    _upLink.reset(new fakenet::NetworkLink(1000, 75000, 1480));
    _upLink->setStaticDelay(185);
    _downLink.reset(new fakenet::NetworkLink(6500, 75000, 1480));
    _downLink->setStaticDelay(85);

    addChannel(new fakenet::FakeAudioSource(_allocator, 120, 1), 48000, _timeCursor - utils::Time::sec * 4);
    auto videoChannel = new fakenet::FakeVideoSource(_allocator, 0, 10);
    addChannel(videoChannel, 90000, _timeCursor - utils::Time::sec * 4);
    videoChannel->setBandwidth(250);
    run(utils::Time::sec * 9, 1050);

    EXPECT_GE(_rateControl->getTargetRate(), 700);
    videoChannel->setBandwidth(600);
    run(utils::Time::sec * 15, 1050);

    EXPECT_GE(_rateControl->getTargetRate(), 900);
}

TEST_F(RateControllerTest, plain300kbps)
{
    _upLink.reset(new fakenet::NetworkLink(300, 75000, 1480));
    _downLink.reset(new fakenet::NetworkLink(6500, 75000, 1480));

    addChannel(new fakenet::FakeAudioSource(_allocator, 120, 1), 48000, _timeCursor - utils::Time::sec * 4);

    run(utils::Time::sec * 5, 310);

    EXPECT_GE(_rateControl->getTargetRate(), 200);

    run(utils::Time::sec * 15, 310);

    EXPECT_GE(_rateControl->getTargetRate(), 250);
}

TEST_F(RateControllerTest, long300kbps)
{
    _upLink.reset(new fakenet::NetworkLink(300, 75000, 1480));
    _downLink.reset(new fakenet::NetworkLink(6500, 75000, 1480));
    _upLink->setStaticDelay(90);
    _downLink->setStaticDelay(85);

    addChannel(new fakenet::FakeAudioSource(_allocator, 120, 1), 48000, _timeCursor - utils::Time::sec * 4);

    run(utils::Time::sec * 5, 310);

    EXPECT_GE(_rateControl->getTargetRate(), 200);

    run(utils::Time::sec * 15, 310);

    EXPECT_GE(_rateControl->getTargetRate(), 250);
}

TEST_F(RateControllerTest, plain500kbps)
{
    _upLink.reset(new fakenet::NetworkLink(500, 75000, 1480));
    _downLink.reset(new fakenet::NetworkLink(6500, 75000, 1480));

    addChannel(new fakenet::FakeAudioSource(_allocator, 120, 1), 48000, _timeCursor - utils::Time::sec * 4);

    run(utils::Time::sec * 5, 550);

    EXPECT_GE(_rateControl->getTargetRate(), 450);

    run(utils::Time::sec * 15, 550);

    EXPECT_GE(_rateControl->getTargetRate(), 470);
}

TEST_F(RateControllerTest, long500kbps)
{
    _upLink.reset(new fakenet::NetworkLink(500, 75000, 1480));
    _downLink.reset(new fakenet::NetworkLink(6500, 75000, 1480));
    _upLink->setStaticDelay(90);
    _downLink->setStaticDelay(85);

    addChannel(new fakenet::FakeAudioSource(_allocator, 120, 1), 48000, _timeCursor - utils::Time::sec * 4);

    run(utils::Time::sec * 5, 550);

    EXPECT_GE(_rateControl->getTargetRate(), 450);

    run(utils::Time::sec * 15, 550);

    EXPECT_GE(_rateControl->getTargetRate(), 470);
}

TEST_F(RateControllerTest, plain4000kbps)
{
    _upLink.reset(new fakenet::NetworkLink(4000, 85000, 1480));
    _downLink.reset(new fakenet::NetworkLink(10500, 85000, 1480));

    addChannel(new fakenet::FakeAudioSource(_allocator, 120, 1), 48000, _timeCursor - utils::Time::sec * 4);
    auto videoChannel = new fakenet::FakeVideoSource(_allocator, 0, 10);
    addChannel(videoChannel, 90000, _timeCursor);

    run(utils::Time::sec * 10, 4100);

    videoChannel->setBandwidth(_rateControl->getTargetRate() - 200);
    EXPECT_GE(_rateControl->getTargetRate(), 1000);

    run(utils::Time::sec * 25, 4100);

    EXPECT_GE(_rateControl->getTargetRate(), 3800);
}

TEST_F(RateControllerTest, long4000kbps)
{
    _upLink.reset(new fakenet::NetworkLink(4000, 85000, 1480));
    _downLink.reset(new fakenet::NetworkLink(10500, 85000, 1480));
    _upLink->setStaticDelay(70); // half way around globe
    _downLink->setStaticDelay(70);

    addChannel(new fakenet::FakeAudioSource(_allocator, 120, 1), 48000, _timeCursor - utils::Time::sec * 4);
    auto videoChannel = new fakenet::FakeVideoSource(_allocator, 0, 10);
    addChannel(videoChannel, 90000, _timeCursor);

    run(utils::Time::sec * 10, 4100);

    videoChannel->setBandwidth(_rateControl->getTargetRate() - 200);
    EXPECT_GE(_rateControl->getTargetRate(), 1200);

    run(utils::Time::sec * 25, 4100);

    EXPECT_GE(_rateControl->getTargetRate(), 3500);
}
