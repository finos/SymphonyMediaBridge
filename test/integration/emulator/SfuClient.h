#pragma once

#include "AudioSource.h"
#include "FakeVideoDecoder.h"
#include "api/SimulcastGroup.h"
#include "bridge/engine/PacketCache.h"
#include "bridge/engine/VideoMissingPacketsTracker.h"
#include "codec/Vp8Header.h"
#include "legacyapi/DataChannelMessage.h"
#include "memory/Array.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtcpFeedback.h"
#include "rtp/RtcpHeader.h"
#include "test/transport/FakeNetwork.h"
#include "transport/DataReceiver.h"
#include "transport/RtcTransport.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SslDtls.h"
#include "utils/IdGenerator.h"
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

template <typename ChannelType>
class SfuClient : public transport::DataReceiver
{
public:
    SfuClient(emulator::HttpdFactory* httpd,
        uint32_t id,
        memory::PacketPoolAllocator& allocator,
        memory::AudioPacketPoolAllocator& audioAllocator,
        transport::TransportFactory& transportFactory,
        transport::SslDtls& sslDtls,
        uint32_t ptime = 20)
        : _channel(httpd),
          _httpd(httpd),
          _allocator(allocator),
          _audioAllocator(audioAllocator),
          _transportFactory(transportFactory),
          _sslDtls(sslDtls),
          _audioReceivers(256),
          _videoSsrcMap(128),
          _loggableId("client", id),
          _recordingActive(true),
          _ptime(ptime),
          _audioType(Audio::None)
    {
    }

    ~SfuClient()
    {
        if (_transport && _transport->isRunning())
        {
            _transport->stop();
        }
        while (_transport && _transport->hasPendingJobs())
        {
            utils::Time::rawNanoSleep(utils::Time::ms * 20);
        }

        for (auto& item : _audioReceivers)
        {
            delete item.second;
        }
    }

    RtxStats getCumulativeRtxStats() const
    {
        auto stats = _rtxStats;
        for (const auto& rcv : _videoReceivers)
        {
            stats += rcv->getRtxStats();
        }
        return stats;
    }

    void initiateCall(const std::string& baseUrl,
        std::string conferenceId,
        bool initiator,
        Audio audio,
        bool video,
        bool forwardMedia,
        uint32_t idleTimeout = 0)
    {
        utils::Span<std::string> noNeighbours;
        _audioType = audio;
        _channel.create(baseUrl,
            conferenceId,
            initiator,
            audio != Audio::None,
            video,
            forwardMedia,
            idleTimeout,
            noNeighbours);
        logger::info("client started %s", _loggableId.c_str(), _channel.getEndpointId().c_str());
    }

    void initiateCall2(const std::string& baseUrl,
        std::string conferenceId,
        bool initiator,
        Audio audio,
        bool video,
        bool forwardMedia,
        const utils::Span<std::string>& neighbours,
        uint32_t idleTimeout = 0)
    {
        _audioType = audio;
        _channel.create(baseUrl,
            conferenceId,
            initiator,
            audio != Audio::None,
            video,
            forwardMedia,
            idleTimeout,
            neighbours);
        logger::info("client started %s", _loggableId.c_str(), _channel.getEndpointId().c_str());
    }

    size_t getEndpointIdHash() const { return _channel.getEndpointIdHash(); }
    std::string getEndpointId() const { return _channel.getEndpointId(); }

    void processOffer()
    {
        auto offer = _channel.getOffer();

        _transport =
            _transportFactory.createOnPrivatePort(ice::IceRole::CONTROLLED, 256 * 1024, _channel.getEndpointIdHash());
        _transport->setDataReceiver(this);
        _transport->setAbsSendTimeExtensionId(3);

        _channel.configureTransport(*_transport, _audioAllocator);

        if (_channel.isAudioOffered())
        {
            _audioSource = std::make_unique<emulator::AudioSource>(_allocator, _idGenerator.next(), _audioType);
            _transport->setAudioPayloadType(111, codec::Opus::sampleRate);
        }

        logger::info("client using %s", _loggableId.c_str(), _transport->getLoggableId().c_str());

        _remoteVideoSsrc = _channel.getOfferedVideoSsrcs();
        _remoteVideoStreams = _channel.getOfferedVideoStreams();
        utils::Optional<uint32_t> localSsrc = _channel.getOfferedLocalSsrc();
        utils::Optional<uint32_t> slidesSsrc = _channel.getOfferedScreensharingSsrc();

        bridge::RtpMap videoRtpMap(bridge::RtpMap::Format::VP8);
        bridge::RtpMap feedbackRtpMap(bridge::RtpMap::Format::VP8RTX);

        bool isLocalSsrcFound = false;

        for (auto& stream : _remoteVideoStreams)
        {
            const auto videoContent = localSsrc.isSet() && stream.containsMainSsrc(localSsrc.get())
                ? RtpVideoReceiver::VideoContent::LOCAL
                : slidesSsrc.isSet() && stream.containsMainSsrc(slidesSsrc.get())
                ? RtpVideoReceiver::VideoContent::SLIDES
                : RtpVideoReceiver::VideoContent::VIDEO;

            isLocalSsrcFound = isLocalSsrcFound || videoContent == RtpVideoReceiver::VideoContent::LOCAL;

            _videoReceivers.emplace_back(std::make_unique<RtpVideoReceiver>(++_instanceId,
                _channel.getEndpointIdHash(),
                stream,
                videoRtpMap,
                feedbackRtpMap,
                _transport.get(),
                videoContent,
                utils::Time::getAbsoluteTime()));

            for (auto& ssrcPair : stream)
            {
                _videoSsrcMap.emplace(ssrcPair.main, _videoReceivers.back().get());
                if (ssrcPair.feedback != 0)
                {
                    _videoSsrcMap.emplace(ssrcPair.feedback, _videoReceivers.back().get());
                }
            }
        }

        // Local ssrc probably is not in _remoteVideoSsrc
        if (!isLocalSsrcFound && localSsrc.isSet())
        {
            api::SsrcPair ssrcPair[1];
            ssrcPair[0].main = localSsrc.get();
            ssrcPair[0].feedback = 0;
            auto stream = makeSsrcGroup(ssrcPair);

            _videoReceivers.emplace_back(std::make_unique<RtpVideoReceiver>(++_instanceId,
                _channel.getEndpointIdHash(),
                stream,
                videoRtpMap,
                feedbackRtpMap,
                _transport.get(),
                RtpVideoReceiver::VideoContent::LOCAL,
                utils::Time::getAbsoluteTime()));

            for (auto& ssrcPair : stream)
            {
                _videoSsrcMap.emplace(ssrcPair.main, _videoReceivers.back().get());
                if (ssrcPair.feedback != 0)
                {
                    _videoSsrcMap.emplace(ssrcPair.feedback, _videoReceivers.back().get());
                }
            }
        }
    }

    std::vector<FakeVideoDecoder::Stats> getActiveVideoDecoderStats()
    {
        std::vector<FakeVideoDecoder::Stats> result;
        for (const auto& receiver : _videoReceivers)
        {
            const auto stats = receiver->getVideoStats();
            if (stats.numDecodedFrames)
            {
                result.push_back(stats);
            }
        }
        return result;
    }

    void connect()
    {
        if (_channel.isVideoEnabled())
        {
            _videoSsrcs[6] = 0;
            const size_t bitrates[] = {100, 500, 2500};
            for (int i = 0; i < 6; ++i)
            {
                _videoSsrcs[i] = _idGenerator.next();
                if (0 == i % 2)
                {
                    _videoSources.emplace(_videoSsrcs[i],
                        std::make_unique<fakenet::FakeVideoSource>(_allocator,
                            bitrates[i / 2],
                            _videoSsrcs[i],
                            _channel.getEndpointIdHash(),
                            i / 2));
                    _videoCaches.emplace(_videoSsrcs[i],
                        std::make_unique<bridge::PacketCache>(
                            (std::string("VideoCache_") + std::to_string(_videoSsrcs[i])).c_str(),
                            _videoSsrcs[i]));
                    _videoFeedbackSequenceCounter.emplace(_videoSsrcs[i], 0);
                }
            }
        }

        assert(_audioSource);

        _channel.sendResponse(_transport->getLocalCredentials(),
            _transport->getLocalCandidates(),
            _sslDtls.getLocalFingerprint(),
            _audioSource->getSsrc(),
            _channel.isVideoEnabled() ? _videoSsrcs : nullptr);

        _dataStream = std::make_unique<webrtc::WebRtcDataStream>(_loggableId.getInstanceId(), *_transport);
        _transport->start();
        _transport->connect();
    }

    void disconnect() { _channel.disconnect(); }

    void process(uint64_t timestamp) { process(timestamp, true); }

    void process(uint64_t timestamp, bool sendVideo)
    {
        auto packet = _audioSource->getPacket(timestamp);
        if (packet)
        {
            /*const auto* rtpHeader = rtp::RtpHeader::fromPacket(*packet);
            const auto refCount = rtpHeader->sequenceNumber % 100;
            if (refCount >= 0 && refCount <= 5)
            {
                logger::debug("discarding send packet", _loggableId.c_str());
                _allocator.free(packet);
                return;
            }*/
            _transport->getJobQueue().addJob<MediaSendJob>(*_transport, std::move(packet), timestamp);
        }

        if (sendVideo)
        {
            for (const auto& videoSource : _videoSources)
            {
                while (auto packet = videoSource.second->getPacket(timestamp))
                {
                    // auto rtpHeader = rtp::RtpHeader::fromPacket(*packet);
                    // logger::debug("sending video %u", _loggableId.c_str(), rtpHeader->ssrc.get());

                    auto cache = _videoCaches.find(videoSource.second->getSsrc());
                    if (cache != _videoCaches.end())
                    {
                        auto rtpHeader = rtp::RtpHeader::fromPacket(*packet);
                        cache->second->add(*packet, rtpHeader->sequenceNumber);
                    }
                    _rtxStats.sender.packetsSent++;
                    if (!_transport->getJobQueue().addJob<MediaSendJob>(*_transport, std::move(packet), timestamp))
                    {
                        logger::warn("failed to add SendMediaJob", "SfuClient");
                    }
                }
            }
        }
    }

    ChannelType _channel;

    class RtpAudioReceiver
    {
    public:
        RtpAudioReceiver(size_t instanceId,
            uint32_t ssrc,
            const bridge::RtpMap& rtpMap,
            transport::RtcTransport* transport,
            emulator::Audio fakeAudio,
            uint64_t timestamp)
            : _rtpMap(rtpMap),
              _context(ssrc, _rtpMap, transport, timestamp),
              _loggableId("rtprcv", instanceId),
              _fakeAudio(fakeAudio)
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
            if (_fakeAudio == Audio::Opus)
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

            for (int32_t i = 0; i < count; ++i)
            {
                _recording.push_back(decodedData[i * 2]);
            }
        }

        void dumpPcmData()
        {
            utils::StringBuilder<512> fileName;
            fileName.append(_loggableId.c_str()).append("-").append(_context.ssrc);

            FILE* logFile = ::fopen(fileName.get(), "wr");
            ::fwrite(_recording.data(), _recording.size(), 2, logFile);
            ::fclose(logFile);
        }

        const std::vector<int16_t>& getRecording() const { return _recording; }
        const logger::LoggableId& getLoggableId() const { return _loggableId; }

    private:
        bridge::RtpMap _rtpMap;
        bridge::SsrcInboundContext _context;
        codec::OpusDecoder _decoder;
        logger::LoggableId _loggableId;
        std::vector<int16_t> _recording;
        const Audio _fakeAudio;
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
                    contexts.emplace(rtpHeader->ssrc.get(), rtpHeader->ssrc.get(), _rtpMap, sender, timestamp);
                it = result.first;
            }

            auto& inboundContext = it->second;
            ++inboundContext.packetsProcessed;

            if (inboundContext.packetsProcessed == 1)
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
                inboundContext.videoMissingPacketsTracker->reset(timestamp);
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

                    inboundContext.videoMissingPacketsTracker->reset(timestamp);
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

        const logger::LoggableId& getLoggableId() const { return _loggableId; }

        bool hasPackets() const { return (videoPacketCount | rtxPacketCount | unknownPayloadPacketCount) != 0; }
        VideoContent getContent() const { return _videoContent; }
        bool isLocalContent() const { return _videoContent == VideoContent::LOCAL; }
        bool isSlidesContent() const { return _videoContent == VideoContent::SLIDES; }

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

public:
    void onRtpPacketReceived(transport::RtcTransport* sender,
        const memory::UniquePacket packet,
        uint32_t extendedSequenceNumber,
        uint64_t timestamp) override
    {
        auto rtpHeader = rtp::RtpHeader::fromPacket(*packet);

        if (rtpHeader->payloadType == 111)
        {
            auto it = _audioReceivers.find(rtpHeader->ssrc.get());
            if (it == _audioReceivers.end())
            {
                bridge::RtpMap rtpMap(bridge::RtpMap::Format::OPUS);
                rtpMap.audioLevelExtId.set(1);
                rtpMap.c9infoExtId.set(8);
                _audioReceivers.emplace(rtpHeader->ssrc.get(),
                    new RtpAudioReceiver(_loggableId.getInstanceId(),
                        rtpHeader->ssrc.get(),
                        rtpMap,
                        sender,
                        _audioType,
                        timestamp));
                it = _audioReceivers.find(rtpHeader->ssrc.get());
            }
            if (it != _audioReceivers.end())
            {
                if (_recordingActive.load())
                {
                    it->second->onRtpPacketReceived(sender, *packet, extendedSequenceNumber, timestamp);
                }
            }
        }
        else
        {
            bridge::RtpMap rtpMap(bridge::RtpMap::Format::VP8);
            bridge::RtpMap fbMap(bridge::RtpMap::Format::VP8RTX);

            auto it = _videoSsrcMap.find(rtpHeader->ssrc.get());
            if (it == _videoSsrcMap.end())
            {
                if (_remoteVideoSsrc.find(rtpHeader->ssrc.get()) == _remoteVideoSsrc.end())
                {
                    logger::warn("unexpected video ssrc %u", _loggableId.c_str(), rtpHeader->ssrc.get());
                }
                return;
            }

            it->second->onRtpPacketReceived(sender, *packet, extendedSequenceNumber, timestamp);
        }
    }

    void onRtcpPacketDecoded(transport::RtcTransport* sender, memory::UniquePacket packet, uint64_t timestamp) override
    {
        rtp::CompoundRtcpPacket compoundPacket(packet->get(), packet->getLength());
        for (const auto& rtcpPacket : compoundPacket)
        {
            auto rtcpFeedback = reinterpret_cast<const rtp::RtcpFeedback*>(&rtcpPacket);
            if (rtcpPacket.packetType == rtp::RtcpPacketType::PAYLOADSPECIFIC_FB &&
                rtcpFeedback->header.fmtCount == rtp::PayloadSpecificFeedbackType::Pli)
            {
                processRtcpPli(sender, rtcpFeedback);
            }
            if (rtcpPacket.packetType == rtp::RtcpPacketType::RTPTRANSPORT_FB &&
                rtcpFeedback->header.fmtCount == rtp::TransportLayerFeedbackType::PacketNack)
            {
                processRtcpNack(sender, rtcpFeedback);
            }
        }
    }

    void processRtcpPli(transport::RtcTransport* sender, const rtp::RtcpFeedback* rtcpFeedback)
    {
        logger::debug("PLI for %u", _loggableId.c_str(), rtcpFeedback->mediaSsrc.get());
        auto it = _videoSources.find(rtcpFeedback->mediaSsrc.get());
        if (it != _videoSources.end())
        {
            auto& videoSource = it->second;
            if (videoSource->getSsrc() == rtcpFeedback->mediaSsrc.get())
            {
                videoSource->requestKeyFrame();
            }
        }
        else
        {
            logger::warn("cannot find video ssrc for PLI %u", _loggableId.c_str(), rtcpFeedback->mediaSsrc.get());
            for (auto& it : _videoSources)
            {
                logger::debug("vsssrc %u", _loggableId.c_str(), it.second->getSsrc());
            }
        }
    }

    void processRtcpNack(transport::RtcTransport* sender, const rtp::RtcpFeedback* rtcpFeedback)
    {
        _rtxStats.sender.nacksReceived++;

        const auto mediaSsrc = rtcpFeedback->mediaSsrc.get();
        if (mediaSsrc)
        {
            logger::warn("SfuClient received NACK, ssrc %u", sender->getLoggableId().c_str(), mediaSsrc);

            const auto numFeedbackControlInfos = rtp::getNumFeedbackControlInfos(rtcpFeedback);
            uint16_t pid = 0;
            uint16_t blp = 0;

            for (size_t i = 0; i < numFeedbackControlInfos; ++i)
            {
                rtp::getFeedbackControlInfo(rtcpFeedback, i, numFeedbackControlInfos, pid, blp);

                auto sequenceNumber = pid;
                sendIfCached(mediaSsrc, sequenceNumber);

                while (blp != 0)
                {
                    ++sequenceNumber;

                    if ((blp & 0x1) == 0x1)
                    {
                        sendIfCached(mediaSsrc, sequenceNumber);
                    }

                    blp = blp >> 1;
                }
            }
        }
    }

    uint32_t getFeedbackSsrc(uint32_t ssrc)
    {
        for (auto i : {0, 2, 4})
        {
            if (_videoSsrcs[i] == ssrc)
            {
                return _videoSsrcs[i + 1];
            }
        }
        return 0;
    }

    void sendIfCached(const uint32_t ssrc, const uint16_t sequenceNumber)
    {
        const auto feedbackSsrc = getFeedbackSsrc(ssrc);
        auto cache = _videoCaches.find(ssrc);
        auto videoSource = _videoSources.find(ssrc);

        _rtxStats.sender.retransmissionRequests++;

        if (videoSource->second->isKeyFrameRequested())
        {
            logger::info("Ignoring NACK for pre key frame packet %u, key frame at %u",
                "SfuClient",
                ssrc,
                sequenceNumber);
            return;
        }

        const auto cachedPacket = cache->second->get(sequenceNumber);
        if (!cachedPacket)
        {
            return;
        }

        const auto cachedRtpHeader = rtp::RtpHeader::fromPacket(*cachedPacket);
        if (!cachedRtpHeader)
        {
            return;
        }

        auto packet = memory::makeUniquePacket(_allocator);
        if (!packet)
        {
            return;
        }

        const auto cachedRtpHeaderLength = cachedRtpHeader->headerLength();
        const auto cachedPayload = cachedRtpHeader->getPayload();
        const auto cachedSequenceNumber = cachedRtpHeader->sequenceNumber.get();

        memcpy(packet->get(), cachedPacket->get(), cachedRtpHeaderLength);
        auto copyHead = packet->get() + cachedRtpHeaderLength;
        reinterpret_cast<uint16_t*>(copyHead)[0] = hton<uint16_t>(cachedSequenceNumber);
        copyHead += sizeof(uint16_t);
        memcpy(copyHead, cachedPayload, cachedPacket->getLength() - cachedRtpHeaderLength);
        packet->setLength(cachedPacket->getLength() + sizeof(uint16_t));

        auto videoFeedbackSequenceCounterItr = _videoFeedbackSequenceCounter.find(ssrc);

        const auto sequenceCounter = videoFeedbackSequenceCounterItr->second;

        auto rtpHeader = rtp::RtpHeader::fromPacket(*packet);
        if (!rtpHeader)
        {
            return;
        }

        rtpHeader->ssrc = feedbackSsrc;
        rtpHeader->payloadType = bridge::RtpMap(bridge::RtpMap::Format::VP8RTX).payloadType;
        rtpHeader->sequenceNumber = sequenceCounter & 0xFFFF;

        videoFeedbackSequenceCounterItr->second++;

        logger::info("Sending cached packet seq %u, ssrc %u, feedbackSsrc %u, seq %u",
            "SfuClient",
            sequenceNumber,
            ssrc,
            feedbackSsrc,
            sequenceCounter & 0xFFFFu);

        _rtxStats.sender.retransmissions++;

        _transport->protectAndSend(std::move(packet));
    }

    void onConnected(transport::RtcTransport* sender) override
    {
        logger::debug("client connected", _loggableId.c_str());
        _transport->setSctp(5000, 5000);
        _transport->connectSctp();
    }

    bool onSctpConnectionRequest(transport::RtcTransport* sender, uint16_t remotePort) override { return false; }
    void onSctpEstablished(transport::RtcTransport* sender) override
    {
        auto streamId = _dataStream->open(_loggableId.c_str());
        assert(streamId != 0xFFFFu);
    }

    void onSctpMessage(transport::RtcTransport* sender,
        uint16_t streamId,
        uint16_t streamSequenceNumber,
        uint32_t payloadProtocol,
        const void* data,
        size_t length) override
    {
        _dataStream->onSctpMessage(sender, streamId, streamSequenceNumber, payloadProtocol, data, length);
    }

    void onRecControlReceived(transport::RecordingTransport* sender,
        memory::UniquePacket packet,
        uint64_t timestamp) override
    {
    }

    bool isRemoteVideoSsrc(uint32_t ssrc) const { return _remoteVideoSsrc.find(ssrc) != _remoteVideoSsrc.end(); }

    void stopRecording() { _recordingActive = false; }

    std::shared_ptr<transport::RtcTransport> _transport;

    std::unique_ptr<emulator::AudioSource> _audioSource;
    // Video source that produces fake VP8
    std::unordered_map<uint32_t, std::unique_ptr<fakenet::FakeVideoSource>> _videoSources;
    std::unordered_map<uint32_t, std::unique_ptr<bridge::PacketCache>> _videoCaches;
    std::unordered_map<uint32_t, uint32_t> _videoFeedbackSequenceCounter;

    const concurrency::MpmcHashmap32<uint32_t, RtpAudioReceiver*>& getAudioReceiveStats() const
    {
        return _audioReceivers;
    }
    const logger::LoggableId& getLoggableId() const { return _loggableId; }

    std::vector<RtpVideoReceiver*> collectReceiversWithPackets()
    {
        std::vector<RtpVideoReceiver*> v;
        for (auto& receiver : _videoReceivers)
        {
            if (receiver->hasPackets())
            {
                v.push_back(receiver.get());
            }
        }

        return v;
    }

    uint32_t getVideoPacketsReceived()
    {
        uint32_t count = 0;
        for (auto& receiver : _videoReceivers)
        {
            count += receiver->videoPacketCount;
        }
        return count;
    }

    void sendEndpointMessage(const std::string& toEndpointId, const char* message)
    {
        if (!_dataStream)
        {
            return;
        }

        utils::StringBuilder<2048> builder;
        legacyapi::DataChannelMessage::makeEndpointMessage(builder, toEndpointId, getEndpointId(), message);
        _dataStream->sendString(builder.build().c_str(), builder.getLength());
    }

    void setDataListener(webrtc::WebRtcDataStream::Listener* listener) { _dataStream->setListener(listener); }

private:
    utils::IdGenerator _idGenerator;
    uint32_t _videoSsrcs[7];
    emulator::HttpdFactory* _httpd;
    memory::PacketPoolAllocator& _allocator;
    memory::AudioPacketPoolAllocator& _audioAllocator;
    transport::TransportFactory& _transportFactory;
    transport::SslDtls& _sslDtls;
    concurrency::MpmcHashmap32<uint32_t, RtpAudioReceiver*> _audioReceivers;
    std::vector<std::unique_ptr<RtpVideoReceiver>> _videoReceivers;
    concurrency::MpmcHashmap32<uint32_t, RtpVideoReceiver*> _videoSsrcMap;
    logger::LoggableId _loggableId;
    std::atomic_bool _recordingActive;
    std::unordered_set<uint32_t> _remoteVideoSsrc;
    std::vector<api::SimulcastGroup> _remoteVideoStreams;
    uint32_t _ptime;
    std::unique_ptr<webrtc::WebRtcDataStream> _dataStream;
    size_t _instanceId;
    RtxStats _rtxStats;
    Audio _audioType;
};

template <typename TClient>
class GroupCall
{
public:
    GroupCall(emulator::HttpdFactory* httpd,
        uint32_t& idCounter,
        memory::PacketPoolAllocator& allocator,
        memory::AudioPacketPoolAllocator& audioAllocator,
        transport::TransportFactory& transportFactory,
        transport::SslDtls& sslDtls,
        uint32_t callCount)
        : _idCounter(idCounter),
          _allocator(allocator),
          _audioAllocator(audioAllocator),
          _transportFactory(transportFactory),
          _sslDtls(sslDtls)
    {
        for (uint32_t i = 0; i < callCount; ++i)
        {
            add(httpd);
        }
    }

    bool connectAll(uint64_t timeout)
    {
        auto start = utils::Time::getAbsoluteTime();
        for (auto& client : clients)
        {
            if (!client->_channel.isSuccess())
            {
                logger::warn("client has not received offer yet", client->getLoggableId().c_str());
                return false;
            }
        }

        for (auto& client : clients)
        {
            if (client->_transport)
            {
                continue; // already connected
            }

            client->processOffer();
            if (!client->_transport || !client->_audioSource)
            {
                logger::warn("client did not parse offer successfully transport %s, audio source %s",
                    client->getLoggableId().c_str(),
                    client->_transport ? "ok" : "bad",
                    client->_audioSource ? "ok" : "bad");
                return false;
            }
        }

        for (auto& client : clients)
        {
            client->connect();
        }

        auto currTime = utils::Time::getAbsoluteTime();
        while (currTime - start < timeout)
        {
            auto it =
                std::find_if_not(clients.begin(), clients.end(), [](auto& c) { return c->_transport->isConnected(); });

            if (it == clients.end())
            {
                logger::info("all clients connected", "test");
                return true;
            }

            utils::Time::nanoSleep(10 * utils::Time::ms);
            logger::debug("waiting for connect...", "test");
            currTime = utils::Time::getAbsoluteTime();
        }

        logger::warn("client transports failed to connect", "GroupCall.connectAll");
        return false;
    }

    bool connectSingle(uint32_t clientIndex, uint64_t timeout)
    {
        auto start = utils::Time::getAbsoluteTime();
        auto& client = clients[clientIndex];
        if (!client->_channel.isSuccess())
        {
            return false;
        }

        if (client->_transport)
        {
            return client->_transport->isConnected();
        }

        client->processOffer();
        if (!client->_transport || !client->_audioSource)
        {
            return false;
        }

        for (auto& client : clients)
        {
            client->connect();
        }

        auto currTime = utils::Time::getAbsoluteTime();
        while (currTime - start < timeout)
        {
            if (client->_transport->isConnected())
            {
                return true;
            }

            utils::Time::nanoSleep(10 * utils::Time::ms);
            logger::debug("waiting for connect...", "test");
            currTime = utils::Time::getAbsoluteTime();
        }

        return false;
    }

    void run(uint64_t period)
    {
        const auto start = utils::Time::getAbsoluteTime();
        utils::Pacer pacer(10 * utils::Time::ms);
        for (auto timestamp = utils::Time::getAbsoluteTime(); timestamp - start < period;)
        {
            for (auto& client : clients)
            {
                client->process(timestamp);
            }
            pacer.tick(utils::Time::getAbsoluteTime());
            utils::Time::nanoSleep(pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
            timestamp = utils::Time::getAbsoluteTime();
        }
    }

    bool awaitPendingJobs(uint64_t timeout)
    {
        auto start = utils::Time::getAbsoluteTime();
        for (size_t runCount = 1; utils::Time::getAbsoluteTime() - start < timeout;)
        {
            runCount = 0;
            utils::Time::nanoSleep(utils::Time::ms * 100);
            for (auto& client : clients)
            {
                if (client->_transport->hasPendingJobs())
                {
                    ++runCount;
                }
            }
            if (runCount == 0)
            {
                return true;
            }
        }
        return false;
    }

    static bool startConference(Conference& conf, std::string url, bool useGlobalPort = true)
    {
        conf.create(url, useGlobalPort);
        auto result = conf.isSuccess();
        utils::Time::nanoSleep(1 * utils::Time::sec);
        return result;
    }

    void add(emulator::HttpdFactory* httpd)
    {
        clients.emplace_back(
            std::make_unique<TClient>(httpd, ++_idCounter, _allocator, _audioAllocator, _transportFactory, _sslDtls));
    }

    void disconnectClients()
    {
        for (auto& client : clients)
        {
            client->disconnect();
        }
    }

    void stopTransports()
    {
        for (auto& client : clients)
        {
            client->_transport->stop();
        }
    }

    std::vector<std::unique_ptr<TClient>> clients;

private:
    uint32_t& _idCounter;
    memory::PacketPoolAllocator& _allocator;
    memory::AudioPacketPoolAllocator& _audioAllocator;
    transport::TransportFactory& _transportFactory;
    transport::SslDtls& _sslDtls;
    std::string _baseUrl;
};

} // namespace emulator
