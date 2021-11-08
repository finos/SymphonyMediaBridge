#pragma once

#include "bwe/RateController.h"
#import "config/Config.h"
#include "memory/PacketPoolAllocator.h"
#include "test/bwe/FakeMedia.h"
#include "transport/RtpReceiveState.h"
#include "transport/RtpSenderState.h"
#include <unordered_map>
#include <vector>

namespace fakenet
{
class NetworkLink;
class MediaSource;

class RcCall
{
    struct Channel
    {
        Channel(MediaSource* mediaSource, uint32_t rtpFrequency, uint64_t lastReportTimestamp, config::Config& config)
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
        MediaSource* source;
        transport::RtpSenderState sendState;
        transport::RtpSenderState rtxSendState;
        uint64_t lastSenderReport;
    };

public:
    RcCall(memory::PacketPoolAllocator& allocator,
        config::Config& config,
        bwe::RateController& estimator,
        NetworkLink* upLink,
        NetworkLink* downLink,
        bool audio,
        uint64_t duration);

    ~RcCall();

    void addLink(NetworkLink* link);
    void addSource(MediaSource* source);

    bool run(uint64_t period, uint32_t expectedCeilingKbps);
    uint64_t getTime() { return _timeCursor; }

    double getEstimate() const;
    bwe::RateController& _bwe;

    uint64_t getWallClock()
    {
        return _wallClockStartNtp + ((_timeCursor - _timeStart) * 0x20000000ull / (125 * utils::Time::ms));
    }
    void addChannel(fakenet::MediaSource* source, uint32_t rtpFrequency);

    void setDownlink(NetworkLink* link);

private:
    void push(std::unique_ptr<NetworkLink>& link, memory::Packet* packet);
    void sendRtpPadding(uint32_t count, uint32_t ssrc, uint16_t paddingSize);
    void sendRtcpPadding(uint32_t count, uint32_t ssrc, uint16_t paddingSize);
    void sendSR(uint32_t ssrc, transport::RtpSenderState& sendState, uint64_t wallClock);
    void processReceiverSide();
    void transmitChannel(Channel& channel, uint64_t srClockOffset);
    void processReceiverReports();

    memory::PacketPoolAllocator& _allocator;
    std::unordered_map<uint32_t, Channel> _channels;
    std::unordered_map<uint32_t, transport::RtpReceiveState> _recvStates;

    std::unique_ptr<NetworkLink> _upLink;
    std::unique_ptr<NetworkLink> _downLink;
    config::Config& _globalConfig;
    utils::Optional<uint32_t> _rtxProbeSsrc;
    transport::RtpSenderState _mixVideoSendState;

    uint64_t _timeCursor;
    uint64_t _endTime;

    const uint64_t _timeStart;
    const uint64_t _wallClockStartNtp;
    uint32_t _rtt;
};
} // namespace fakenet
