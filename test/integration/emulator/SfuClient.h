#pragma once

#include "AudioSource.h"
#include "memory/AudioPacketPoolAllocator.h"
#include "memory/PacketPoolAllocator.h"
#include "memory/StackArray.h"
#include "rtp/RtcpFeedback.h"
#include "rtp/RtcpHeader.h"
#include "transport/DataReceiver.h"
#include "transport/RtcTransport.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SslDtls.h"
#include "utils/IdGenerator.h"
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

template <typename ChannelType>
class SfuClient : public transport::DataReceiver
{
public:
    SfuClient(uint32_t id,
        memory::PacketPoolAllocator& allocator,
        memory::AudioPacketPoolAllocator& audioAllocator,
        transport::TransportFactory& transportFactory,
        transport::SslDtls& sslDtls,
        uint32_t ptime = 20)
        : _allocator(allocator),
          _audioAllocator(audioAllocator),
          _transportFactory(transportFactory),
          _sslDtls(sslDtls),
          _audioReceivers(256),
          _videoSsrcMap(128),
          _loggableId("client", id),
          _recordingActive(true),
          _ptime(ptime)
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
            utils::Time::nanoSleep(utils::Time::sec * 1);
        }

        for (auto& item : _audioReceivers)
        {
            delete item.second;
        }
    }

    void initiateCall(const std::string& baseUrl,
        std::string conferenceId,
        bool initiator,
        bool audio,
        bool video,
        bool forwardMedia)
    {
        _channel.create(baseUrl, conferenceId, initiator, audio, video, forwardMedia);
        logger::info("client started %s", _loggableId.c_str(), _channel.getEndpointId().c_str());
    }

    void processOffer()
    {
        auto offer = _channel.getOffer();
        _transport =
            _transportFactory.createOnPrivatePort(ice::IceRole::CONTROLLED, 256 * 1024, _channel.getEndpointIdHash());
        _transport->setDataReceiver(this);

        _channel.configureTransport(*_transport, _audioAllocator);

        if (_channel.isAudioOffered())
        {
            _audioSource = std::make_unique<emulator::AudioSource>(_allocator, _idGenerator.next());
            _transport->setAudioPayloadType(111, codec::Opus::sampleRate);
        }

        _remoteVideoSsrc = _channel.getOfferedVideoSsrcs();
    }

    void connect()
    {
        uint32_t videoSsrcs[7];
        if (_channel.isVideoEnabled())
        {
            videoSsrcs[6] = 0;
            for (int i = 0; i < 6; ++i)
            {
                videoSsrcs[i] = _idGenerator.next();
                if (0 == i % 2)
                {
                    _videoSources.emplace(videoSsrcs[i],
                        std::make_unique<fakenet::FakeVideoSource>(_allocator, 1024, videoSsrcs[i]));
                }
            }
        }

        assert(_audioSource);

        _channel.sendResponse(_transport->getLocalCredentials(),
            _transport->getLocalCandidates(),
            _sslDtls.getLocalFingerprint(),
            _audioSource->getSsrc(),
            _channel.isVideoEnabled() ? videoSsrcs : nullptr);

        _dataStream = std::make_unique<webrtc::WebRtcDataStream>(_loggableId.getInstanceId(), *_transport);
        _transport->start();
        _transport->connect();
    }

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
                auto packet = videoSource.second->getPacket(timestamp);
                if (packet)
                {
                    _transport->getJobQueue().addJob<MediaSendJob>(*_transport, std::move(packet), timestamp);
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
            uint64_t timestamp)
            : _rtpMap(rtpMap),
              _context(ssrc, _rtpMap, transport, timestamp),
              _loggableId("rtprcv", instanceId)
        {
            _recording.reserve(256 * 1024);
        }

        void onRtpPacketReceived(transport::RtcTransport* sender,
            memory::Packet& packet,
            uint32_t extendedSequenceNumber,
            uint64_t timestamp)
        {
            _context.onRtpPacket(timestamp);
            if (!sender->unprotect(packet))
            {
                return;
            }

            auto rtpHeader = rtp::RtpHeader::fromPacket(packet);
            addOpus(reinterpret_cast<unsigned char*>(rtpHeader->getPayload()),
                packet.getLength() - rtpHeader->headerLength(),
                extendedSequenceNumber);
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
            fileName.append(_loggableId.c_str()).append("-").append(_context._ssrc);

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
    };

    class RtpVideoReceiver
    {
    public:
        RtpVideoReceiver(size_t instanceId,
            uint32_t ssrc,
            const bridge::RtpMap& rtpMap,
            const bridge::RtpMap& rtpMapFeedback,
            transport::RtcTransport* transport,
            uint64_t timestamp)
            : _rtpMap(rtpMap),
              _rtpMapFeedback(rtpMapFeedback),
              _context(ssrc, _rtpMap, transport, timestamp),
              _loggableId("rtprcv", instanceId)
        {
            _recording.reserve(256 * 1024);
        }

        void onRtpPacketReceived(transport::RtcTransport* sender,
            memory::Packet& packet,
            uint32_t extendedSequenceNumber,
            uint64_t timestamp)
        {
            _context.onRtpPacket(timestamp);
            if (!sender->unprotect(packet))
            {
                return;
            }

            auto rtpHeader = rtp::RtpHeader::fromPacket(packet);
            if (rtpHeader->payloadType == _rtpMap._payloadType)
            {
                ++videoPacketCount;
            }
            else
            {
                ++rtxPacketCount;
            }
        }

        const logger::LoggableId& getLoggableId() const { return _loggableId; }

        size_t videoPacketCount = 0;
        size_t rtxPacketCount = 0;

    private:
        bridge::RtpMap _rtpMap;
        bridge::RtpMap _rtpMapFeedback;
        bridge::SsrcInboundContext _context;
        codec::OpusDecoder _decoder;
        logger::LoggableId _loggableId;
        std::vector<int16_t> _recording;
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
                rtpMap._audioLevelExtId.set(1);
                rtpMap._c9infoExtId.set(8);
                _audioReceivers.emplace(rtpHeader->ssrc.get(),
                    new RtpAudioReceiver(_loggableId.getInstanceId(),
                        rtpHeader->ssrc.get(),
                        rtpMap,
                        sender,
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
                return;
            }

            it->second->onRtpPacketReceived(sender, *packet, extendedSequenceNumber, timestamp);
            /*
            _videoReceivers.emplace(rtpHeader->ssrc.get(),
                new RtpVideoReceiver(_loggableId.getInstanceId(),
                    rtpHeader->ssrc.get(),
                    rtpMap,
                    fbMap,
                    sender,
                    timestamp));
            it = _videoReceivers.find(rtpHeader->ssrc.get());
        }
*/
            if ((extendedSequenceNumber % 100) == 0)
            {
                logger::debug("%s ssrc %u received seq %u, audio receiver count %zu",
                    _loggableId.c_str(),
                    _channel.getEndpointId().c_str(),
                    rtpHeader->ssrc.get(),
                    extendedSequenceNumber,
                    _audioReceivers.size());
            }
        }
    }

    void onRtcpPacketDecoded(transport::RtcTransport* sender, memory::UniquePacket packet, uint64_t timestamp) override
    {
        rtp::CompoundRtcpPacket compoundPacket(packet->get(), packet->getLength());
        for (const auto& rtcpPacket : compoundPacket)
        {
            auto rtcpFeedback = reinterpret_cast<const rtp::RtcpFeedback*>(&rtcpPacket);
            if (rtcpPacket.packetType == rtp::RtcpPacketType::PAYLOADSPECIFIC_FB &&
                rtcpFeedback->_header.fmtCount == rtp::PayloadSpecificFeedbackType::Pli)
            {
                logger::debug("PLI for %u", _loggableId.c_str(), rtcpFeedback->_mediaSsrc.get());
                auto it = _videoSources.find(rtcpFeedback->_mediaSsrc.get());
                if (it != _videoSources.end())
                {
                    auto& videoSource = it->second;
                    if (videoSource->getSsrc() == rtcpFeedback->_mediaSsrc.get())
                    {
                        videoSource->requestKeyFrame();
                    }
                }
                else
                {
                    logger::warn("cannot find video ssrc for PLI %u",
                        _loggableId.c_str(),
                        rtcpFeedback->_mediaSsrc.get());
                    for (auto& it : _videoSources)
                    {
                        logger::debug("vsssrc %u", _loggableId.c_str(), it.second->getSsrc());
                    }
                }
            }
        }
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

    const concurrency::MpmcHashmap32<uint32_t, RtpAudioReceiver*>& getReceiveStats() const { return _audioReceivers; }
    const logger::LoggableId& getLoggableId() const { return _loggableId; }

private:
    utils::IdGenerator _idGenerator;
    memory::PacketPoolAllocator& _allocator;
    memory::AudioPacketPoolAllocator& _audioAllocator;
    transport::TransportFactory& _transportFactory;
    transport::SslDtls& _sslDtls;
    concurrency::MpmcHashmap32<uint32_t, RtpAudioReceiver*> _audioReceivers;
    memory::StackArray<std::unique_ptr<RtpVideoReceiver>, 32> _videoReceivers;
    concurrency::MpmcHashmap32<uint32_t, RtpVideoReceiver*> _videoSsrcMap;
    logger::LoggableId _loggableId;
    std::atomic_bool _recordingActive;
    std::unordered_set<uint32_t> _remoteVideoSsrc;
    uint32_t _ptime;
    std::unique_ptr<webrtc::WebRtcDataStream> _dataStream;
};

template <typename T>
class GroupCall
{
public:
    GroupCall(std::initializer_list<T*> clients) : _clients(clients) {}

    bool connect(uint64_t timeout)
    {
        auto start = utils::Time::getAbsoluteTime();
        for (auto client : _clients)
        {
            if (!client->_channel.isSuccess())
            {
                return false;
            }
        }

        for (auto client : _clients)
        {
            client->processOffer();
            if (!client->_transport || !client->_audioSource)
            {
                return false;
            }
        }

        for (auto client : _clients)
        {
            client->connect();
        }

        while (utils::Time::getAbsoluteTime() - start < timeout)
        {
            auto it =
                std::find_if_not(_clients.begin(), _clients.end(), [](auto c) { return c->_transport->isConnected(); });

            if (it == _clients.end())
            {
                return true;
            }

            utils::Time::nanoSleep(1 * utils::Time::sec);
            logger::debug("waiting for connect...", "test");
        }

        return false;
    }

    void run(uint64_t period)
    {
        const auto start = utils::Time::getAbsoluteTime();
        utils::Pacer pacer(10 * utils::Time::ms);
        for (auto timestamp = utils::Time::getAbsoluteTime(); timestamp - start < period;)
        {
            for (auto client : _clients)
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
            for (auto client : _clients)
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

    std::vector<T*> _clients;
};
} // namespace emulator
