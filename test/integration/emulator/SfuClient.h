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
#include "test/integration/emulator/SfuClientReceivers.h"
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

template <typename ChannelType>
class SfuClient : public transport::DataReceiver
{
public:
    SfuClient(emulator::HttpdFactory* httpd,
        uint32_t id,
        memory::PacketPoolAllocator& allocator,
        memory::AudioPacketPoolAllocator& audioAllocator,
        transport::TransportFactory& transportFactory,
        transport::TransportFactory& publicTransportFactory,
        transport::SslDtls& sslDtls)
        : _channel(httpd),
          _httpd(httpd),
          _allocator(allocator),
          _audioAllocator(audioAllocator),
          _transportFactory(transportFactory),
          _publicTransportFactory(publicTransportFactory),
          _sslDtls(sslDtls),
          _audioReceivers(256),
          _videoSsrcMap(128),
          _loggableId("client", id),
          _recordingActive(true),
          _expectedReceiveAudioType(Audio::None),
          _startTime(0)
    {
    }

    ~SfuClient()
    {
        stopTransports();

        while ((_audioTransport && _audioTransport->hasPendingJobs()) ||
            (_videoTransport && _videoTransport->hasPendingJobs()) ||
            (_bundleTransport && _bundleTransport->hasPendingJobs()))
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

    void initiateCall(const CallConfig& callConfig)
    {
        _callConfig = callConfig;
        _channel.create(true, callConfig);
        logger::info("client started %s", _loggableId.c_str(), _channel.getEndpointId().c_str());
    }

    void joinCall(const CallConfig& callConfig)
    {
        _callConfig = callConfig;
        if (_channel.create(false, callConfig))
        {
            logger::info("client started %s", _loggableId.c_str(), _channel.getEndpointId().c_str());
        }
        else
        {
            logger::error("client failed %s", _loggableId.c_str(), _channel.getEndpointId().c_str());
        }
    }

    void setExpectedAudioType(Audio audio) { _expectedReceiveAudioType = audio; }

    size_t getEndpointIdHash() const { return _channel.getEndpointIdHash(); }
    std::string getEndpointId() const { return _channel.getEndpointId(); }

    void processOffer()
    {
        auto offer = _channel.getOffer();

        if (_callConfig.transportMode == TransportMode::BundledIce)
        {
            _bundleTransport = _transportFactory.createOnPrivatePort(ice::IceRole::CONTROLLED,
                256 * 1024,
                _channel.getEndpointIdHash());
            _bundleTransport->setDataReceiver(this);
            _bundleTransport->setAbsSendTimeExtensionId(3);
            _channel.configureTransport(*_bundleTransport, _audioAllocator);
            logger::info("created transport %s", _loggableId.c_str(), _bundleTransport->getLoggableId().c_str());
        }
        else
        {
            if (_callConfig.audio != Audio::None)
            {
                if (_callConfig.transportMode == TransportMode::StreamTransportIce)
                {
                    _audioTransport = _transportFactory.createOnPrivatePort(ice::IceRole::CONTROLLED,
                        256 * 1024,
                        _channel.getEndpointIdHash());
                }
                else
                {
                    _audioTransport = _publicTransportFactory.create(256 * 1024, _channel.getEndpointIdHash());
                }
                _audioTransport->setDataReceiver(this);
                _audioTransport->setAbsSendTimeExtensionId(3);
                _channel.configureAudioTransport(*_audioTransport, _audioAllocator);
                logger::info("created audio transport %s, %s",
                    _loggableId.c_str(),
                    _audioTransport->getLoggableId().c_str(),
                    _audioTransport->getLocalRtpPort().toString().c_str());
            }

            if (_callConfig.video)
            {
                if (_callConfig.transportMode == TransportMode::StreamTransportIce)
                {
                    _videoTransport = _transportFactory.createOnPrivatePort(ice::IceRole::CONTROLLED,
                        256 * 1024,
                        _channel.getEndpointIdHash());
                }
                else
                {
                    _videoTransport = _publicTransportFactory.create(256 * 1024, _channel.getEndpointIdHash());
                }
                _videoTransport->setDataReceiver(this);
                _videoTransport->setAbsSendTimeExtensionId(3);
                _channel.configureVideoTransport(*_videoTransport, _audioAllocator);
                logger::info("created video transport %s",
                    _loggableId.c_str(),
                    _videoTransport->getLoggableId().c_str());
            }
        }

        if (_channel.isAudioOffered())
        {
            _audioSource = std::make_unique<emulator::AudioSource>(_allocator,
                _idGenerator.next(),
                _callConfig.audio,
                _callConfig.ptime);

            if (_bundleTransport)
            {
                _bundleTransport->setAudioPayloads(111, utils::NullOpt, codec::Opus::sampleRate);
            }
            else if (_audioTransport)
            {
                _audioTransport->setAudioPayloads(111, utils::NullOpt, codec::Opus::sampleRate);
            }
        }

        if (_bundleTransport)
        {
            logger::info("client using %s", _loggableId.c_str(), _bundleTransport->getLoggableId().c_str());
        }

        _remoteVideoSsrc = _channel.getOfferedVideoSsrcs();
        _remoteVideoStreams = _channel.getOfferedVideoStreams();
        utils::Optional<uint32_t> localSsrc = _channel.getOfferedLocalSsrc();
        utils::Optional<uint32_t> slidesSsrc = _channel.getOfferedScreensharingSsrc();

        bridge::RtpMap videoRtpMap(bridge::RtpMap::Format::VP8);
        bridge::RtpMap feedbackRtpMap(bridge::RtpMap::Format::RTX);

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
                _bundleTransport ? _bundleTransport.get() : _videoTransport.get(),
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
                _bundleTransport ? _bundleTransport.get() : _videoTransport.get(),
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

    bool connect()
    {
        if (_callConfig.video)
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
        std::vector<srtp::AesKey> sdesKeys;

        if (_bundleTransport)
        {
            _configured = _channel.sendResponse(*_bundleTransport,
                _sslDtls.getLocalFingerprint(),
                _audioSource->getSsrc(),
                _callConfig.video ? _videoSsrcs : nullptr);
        }
        else
        {
            _configured = _channel.sendResponse(_audioTransport.get(),
                _videoTransport.get(),
                _sslDtls.getLocalFingerprint(),
                _audioSource->getSsrc(),
                _callConfig.video ? _videoSsrcs : nullptr);
        }

        if (!_configured)
        {
            return false;
        }

        _dataStream = std::make_unique<webrtc::WebRtcDataStream>(_loggableId.getInstanceId(),
            (_bundleTransport ? *_bundleTransport : (_audioTransport ? *_audioTransport : *_videoTransport)));
        if (_bundleTransport)
        {
            _bundleTransport->start();
            _bundleTransport->connect();
        }
        if (_audioTransport)
        {
            _audioTransport->start();
            _audioTransport->connect();
        }
        if (_videoTransport)
        {
            _videoTransport->start();
            _videoTransport->connect();
        }

        return true;
    }

    void disconnect() { _channel.disconnect(); }

    void process(uint64_t timestamp) { process(timestamp, true); }

    void process(uint64_t timestamp, bool sendVideo)
    {
        if (_callConfig.ipv6CandidateDelay != 0 &&
            utils::Time::diffGE(_startTime, utils::Time::getAbsoluteTime(), _callConfig.ipv6CandidateDelay))
        {
            _callConfig.ipv6CandidateDelay = 0;
            if (_bundleTransport)
            {
                _channel.addIpv6RemoteCandidates(*_bundleTransport);
            }
            if (_audioTransport)
            {
                _channel.addIpv6RemoteCandidates(*_audioTransport);
            }
            if (_videoTransport)
            {
                _channel.addIpv6RemoteCandidates(*_videoTransport);
            }
        }

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
            auto* transport = _bundleTransport ? _bundleTransport.get() : _audioTransport.get();
            transport->getJobQueue().template addJob<MediaSendJob>(*transport, std::move(packet), timestamp);
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
                    auto* transport = _bundleTransport ? _bundleTransport.get() : _videoTransport.get();
                    if (!transport->getJobQueue().template addJob<MediaSendJob>(*transport,
                            std::move(packet),
                            timestamp))
                    {
                        logger::warn("failed to add SendMediaJob", "SfuClient");
                    }
                }
            }
        }
    }

    ChannelType _channel;

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
                logger::info("audio RTP received %u", _loggableId.c_str(), rtpHeader->sequenceNumber.get());
                bridge::RtpMap rtpMap(bridge::RtpMap::Format::OPUS);
                bridge::RtpMap telephoneEventRtpMap(bridge::RtpMap::Format::TELEPHONE_EVENT);
                rtpMap.audioLevelExtId.set(1);
                rtpMap.c9infoExtId.set(8);
                _audioReceivers.emplace(rtpHeader->ssrc.get(),
                    new RtpAudioReceiver(_loggableId.getInstanceId(),
                        rtpHeader->ssrc.get(),
                        rtpMap,
                        telephoneEventRtpMap,
                        sender,
                        _expectedReceiveAudioType == Audio::None ? _callConfig.audio : _expectedReceiveAudioType,
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
            bridge::RtpMap fbMap(bridge::RtpMap::Format::RTX);

            auto it = _videoSsrcMap.find(rtpHeader->ssrc.get());
            if (it == _videoSsrcMap.end())
            {
                if (_remoteVideoSsrc.find(rtpHeader->ssrc.get()) == _remoteVideoSsrc.end() &&
                    _callConfig.relayType != "forwarded")
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
                logger::debug("video ssrc %u", _loggableId.c_str(), it.second->getSsrc());
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
        rtpHeader->payloadType = bridge::RtpMap(bridge::RtpMap::Format::RTX).payloadType;
        rtpHeader->sequenceNumber = sequenceCounter & 0xFFFF;

        videoFeedbackSequenceCounterItr->second++;

        logger::info("Sending cached packet seq %u, ssrc %u, feedbackSsrc %u, seq %u",
            "SfuClient",
            sequenceNumber,
            ssrc,
            feedbackSsrc,
            sequenceCounter & 0xFFFFu);

        _rtxStats.sender.retransmissions++;

        auto* transport = _bundleTransport ? _bundleTransport.get() : _videoTransport.get();
        transport->protectAndSend(std::move(packet));
    }

    void onConnected(transport::RtcTransport* sender) override
    {
        logger::debug("client connected %s", _loggableId.c_str(), sender->getLoggableId().c_str());
        if (_callConfig.dtls && _callConfig.transportMode == TransportMode::BundledIce)
        {
            logger::debug("connecting SCTP", _loggableId.c_str());
            sender->setSctp(5000, 5000);
            sender->connectSctp();
        }
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

    void onIceReceived(transport::RtcTransport* sender, uint64_t timesetamp) override {}

    bool isRemoteVideoSsrc(uint32_t ssrc) const { return _remoteVideoSsrc.find(ssrc) != _remoteVideoSsrc.end(); }

    void stopRecording() { _recordingActive = false; }

    std::shared_ptr<transport::RtcTransport> _bundleTransport;
    std::shared_ptr<transport::RtcTransport> _audioTransport;
    std::shared_ptr<transport::RtcTransport> _videoTransport;

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

    void stopTransports()
    {
        if (_bundleTransport)
        {
            _bundleTransport->stop();
        }
        if (_audioTransport)
        {
            _audioTransport->stop();
        }
        if (_videoTransport)
        {
            _videoTransport->stop();
        }
    }

    bool hasPendingJobs() const
    {
        bool pending = false;
        if (_bundleTransport)
        {
            pending |= _bundleTransport->hasPendingJobs();
        }
        if (_audioTransport)
        {
            pending |= _audioTransport->hasPendingJobs();
        }
        if (_videoTransport)
        {
            pending |= _videoTransport->hasPendingJobs();
        }
        return pending;
    }

    bool hasTransport() const { return _bundleTransport || _audioTransport || _videoTransport; }

    bool isEndpointConfigured() const { return _configured; }

    bool isConnected() const
    {
        if (!_configured)
        {
            return false;
        }

        if (_bundleTransport && !_bundleTransport->isConnected())
        {
            return false;
        }
        if (_audioTransport && !_audioTransport->isConnected())
        {
            return false;
        }
        if (_videoTransport && !_videoTransport->isConnected())
        {
            return false;
        }

        return true;
    }

    bool hasProcessedOffer() const { return ((_bundleTransport || _audioTransport) && _audioSource); }
    void getReportSummary(std::unordered_map<uint32_t, transport::ReportSummary>& outReportSummary) const
    {
        if (_bundleTransport)
        {
            _bundleTransport->getReportSummary(outReportSummary);
        }
        if (_audioTransport)
        {
            _audioTransport->getReportSummary(outReportSummary);
        }
        if (_videoTransport)
        {
            _videoTransport->getReportSummary(outReportSummary);
        }
    }

    transport::PacketCounters getAudioReceiveCounters(uint64_t timestamp)
    {
        if (_bundleTransport)
        {
            return _bundleTransport->getAudioReceiveCounters(timestamp);
        }
        if (_audioTransport)
        {
            return _audioTransport->getAudioReceiveCounters(timestamp);
        }

        return transport::PacketCounters();
    }

    transport::PacketCounters getCumulativeVideoReceiveCounters() const
    {
        if (_bundleTransport)
        {
            return _bundleTransport->getCumulativeVideoReceiveCounters();
        }
        if (_audioTransport)
        {
            return _videoTransport->getCumulativeVideoReceiveCounters();
        }

        return transport::PacketCounters();
    }

private:
    utils::IdGenerator _idGenerator;
    uint32_t _videoSsrcs[7];
    emulator::HttpdFactory* _httpd;
    memory::PacketPoolAllocator& _allocator;
    memory::AudioPacketPoolAllocator& _audioAllocator;
    transport::TransportFactory& _transportFactory;
    transport::TransportFactory& _publicTransportFactory;
    transport::SslDtls& _sslDtls;
    concurrency::MpmcHashmap32<uint32_t, RtpAudioReceiver*> _audioReceivers;
    std::vector<std::unique_ptr<RtpVideoReceiver>> _videoReceivers;
    concurrency::MpmcHashmap32<uint32_t, RtpVideoReceiver*> _videoSsrcMap;
    logger::LoggableId _loggableId;
    std::atomic_bool _recordingActive;
    std::unordered_set<uint32_t> _remoteVideoSsrc;
    std::vector<api::SimulcastGroup> _remoteVideoStreams;
    std::unique_ptr<webrtc::WebRtcDataStream> _dataStream;
    size_t _instanceId;
    RtxStats _rtxStats;
    Audio _expectedReceiveAudioType;
    uint64_t _startTime;
    CallConfig _callConfig;
    bool _configured = false;
};

} // namespace emulator
