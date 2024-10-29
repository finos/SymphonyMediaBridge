#pragma once

#include "AudioSource.h"
#include "FakeVideoDecoder.h"
#include "api/SimulcastGroup.h"
#include "bridge/RtpMap.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "bridge/engine/VideoMissingPacketsTracker.h"
#include "codec/Opus.h"
#include "codec/Vp8Header.h"
#include "legacyapi/DataChannelMessage.h"
#include "logger/PruneSpam.h"
#include "memory/Array.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtcpFeedback.h"
#include "rtp/RtcpHeader.h"
#include "test/integration/emulator/ApiChannel.h"
#include "test/integration/emulator/CallConfigBuilder.h"
#include "test/transport/FakeNetwork.h"
#include "transport/DataReceiver.h"
#include "transport/RtcTransport.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SslDtls.h"
#include "utils/IdGenerator.h"
#include "utils/Pacer.h"
#include "utils/Span.h"
#include "utils/StringBuilder.h"
#include "webrtc/WebRtcDataStream.h"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace emulator
{

class MediaSendJob : public jobmanager::Job
{
public:
    MediaSendJob(transport::Transport& transport, memory::UniquePacket packet, uint64_t timestamp)
        : _transport(transport),
          _packet(std::move(packet))
    {
    }

    void run() override { _transport.protectAndSend(std::move(_packet)); }

private:
    transport::Transport& _transport;
    memory::UniquePacket _packet;
};

struct RtxStats
{
    struct Receiver
    {
        size_t packetsMissing = 0;
        size_t packetsRecovered = 0;
        size_t nackRequests = 0; // Not implemented: SfuClient does not sent NACK yet.

        Receiver& operator+=(const Receiver& other)
        {
            packetsMissing += other.packetsMissing;
            packetsRecovered += other.packetsRecovered;
            nackRequests += other.nackRequests;
            return *this;
        }
    } receiver;

    struct Sender
    {
        size_t nacksReceived = 0;
        size_t retransmissionRequests = 0;
        size_t retransmissions = 0;
        size_t packetsSent = 0;

        Sender& operator+=(const Sender& other)
        {
            nacksReceived += other.nacksReceived;
            retransmissionRequests += other.retransmissionRequests;
            retransmissions += other.retransmissions;
            packetsSent += other.packetsSent;
            return *this;
        }
    } sender;

    RtxStats& operator+=(const RtxStats& other)
    {
        receiver += other.receiver;
        sender += other.sender;

        return *this;
    }

    RtxStats operator+(const RtxStats& other)
    {
        RtxStats sum = *this;
        sum += other;
        return sum;
    }
};

class RtpAudioReceiver
{
public:
    RtpAudioReceiver(size_t instanceId,
        uint32_t ssrc,
        const bridge::RtpMap& rtpMap,
        const bridge::RtpMap& telephoneEventRtpMap,
        transport::RtcTransport* transport,
        emulator::Audio emulatedAudioType,
        uint64_t timestamp)
        : _rtpMap(rtpMap),
          _telephoneEventRtpMap(telephoneEventRtpMap),
          _context(ssrc, _rtpMap, _telephoneEventRtpMap, transport, timestamp),
          _loggableId("rtprcv", instanceId),
          _emulatedAudioType(emulatedAudioType)
    {
        _recording.reserve(256 * 1024);
    }

    void onRtpPacketReceived(transport::RtcTransport* sender,
        memory::Packet& packet,
        uint32_t extendedSequenceNumber,
        uint64_t timestamp)
    {
        _context.onRtpPacketReceived(timestamp);
        if (!sender->unprotect(packet))
        {
            return;
        }

        auto rtpHeader = rtp::RtpHeader::fromPacket(packet);
        if (_emulatedAudioType == Audio::Opus)
        {
            addOpus(reinterpret_cast<unsigned char*>(rtpHeader->getPayload()),
                packet.getLength() - rtpHeader->headerLength(),
                extendedSequenceNumber);
        }
    }

    void addOpus(const unsigned char* opusData, int32_t payloadLength, uint32_t extendedSequenceNumber)
    {
        int16_t decodedData[memory::AudioPacket::size];

        auto count = _decoder.decode(extendedSequenceNumber,
            opusData,
            payloadLength,
            reinterpret_cast<unsigned char*>(decodedData),
            memory::AudioPacket::size / codec::Opus::channelsPerFrame / codec::Opus::bytesPerSample);

        if (extendedSequenceNumber % 50 == 0)
        {
            logger::debug("recorded %u, %d samples", _loggableId.c_str(), extendedSequenceNumber, count);
        }
        for (int32_t i = 0; i < count; ++i)
        {
            _recording.push_back(decodedData[i * 2]);
        }
    }

    void dumpPcmData()
    {
        utils::StringBuilder<512> fileName;
        fileName.append(_loggableId.c_str()).append("-").append(_context.ssrc);
        logger::info("storing recording %s, %zu samples, pcm16-mono",
            _loggableId.c_str(),
            fileName.get(),
            _recording.size());

        FILE* logFile = ::fopen(fileName.get(), "wr");
        ::fwrite(_recording.data(), _recording.size(), 2, logFile);
        ::fclose(logFile);
    }

    const std::vector<int16_t>& getRecording() const { return _recording; }
    const logger::LoggableId& getLoggableId() const { return _loggableId; }

private:
    bridge::RtpMap _rtpMap;
    bridge::RtpMap _telephoneEventRtpMap;
    bridge::SsrcInboundContext _context;
    codec::OpusDecoder _decoder;
    logger::LoggableId _loggableId;
    std::vector<int16_t> _recording;
    const Audio _emulatedAudioType;
};

class RtpVideoReceiver
{
public:
    enum class VideoContent
    {
        LOCAL,
        VIDEO,
        SLIDES
    };

    RtpVideoReceiver(size_t instanceId,
        size_t endpointIdHash,
        api::SimulcastGroup ssrcs,
        const bridge::RtpMap& rtpMap,
        const bridge::RtpMap& rtxRtpMap,
        transport::RtcTransport* transport,
        VideoContent content,
        uint64_t timestamp)
        : contexts(256),
          _rtpMap(rtpMap),
          _rtxRtpMap(rtxRtpMap),
          _loggableId("rtprcv", instanceId),
          _ssrcs(ssrcs),
          _videoContent(content),
          _videoDecoder(endpointIdHash, instanceId)
    {
        _recording.reserve(256 * 1024);
        logger::info("video offered ssrc %u, payload %u", _loggableId.c_str(), _ssrcs[0].main, _rtpMap.payloadType);
    }

    RtxStats getRtxStats() const { return _rtxStats; }

    const FakeVideoDecoder::Stats getVideoStats() const { return _videoDecoder.getStats(); }

    void onRtpPacketReceived(transport::RtcTransport* sender,
        memory::Packet& packet,
        uint32_t extendedSequenceNumber,
        uint64_t timestamp)
    {
        auto rtpHeader = rtp::RtpHeader::fromPacket(packet);

        bool found = false;
        api::SsrcPair ssrcLevel;
        for (const auto& ssrc : _ssrcs)
        {
            if (ssrc.feedback == rtpHeader->ssrc || ssrc.main == rtpHeader->ssrc)
            {
                ssrcLevel = ssrc;
                found = true;
                break;
            }
        }
        assert(found);
        if (rtpHeader->ssrc == ssrcLevel.main)
        {
            if (ssrcLevel.feedback != 0)
            {
                assert(rtpHeader->payloadType == _rtpMap.payloadType);
            }
        }
        else
        {
            assert(rtpHeader->payloadType == _rtxRtpMap.payloadType);
        }

        auto it = contexts.find(rtpHeader->ssrc.get());
        if (it == contexts.end())
        {
            // we should perhaps figure out which simulcastLevel this is among the 3
            auto result =
                contexts.emplace(rtpHeader->ssrc.get(), rtpHeader->ssrc.get(), _rtpMap, sender, timestamp, 0, 0);
            it = result.first;
        }

        auto& inboundContext = it->second;

        if (!inboundContext.videoMissingPacketsTracker)
        {
            inboundContext.lastReceivedExtendedSequenceNumber = extendedSequenceNumber;
            inboundContext.videoMissingPacketsTracker = std::make_shared<bridge::VideoMissingPacketsTracker>();
        }

        inboundContext.onRtpPacketReceived(timestamp);
#if 0
            logger::debug("%s received ssrc %u, seq %u, extseq %u",
                _loggableId.c_str(),
                sender->getLoggableId().c_str(),
                inboundContext.ssrc,
                rtpHeader->sequenceNumber.get(),
                inboundContext.lastReceivedExtendedSequenceNumber);
#endif

        if (!sender->unprotect(packet))
        {
            return;
        }

        if (rtpHeader->payloadType == _rtpMap.payloadType)
        {
            ++videoPacketCount;
        }
        else if (!_rtxRtpMap.isEmpty() && rtpHeader->payloadType == _rtxRtpMap.payloadType)
        {
            ++rtxPacketCount;
        }
        else
        {
            ++unknownPayloadPacketCount;

            logger::warn("%u unexpected payload type %u",
                _loggableId.c_str(),
                rtpHeader->ssrc.get(),
                rtpHeader->payloadType);
        }

        const auto payload = rtpHeader->getPayload();
        const auto payloadSize = packet.getLength() - rtpHeader->headerLength();
        const auto payloadDescriptorSize = codec::Vp8Header::getPayloadDescriptorSize(payload, payloadSize);
        const bool isKeyframe = codec::Vp8Header::isKeyFrame(payload, payloadDescriptorSize);
        const auto sequenceNumber = rtpHeader->sequenceNumber.get();
        bool missingPacketsTrackerReset = false;

        if (isKeyframe)
        {
            inboundContext.videoMissingPacketsTracker->reset();
            missingPacketsTrackerReset = true;
            _videoDecoder.resetPacketCache();
        }

        if (extendedSequenceNumber > inboundContext.lastReceivedExtendedSequenceNumber)
        {
            if (extendedSequenceNumber - inboundContext.lastReceivedExtendedSequenceNumber >=
                bridge::VideoMissingPacketsTracker::maxMissingPackets)
            {
                logger::info("Resetting full missing packet tracker for %s, ssrc %u",
                    "SfuClient",
                    sender->getLoggableId().c_str(),
                    inboundContext.ssrc);

                inboundContext.videoMissingPacketsTracker->reset();
                _videoDecoder.resetPacketCache();
            }
            else if (!missingPacketsTrackerReset)
            {
                for (uint32_t missingSequenceNumber = inboundContext.lastReceivedExtendedSequenceNumber + 1;
                     missingSequenceNumber != extendedSequenceNumber;
                     ++missingSequenceNumber)
                {
                    _rtxStats.receiver.packetsMissing++;
                    inboundContext.videoMissingPacketsTracker->onMissingPacket(missingSequenceNumber, timestamp);
                }
            }

            inboundContext.lastReceivedExtendedSequenceNumber = extendedSequenceNumber;
        }
        else if (extendedSequenceNumber != inboundContext.lastReceivedExtendedSequenceNumber)
        {
            uint32_t esn = 0;
            if (!inboundContext.videoMissingPacketsTracker->onPacketArrived(sequenceNumber, esn) ||
                esn != extendedSequenceNumber)
            {
                logger::debug("%s Unexpected re-transmission of packet seq %u ssrc %u, dropping",
                    "SfuClient",
                    sender->getLoggableId().c_str(),
                    sequenceNumber,
                    inboundContext.ssrc);
                return;
            }
            else
            {
                _rtxStats.receiver.packetsRecovered++;
            }
        }

        if (inboundContext.videoMissingPacketsTracker)
        {
            std::array<uint16_t, bridge::VideoMissingPacketsTracker::maxMissingPackets> missingSequenceNumbers;
            const auto numMissingSequenceNumbers =
                inboundContext.videoMissingPacketsTracker->process(utils::Time::getAbsoluteTime(),
                    inboundContext.sender->getRtt(),
                    missingSequenceNumbers);

            if (numMissingSequenceNumbers)
            {
                logger::debug("Video missing packet tracker: %zu packets missing",
                    sender->getLoggableId().c_str(),
                    numMissingSequenceNumbers);
                for (size_t i = 0; i < numMissingSequenceNumbers; i++)
                {
                    logger::debug("\n missing sequence number: %u", "SfuClient", missingSequenceNumbers[i]);
                }
            }
        }

        if (rtpHeader->padding == 1)
        {
            assert(rtpHeader->payloadType == _rtxRtpMap.payloadType);
            return;
        }
        _videoDecoder.process(payload, payloadSize, timestamp);
    }

    const logger::LoggableId& getLoggableId() const
    {
        return _loggableId;
    }

    bool hasPackets() const
    {
        return (videoPacketCount | rtxPacketCount | unknownPayloadPacketCount) != 0;
    }
    VideoContent getContent() const
    {
        return _videoContent;
    }
    bool isLocalContent() const
    {
        return _videoContent == VideoContent::LOCAL;
    }
    bool isSlidesContent() const
    {
        return _videoContent == VideoContent::SLIDES;
    }

    size_t videoPacketCount = 0;
    size_t rtxPacketCount = 0;
    size_t unknownPayloadPacketCount = 0;

    concurrency::MpmcHashmap32<uint32_t, bridge::SsrcInboundContext> contexts;

private:
    bridge::RtpMap _rtpMap;
    bridge::RtpMap _rtxRtpMap;
    codec::OpusDecoder _decoder;
    logger::LoggableId _loggableId;
    std::vector<int16_t> _recording;
    api::SimulcastGroup _ssrcs;
    VideoContent _videoContent;
    RtxStats _rtxStats;
    FakeVideoDecoder _videoDecoder;
};

} // namespace emulator
