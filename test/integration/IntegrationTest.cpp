#include "test/integration/IntegrationTest.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "codec/Opus.h"
#include "codec/OpusDecoder.h"
#include "concurrency/MpmcHashmap.h"
#include "external/http.h"
#include "jobmanager/JobManager.h"
#include "jobmanager/WorkerThread.h"
#include "memory/PacketPoolAllocator.h"
#include "nlohmann/json.hpp"
#include "test/integration/SampleDataUtils.h"
#include "test/integration/emulator/AudioSource.h"
#include "transport/DataReceiver.h"
#include "transport/RtcTransport.h"
#include "transport/RtcePoll.h"
#include "transport/Transport.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SslDtls.h"
#include "utils/IdGenerator.h"
#include "utils/StringBuilder.h"
#include <complex>
#include <sstream>

IntegrationTest::IntegrationTest()
    : _sendAllocator(memory::packetPoolSize, "IntegrationTest"),
      _jobManager(std::make_unique<jobmanager::JobManager>()),
      _mainPoolAllocator(std::make_unique<memory::PacketPoolAllocator>(4096, "testMain")),
      _sslDtls(nullptr),
      _network(transport::createRtcePoll()),
      _pacer(10 * 1000000),
      _instanceCounter(0)
{
    for (size_t threadIndex = 0; threadIndex < std::thread::hardware_concurrency(); ++threadIndex)
    {
        _workerThreads.push_back(std::make_unique<jobmanager::WorkerThread>(*_jobManager));
    }
}

void IntegrationTest::SetUp()
{
    using namespace std;

    utils::Time::initialize();
}

void IntegrationTest::initBridge(config::Config& config)
{
    _bridge = std::make_unique<bridge::Bridge>(config);
    _bridge->initialize();

    _sslDtls = &_bridge->getSslDtls();
    _srtpClientFactory = std::make_unique<transport::SrtpClientFactory>(*_sslDtls, *_mainPoolAllocator);

    std::string configJson =
        "{\"ice.preferredIp\": \"127.0.0.1\", \"ice.singlePort\":10050, \"recording.singlePort\":0}";
    _config.readFromString(configJson);
    std::vector<transport::SocketAddress> interfaces;
    interfaces.push_back(transport::SocketAddress::parse(_config.ice.preferredIp, 0));

    _transportFactory = transport::createTransportFactory(*_jobManager,
        *_srtpClientFactory,
        _config,
        _sctpConfig,
        _iceConfig,
        _bweConfig,
        _rateControlConfig,
        interfaces,
        *_network,
        *_mainPoolAllocator);
}

void IntegrationTest::TearDown()
{
    _bridge.reset();

    _transportFactory.reset();
    _jobManager->stop();
    for (auto& worker : _workerThreads)
    {
        worker->stop();
    }

    logger::info("IntegrationTest torn down", "IntegrationTest");
}

namespace
{
std::string newGuuid()
{
    utils::IdGenerator idGen;
    char uuid[200];

    sprintf(uuid,
        "%08x-%04x-%04x-%04x-%012x",
        static_cast<uint32_t>(idGen.next() & 0xFFFFFFFFu),
        static_cast<uint32_t>(idGen.next() & 0xFFFFu),
        static_cast<uint32_t>(idGen.next() & 0xFFFFu),
        static_cast<uint32_t>(idGen.next() & 0xFFFFu),
        static_cast<uint32_t>(idGen.next()));

    return uuid;
}

std::string newIdString()
{
    utils::IdGenerator idGen;
    char uuid[200];

    sprintf(uuid, "%08u", static_cast<uint32_t>(idGen.next() & 0xFFFFFFFFu));

    return uuid;
}
} // namespace

class HttpPostRequest
{
public:
    HttpPostRequest(const char* url, const char* body) : _request(nullptr), _status(HTTP_STATUS_PENDING), _prevSize(0)
    {
        _request = http_post(url, body, body ? std::strlen(body) : 0, nullptr);
    }

    ~HttpPostRequest() { http_release(_request); }

    void awaitResponse(uint64_t timeout)
    {
        const auto startTime = utils::Time::getAbsoluteTime();

        while (_status == HTTP_STATUS_PENDING)
        {
            _status = http_process(_request);
            if (_prevSize != _request->response_size)
            {
                logger::debug("%zu byte(s) received.", "", _request->response_size);
                _prevSize = _request->response_size;
            }
            if (utils::Time::getAbsoluteTime() - startTime > timeout)
            {
                logger::error("Timeout waiting for response", "AwsHarvester");
                _status = HTTP_STATUS_FAILED;
                break;
            }
            utils::Time::nanoSleep(2 * utils::Time::ms);
        }
    }

    bool isPending() const { return _status == HTTP_STATUS_PENDING; }
    bool hasFailed() const { return _status == HTTP_STATUS_FAILED; }
    bool isSuccess() const { return _status == HTTP_STATUS_COMPLETED; }

    std::string getResponse() const
    {
        if (isSuccess())
        {
            return (char const*)_request->response_data;
        }
        return "";
    }

    nlohmann::json getJsonBody() const
    {
        if (isSuccess())
        {
            return nlohmann::json::parse(static_cast<const char*>(_request->response_data));
        }
        return nlohmann::json();
    }

    int getCode() const { return _request->status_code; }

protected:
    HttpPostRequest() : _request(nullptr), _status(HTTP_STATUS_PENDING), _prevSize(0) {}
    http_t* _request;

private:
    http_status_t _status;
    size_t _prevSize;
};

class HttpPatchRequest : public HttpPostRequest
{
public:
    HttpPatchRequest(const char* url, const char* body)
    {
        _request = http_patch(url, body, body ? std::strlen(body) : 0, nullptr);
    }
};

class HttpGetRequest : public HttpPostRequest
{
public:
    HttpGetRequest(const char* url) { _request = http_get(url, nullptr); }
};

class Conference
{
public:
    void create(const char* baseUrl)
    {
        HttpPostRequest request((std::string(baseUrl) + "/colibri/conferences").c_str(), "{\"last-n\":9}");
        request.awaitResponse(3000 * utils::Time::ms);

        if (request.isSuccess())
        {
            auto body = request.getJsonBody();
            _id = body["id"].get<std::string>();
            _success = true;
        }
    }

    const std::string& getId() const { return _id; }

    bool isSuccess() const { return _success; }

private:
    std::string _id;
    bool _success = false;
};

nlohmann::json newContent(const std::string& endpointId, const char* type, const char* relayType, bool initiator)
{
    using namespace nlohmann;
    json contentItem = json::object({{"name", type},
        {"channels",
            json::array({json::object({{"initiator", initiator},
                {"endpoint", endpointId},
                {"expire", 60},
                {"direction", "sendrecv"},
                {"channel-bundle-id", endpointId},
                {"rtp-level-relay-type", relayType}})})}});

    return contentItem;
}
class Channel
{
public:
    Channel() : _id(newGuuid()), _audioId(newIdString()), _dataId(newIdString()), _videoId(newIdString()) {}

    void create(const char* baseUrl,
        std::string conferenceId,
        bool initiator,
        bool audio,
        bool video,
        std::string relayType)
    {
        _conferenceId = conferenceId;
        _relayType = relayType;
        _baseUrl = baseUrl;

        using namespace nlohmann;
        json body = {{"id", conferenceId}, {"contents", json::array()}};

        if (audio)
        {
            body["contents"].push_back(newContent(_id, "audio", relayType.c_str(), initiator));
        }
        if (video)
        {
            auto videoContent = newContent(_id, "video", relayType.c_str(), initiator);
            videoContent["channels"][0]["last-n"] = 5;
            body["contents"].push_back(videoContent);
        }
        if (audio || video)
        {
            // data must be last or request handler cannot decide if bundling is used
            body["contents"].push_back(json::object({{"name", "data"},
                {"sctpconnections",
                    json::array({json::object({{"initiator", initiator},
                        {"endpoint", _id},
                        {"expire", 60},
                        {"channel-bundle-id", _id}})})}}));
        }

        logger::debug("allocate ch with %s", "", body.dump().c_str());
        HttpPatchRequest request((std::string(baseUrl) + "/colibri/conferences/" + conferenceId).c_str(),
            body.dump().c_str());
        request.awaitResponse(90000 * utils::Time::ms);

        if (request.isSuccess())
        {
            _offer = request.getJsonBody();
            raw = request.getResponse();
        }
        else
        {
            logger::error("failed to allocate channel %d", "Test", request.getCode());
        }
    }

    void sendResponse(const std::pair<std::string, std::string>& iceCredentials,
        const ice::IceCandidates& candidates,
        const std::string& fingerprint,
        uint32_t audioSsrc,
        uint32_t* videoSsrcs)
    {
        using namespace nlohmann;
        json body = {{"id", _conferenceId},
            {"contents", json::array()},
            {"channel-bundles", json::array({json::object({{"id", _id}})})}};

        auto transportSpec = json::object({{"xmlns", "urn:xmpp:jingle:transports:ice-udp:1"},
            {"rtcp-mux", true},
            {"ufrag", iceCredentials.first},
            {"pwd", iceCredentials.second},
            {"fingerprints", json::array({{{"setup", "active"}, {"hash", "sha-256"}, {"fingerprint", fingerprint}}})},
            {"candidates", json::array()}});

        for (auto& c : candidates)
        {
            auto jsonCandidate = json::object({{"foundation", c.getFoundation()},
                {"component", c.component},
                {"protocol", c.transportType == ice::TransportType::UDP ? "udp" : "tcp"},
                {"priority", c.priority},
                {"ip", c.address.ipToString()},
                {"port", c.address.getPort()},
                {"type", "host"},
                {"generation", 0},
                {"network", 1}});

            transportSpec["candidates"].push_back(jsonCandidate);
        }

        body["channel-bundles"][0]["transport"] = transportSpec;

        if (audioSsrc != 0)
        {
            auto audioContent = json::object({{"name", "audio"},
                {"channels",
                    json::array({json::object({{"endpoint", _id},
                        {"expire", 180},
                        {"id", _audioId},
                        {"channel-bundle-id", _id},
                        {"rtp-level-relay-type", _relayType},
                        {"direction", "sendrecv"},
                        {"rtp-hdrexts",
                            json::array({{{"id", 1}, {"uri", "urn:ietf:params:rtp-hdrext:ssrc-audio-level"}},
                                {{"id", 3},
                                    {"uri", "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"}}})}})})}});

            auto payloadTypesJson = json::array();
            payloadTypesJson.push_back(json::object({{"id", 111},
                {"parameters", {{"minptime", "10"}}},
                {"rtcp-fbs", json::array()},
                {"name", "opus"},
                {"clockrate", "48000"},
                {"channels", "2"}}));

            payloadTypesJson.push_back(json::object({{"id", 0},
                {"parameters", json::object()},
                {"rtcp-fbs", json::array()},
                {"name", "PCMU"},
                {"clockrate", "8000"}}));

            payloadTypesJson.push_back(json::object({{"id", 8},
                {"parameters", json::object()},
                {"rtcp-fbs", json::array()},
                {"name", "PCMA"},
                {"clockrate", "8000"}}));

            audioContent["channels"][0]["payload-types"] = payloadTypesJson;
            audioContent["channels"][0]["sources"] = json::array({audioSsrc});

            body["contents"].push_back(audioContent);
        }

        if (videoSsrcs)
        {
            auto videoContent = json::object({{"name", "video"}, {"channels", json::array()}});
            auto videoChannel = json::object({{"endpoint", _id},
                {"expire", 180},
                {"id", _videoId},
                {"channel-bundle-id", _id},
                {"rtp-level-relay-type", _relayType},
                {"direction", "sendrecv"},
                {"rtp-hdrexts",
                    json::array({{{"id", 3}, {"uri", "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"}},
                        {{"id", 4}, {"uri", "urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"}}})}});
            auto payloadTypes = json::array();
            payloadTypes.push_back(json::object({{"id", 100},
                {"name", "VP8"},
                {"clockrate", "90000"},
                {"parameters", json::object()},
                {"rtcp-fbs",
                    json::array({{{"type", "goog-remb"}},
                        {{"type", "ccm"}, {"subtype", "fir"}},
                        {{"type", "nack"}},
                        {{"type", "nack"}, {"subtype", "pli"}}})}}));

            payloadTypes.push_back(json::object({{"id", 96},
                {"name", "rtx"},
                {"clockrate", "90000"},
                {"rtcp-fbs", json::array()},
                {"parameters", {{"apt", "100"}}}}));

            videoChannel["payload-types"] = payloadTypes;

            auto sources = json::array();
            for (uint32_t* pSsrc = videoSsrcs; *pSsrc != 0; ++pSsrc)
            {
                sources.push_back(*pSsrc);
            }
            videoChannel["sources"] = sources;

            auto ssrcGroups = json::array();
            auto mainSsrcs = json::array();
            for (uint32_t* pSsrc = videoSsrcs; *pSsrc != 0; pSsrc += 2)
            {
                mainSsrcs.push_back(*pSsrc);
                ssrcGroups.push_back(
                    json::object({{"sources", json::array({*pSsrc, *(pSsrc + 1)})}, {"semantics", "FID"}}));
            }
            ssrcGroups.push_back(json::object({{"semantics", "SIM"}, {"sources", mainSsrcs}}));
            videoChannel["ssrc-groups"] = ssrcGroups;

            videoContent["channels"].push_back(videoChannel);
            body["contents"].push_back(videoContent);
        }

        auto dataJson = json::object({{"name", "data"},
            {"sctpconnections",
                json::array({json::object({{"endpoint", _id},
                    {"expire", 180},
                    {"id", _dataId},
                    {"channel-bundle-id", _id},
                    {"port", "5000"},
                    {"direction", "sendrecv"}})})}});
        body["contents"].push_back(dataJson);

        HttpPatchRequest request((_baseUrl + "/colibri/conferences/" + _conferenceId).c_str(), body.dump().c_str());
        request.awaitResponse(3000 * utils::Time::ms);

        if (request.isSuccess())
        {
            raw = request.getResponse();
        }
        else
        {
            logger::error("failed to patch channel %d", "Test", request.getCode());
        }
    }

    bool isSuccess() const { return !raw.empty(); }

    nlohmann::json getOffer() const { return _offer; }
    std::string getEndpointId() const { return _id; }
    uint32_t getEndpointIdHash() const { return std::hash<std::string>{}(_id); }
    std::string raw;

private:
    std::string _id;
    std::string _conferenceId;

    std::string _audioId;
    std::string _dataId;
    std::string _videoId;
    std::string _relayType;
    nlohmann::json _offer;
    std::string _baseUrl;
};

class AudioSendJob : public jobmanager::Job
{
public:
    AudioSendJob(transport::Transport& transport,
        memory::Packet* packet,
        emulator::AudioSource& source,
        uint64_t timestamp)
        : _transport(transport),
          _packet(packet),
          _source(source)
    {
    }

    ~AudioSendJob()
    {
        if (_packet)
        {
            _source.getAllocator().free(_packet);
        }
    }

    void run() override
    {
        _transport.protectAndSend(_packet, _source.getAllocator());
        _packet = nullptr;
    }

private:
    transport::Transport& _transport;
    memory::Packet* _packet;
    emulator::AudioSource& _source;
};

class SfuClient : public transport::DataReceiver
{
public:
    SfuClient(uint32_t id,
        memory::PacketPoolAllocator& allocator,
        transport::TransportFactory& transportFactory,
        transport::SslDtls& sslDtls)
        : _allocator(allocator),
          _transportFactory(transportFactory),
          _sslDtls(sslDtls),
          _receivedData(256),
          _loggableId("client", id)
    {
    }

    ~SfuClient()
    {
        for (auto& item : _receivedData)
        {
            delete item.second;
        }
    }

    void initiateCall(const char* baseUrl,
        std::string conferenceId,
        bool initiator,
        bool audio,
        bool video,
        std::string relayType)
    {
        _channel.create(baseUrl, conferenceId, initiator, audio, video, relayType);
    }

    void processOffer()
    {
        auto offer = _channel.getOffer();
        _transport =
            _transportFactory.createOnPrivatePort(ice::IceRole::CONTROLLED, 256 * 1024, _channel.getEndpointIdHash());
        _transport->setDataReceiver(this);

        for (auto& bundle : offer["channel-bundles"])
        {
            ice::IceCandidates candidates;
            for (auto& c : bundle["transport"]["candidates"])
            {
                candidates.push_back(ice::IceCandidate(c["foundation"].get<std::string>().c_str(),
                    ice::IceComponent::RTP,
                    c["protocol"] == "udp" ? ice::TransportType::UDP : ice::TransportType::TCP,
                    c["priority"].get<uint32_t>(),
                    transport::SocketAddress::parse(c["ip"], c["port"]),
                    ice::IceCandidate::Type::HOST));
            }

            std::pair<std::string, std::string> credentials;
            credentials.first = bundle["transport"]["ufrag"];
            credentials.second = bundle["transport"]["pwd"];

            _transport->setRemoteIce(credentials, candidates, _allocator);

            std::string fingerPrint = bundle["transport"]["fingerprints"][0]["fingerprint"];
            _transport->setRemoteDtlsFingerprint(bundle["transport"]["fingerprints"][0]["hash"], fingerPrint, true);
        }

        for (auto& content : offer["contents"])
        {
            if (content["name"] == "audio")
            {
                _audioSource = std::make_unique<emulator::AudioSource>(_allocator, _idGenerator.next());
                _transport->setAudioPayloadType(111, codec::Opus::sampleRate);
            }
        }
    }

    void connect()
    {
        uint32_t videoSsrcs[7];
        videoSsrcs[6] = 0;
        for (int i = 0; i < 6; ++i)
        {
            videoSsrcs[i] = _idGenerator.next();
        }

        _channel.sendResponse(_transport->getLocalCredentials(),
            _transport->getLocalCandidates(),
            _sslDtls.getLocalFingerprint(),
            _audioSource->getSsrc(),
            videoSsrcs);

        _transport->start();
        _transport->connect();
    }

    void process(uint64_t timestamp)
    {
        auto* packet = _audioSource->getPacket(timestamp);
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
            _transport->getJobQueue().addJob<AudioSendJob>(*_transport, packet, *_audioSource, timestamp);
        }
    }

    Channel _channel;

    class RtpReceiver
    {
    public:
        RtpReceiver(size_t instanceId,
            uint32_t ssrc,
            const bridge::RtpMap& rtpMap,
            transport::RtcTransport* transport,
            uint64_t timestamp)
            : _rtpMap(rtpMap),
              _context(ssrc, _rtpMap, 1, transport, timestamp),
              _loggableId("rtprcv", instanceId)
        {
            _recording.reserve(256 * 1024);
        }

        void onRtpPacketReceived(transport::RtcTransport* sender,
            memory::Packet* packet,
            uint32_t extendedSequenceNumber,
            uint64_t timestamp)
        {
            _context.onRtpPacket(timestamp);
            if (!sender->unprotect(packet))
            {
                return;
            }

            auto rtpHeader = rtp::RtpHeader::fromPacket(*packet);
            addOpus(reinterpret_cast<unsigned char*>(rtpHeader->getPayload()),
                packet->getLength() - rtpHeader->headerLength(),
                extendedSequenceNumber);
        }

        void addOpus(unsigned char* opusData, int32_t payloadLength, uint32_t extendedSequenceNumber)
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

public:
    void onRtpPacketReceived(transport::RtcTransport* sender,
        memory::Packet* packet,
        memory::PacketPoolAllocator& receiveAllocator,
        uint32_t extendedSequenceNumber,
        uint64_t timestamp) override
    {
        auto rtpHeader = rtp::RtpHeader::fromPacket(*packet);

        auto it = _receivedData.find(rtpHeader->ssrc.get());
        if (it == _receivedData.end())
        {
            _receivedData.emplace(rtpHeader->ssrc.get(),
                new RtpReceiver(_loggableId.getInstanceId(),
                    rtpHeader->ssrc.get(),
                    bridge::RtpMap(bridge::RtpMap::Format::OPUS, 111, codec::Opus::sampleRate),
                    sender,
                    timestamp));
            it = _receivedData.find(rtpHeader->ssrc.get());
        }

        if (it != _receivedData.end())
        {
            if (rtpHeader->payloadType == 111)
            {
                it->second->onRtpPacketReceived(sender, packet, extendedSequenceNumber, timestamp);
            }
        }

        if ((extendedSequenceNumber % 500) == 0)
        {
            logger::debug("%s pkt %u received %u, ssrc count %zu",
                _loggableId.c_str(),
                _channel.getEndpointId().c_str(),
                rtpHeader->ssrc.get(),
                extendedSequenceNumber,
                _receivedData.size());
        }
        receiveAllocator.free(packet);
    }

    void onRtcpPacketDecoded(transport::RtcTransport* sender,
        memory::Packet* packet,
        memory::PacketPoolAllocator& receiveAllocator,
        uint64_t timestamp) override
    {
        receiveAllocator.free(packet);
    }

    void onConnected(transport::RtcTransport* sender) override
    {
        logger::debug("client connected", _loggableId.c_str());
    }

    bool onSctpConnectionRequest(transport::RtcTransport* sender, uint16_t remotePort) override { return false; }
    void onSctpEstablished(transport::RtcTransport* sender) override {}
    void onSctpMessage(transport::RtcTransport* sender,
        uint16_t streamId,
        uint16_t streamSequenceNumber,
        uint32_t payloadProtocol,
        const void* data,
        size_t length) override
    {
        logger::debug("sctp message", "");
    }

    void onRecControlReceived(transport::RecordingTransport* sender,
        memory::Packet* packet,
        memory::PacketPoolAllocator& receiveAllocator,
        uint64_t timestamp) override
    {
    }

    std::shared_ptr<transport::RtcTransport> _transport;

    std::unique_ptr<emulator::AudioSource> _audioSource;
    // Video source that produces fake VP8

    const concurrency::MpmcHashmap32<uint32_t, RtpReceiver*>& getReceiveStats() const { return _receivedData; }
    const logger::LoggableId& getLoggableId() const { return _loggableId; }

private:
    utils::IdGenerator _idGenerator;
    memory::PacketPoolAllocator& _allocator;
    transport::TransportFactory& _transportFactory;
    transport::SslDtls& _sslDtls;
    concurrency::MpmcHashmap32<uint32_t, RtpReceiver*> _receivedData;
    logger::LoggableId _loggableId;
};

namespace
{
void analyzeRecording(const std::vector<int16_t>& recording,
    std::vector<double>& frequencyPeaks,
    std::vector<std::pair<uint64_t, double>>& amplitudeProfile,
    const char* logId)
{
    utils::RateTracker<5> amplitudeTracker(codec::Opus::sampleRate / 10);
    size_t analysisWindowSize = 0;
    const size_t fftWindowSize = 2048;
    std::valarray<std::complex<double>> testVector(fftWindowSize);

    for (size_t t = 0; t < recording.size(); ++t)
    {
        amplitudeTracker.update(std::abs(recording[t]), t);
        if (t > codec::Opus::sampleRate / 10)
        {
            if (amplitudeProfile.empty() ||
                (t - amplitudeProfile.back().first > codec::Opus::sampleRate / 10 &&
                    std::abs(amplitudeProfile.back().second - amplitudeTracker.get(t, codec::Opus::sampleRate / 5)) >
                        100))
            {
                amplitudeProfile.push_back(std::make_pair(t, amplitudeTracker.get(t, codec::Opus::sampleRate / 5)));
            }
        }
    }

    for (size_t cursor = 0; cursor < recording.size() - fftWindowSize; cursor += 256)
    {
        for (uint64_t x = 0; x < fftWindowSize; ++x)
        {
            testVector[x] = std::complex<double>(static_cast<double>(recording[x + cursor]), 0.0) / (256.0 * 128);
        }
        analysisWindowSize = cursor + fftWindowSize;

        SampleDataUtils::fft(testVector);

        std::vector<double> frequencies;
        SampleDataUtils::listFrequencies(testVector, codec::Opus::sampleRate, frequencies);
        for (size_t i = 0; i < frequencies.size() && i < 50; ++i)
        {
            if (std::find(frequencyPeaks.begin(), frequencyPeaks.end(), frequencies[i]) == frequencyPeaks.end())
            {
                logger::debug("added new freq %.3f", logId, frequencies[i]);
                frequencyPeaks.push_back(frequencies[i]);
            }
        }
    }
}
} // namespace

TEST_F(IntegrationTest, plain)
{
#if !ENABLE_LEGACY_API
    return;
#endif

    _config.readFromString("{\"ip\":\"127.0.0.1\", "
                           "\"ice.preferredIp\":\"127.0.0.1\",\"ice.publicIpv4\":\"127.0.0.1\"}");
    initBridge(_config);

    const auto baseUrl = "http://127.0.0.1:8080";

    Conference conf;
    conf.create(baseUrl);
    EXPECT_TRUE(conf.isSuccess());
    utils::Time::nanoSleep(1 * utils::Time::sec);

    SfuClient client1(++_instanceCounter, *_mainPoolAllocator, *_transportFactory, *_sslDtls);
    SfuClient client2(++_instanceCounter, *_mainPoolAllocator, *_transportFactory, *_sslDtls);
    SfuClient client3(++_instanceCounter, *_mainPoolAllocator, *_transportFactory, *_sslDtls);

    client1.initiateCall(baseUrl, conf.getId(), true, true, true, "ssrc-rewrite");
    client2.initiateCall(baseUrl, conf.getId(), false, true, true, "ssrc-rewrite");
    client3.initiateCall(baseUrl, conf.getId(), false, true, true, "mixer");

    EXPECT_TRUE(client1._channel.isSuccess());
    EXPECT_TRUE(client2._channel.isSuccess());
    EXPECT_TRUE(client3._channel.isSuccess());

    if (!client1._channel.isSuccess() && client2._channel.isSuccess() && client3._channel.isSuccess())
    {
        return;
    }

    client1.processOffer();
    client2.processOffer();
    client3.processOffer();

    client1.connect();
    client2.connect();
    client3.connect();

    while (
        !client1._transport->isConnected() || !client2._transport->isConnected() || !client3._transport->isConnected())
    {
        utils::Time::nanoSleep(1 * utils::Time::sec);
        logger::debug("waiting for connect...", "test");
    }

    client1._audioSource->setFrequency(600);
    client2._audioSource->setFrequency(1300);
    client3._audioSource->setFrequency(2100);

    client1._audioSource->setVolume(0.6);
    client2._audioSource->setVolume(0.6);
    client3._audioSource->setVolume(0.6);

    utils::Pacer pacer(10 * utils::Time::ms);
    for (int i = 0; i < 500; ++i)
    {
        const auto timestamp = utils::Time::getAbsoluteTime();
        client1.process(timestamp);
        client2.process(timestamp);
        client3.process(timestamp);
        pacer.tick(utils::Time::getAbsoluteTime());
        utils::Time::nanoSleep(pacer.timeToNextTick(utils::Time::getAbsoluteTime()));
    }
    client3._transport->stop();
    HttpGetRequest statsRequest((std::string(baseUrl) + "/colibri/stats").c_str());
    statsRequest.awaitResponse(1500 * utils::Time::ms);
    EXPECT_TRUE(statsRequest.isSuccess());
    HttpGetRequest confRequest((std::string(baseUrl) + "/colibri/conferences").c_str());
    confRequest.awaitResponse(500 * utils::Time::ms);
    EXPECT_TRUE(confRequest.isSuccess());

    client1._transport->stop();
    client2._transport->stop();

    for (int i = 0; i < 10 &&
         (client1._transport->hasPendingJobs() || client2._transport->hasPendingJobs() ||
             client3._transport->hasPendingJobs());
         ++i)
    {
        utils::Time::nanoSleep(1 * utils::Time::sec);
    }

    {
        auto audioCounters = client1._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
        EXPECT_EQ(audioCounters.lostPackets, 0);
        const auto& rData1 = client1.getReceiveStats();
        std::vector<double> allFreq;
        for (const auto& item : rData1)
        {
            std::vector<double> freqVector;
            std::vector<std::pair<uint64_t, double>> amplitudeProfile;
            auto rec = item.second->getRecording();
            analyzeRecording(rec, freqVector, amplitudeProfile, item.second->getLoggableId().c_str());
            EXPECT_NEAR(rec.size(), 5 * codec::Opus::sampleRate, 100);
            EXPECT_EQ(freqVector.size(), 1);
            allFreq.insert(allFreq.begin(), freqVector.begin(), freqVector.end());

            EXPECT_EQ(amplitudeProfile.size(), 2);
            if (amplitudeProfile.size() > 1)
            {
                EXPECT_NEAR(amplitudeProfile[1].second, 5725, 100);
            }

            // item.second->dumpPcmData();
        }

        std::sort(allFreq.begin(), allFreq.end());
        EXPECT_NEAR(allFreq[0], 1300.0, 25.0);
        EXPECT_NEAR(allFreq[1], 2100.0, 25.0);
    }
    {
        auto audioCounters = client2._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
        EXPECT_EQ(audioCounters.lostPackets, 0);

        const auto& rData1 = client2.getReceiveStats();
        std::vector<double> allFreq;
        for (const auto& item : rData1)
        {
            std::vector<double> freqVector;
            std::vector<std::pair<uint64_t, double>> amplitudeProfile;
            auto rec = item.second->getRecording();
            analyzeRecording(rec, freqVector, amplitudeProfile, item.second->getLoggableId().c_str());
            EXPECT_NEAR(rec.size(), 5 * codec::Opus::sampleRate, 100);
            EXPECT_EQ(freqVector.size(), 1);
            allFreq.insert(allFreq.begin(), freqVector.begin(), freqVector.end());

            EXPECT_EQ(amplitudeProfile.size(), 2);
            if (amplitudeProfile.size() > 1)
            {
                EXPECT_NEAR(amplitudeProfile[1].second, 5725, 100);
            }

            // item.second->dumpPcmData();
        }

        std::sort(allFreq.begin(), allFreq.end());
        EXPECT_NEAR(allFreq[0], 600.0, 25.0);
        EXPECT_NEAR(allFreq[1], 2100.0, 25.0);
    }
    {
        auto audioCounters = client3._transport->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
        EXPECT_EQ(audioCounters.lostPackets, 0);

        const auto& rData1 = client3.getReceiveStats();
        EXPECT_EQ(rData1.size(), 1);
        for (const auto& item : rData1)
        {
            std::vector<double> freqVector;
            std::vector<std::pair<uint64_t, double>> amplitudeProfile;
            auto rec = item.second->getRecording();
            analyzeRecording(rec, freqVector, amplitudeProfile, item.second->getLoggableId().c_str());

            std::sort(freqVector.begin(), freqVector.end());
            EXPECT_EQ(freqVector.size(), 2);
            EXPECT_NEAR(freqVector[0], 600.0, 25.0);
            EXPECT_NEAR(freqVector[1], 1300.0, 25.0);

            EXPECT_GE(amplitudeProfile.size(), 4);
            for (auto& item : amplitudeProfile)
            {
                logger::debug("%.3fs, %.3f", "", item.first / 48000.0, item.second);
            }
            if (amplitudeProfile.size() >= 4)
            {
                EXPECT_EQ(amplitudeProfile[0].second, 0);

                EXPECT_NEAR(amplitudeProfile[3].second, 1826, 250);
                EXPECT_NEAR(amplitudeProfile[3].first, 48000 * 1.67, 2400);

                EXPECT_NEAR(amplitudeProfile.back().second, 1730, 250);
                EXPECT_LE(amplitudeProfile.back().first, 2 * 48000);
            }

            // item.second->dumpPcmData();
        }
    }
}
