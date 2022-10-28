#pragma once

#include "api/ConferenceEndpoint.h"
#include "bridge/Bridge.h"
#include "config/Config.h"
#include "emulator/TimeTurner.h"
#include "test/integration/emulator/Httpd.h"
#include "test/transport/FakeNetwork.h"
#include "transport/EndpointFactory.h"
#include "transport/RtcTransport.h"
#include "utils/Pacer.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <mutex>

namespace
{
template <typename T>
void logVideoSent(const char* clientName, T& client)
{
    for (auto& itPair : client._videoSources)
    {
        auto& videoSource = itPair.second;
        logger::info("%s video source %u, sent %u packets, bitrate %f",
            "Test",
            clientName,
            videoSource->getSsrc(),
            videoSource->getPacketsSent(),
            videoSource->getBitRate());
    }
}

template <typename T>
void logVideoReceive(const char* clientName, T& client)
{
    auto allStreamsVideoStats = client.getActiveVideoDecoderStats();
    uint32_t streamId = 0;
    std::ostringstream logLine;
    for (const auto& videoStats : allStreamsVideoStats)
    {
        std::ostringstream table;
        table << "\n"
              << clientName << " (ID hash: " << client.getEndpointIdHash() << ")"
              << " stream-" << streamId++ << " video stats:";

        table << "\n----------------------------------------------";
        table << "\n\tEndpoint ID Hash \t\t Tag \tFrames";
        table << "\n";
        for (const auto& sequence : videoStats.frameSequences)
        {
            table << "\n\t" << std::setw(16) << sequence.endpointHashId << "\t" << std::setw(6) << sequence.tag
                  << "\t\t" << sequence.numFrames;
        }
        table << "\n----------------------------------------------";
        table << "\n\tDecoded frames: " << std::setw(13) << videoStats.numDecodedFrames;
        table << "\n\tLast frame num: " << std::setw(13) << videoStats.lastDecodedFrameNum;
        table << "\n\tAverage FPS: " << std::setw(16)
              << (double)utils::Time::sec / (double)videoStats.averageFrameRateDelta;
        table << "\n\tMax inter-frame delta: " << std::setw(6) << videoStats.maxFrameRateDelta / utils::Time::ms
              << " ms";
        table << "\n\tMax frame reorder: " << std::setw(10) << videoStats.maxReorderFrameCount;
        table << "\n\tMax packet reorder: " << std::setw(9) << videoStats.maxReorderPacketCount;
        table << "\n\tReceived packets: " << std::setw(11) << videoStats.numReceivedPackets;
        logLine << table.str() << "\n";
    }
    logger::info("%s\n", "VideoDecoderInfo", logLine.str().c_str());
}

template <typename T>
void logTransportSummary(const char* clientName, transport::RtcTransport* transport, T& summary)
{
    for (auto& report : summary)
    {
        const auto bitrate = report.second.rtpFrequency * report.second.octets /
            (125 * (report.second.rtpTimestamp - report.second.initialRtpTimestamp));

        const char* modality = (report.second.rtpFrequency == 90000 ? "video" : "audio");
        logger::debug("%s %s ssrc %u sent %s pkts %u, %" PRIu64 " kbps",
            "Test",
            clientName,
            transport->getLoggableId().c_str(),
            report.first,
            modality,
            report.second.packetsSent,
            bitrate);
    }
}
} // namespace

struct IntegrationTest : public ::testing::Test
{

    struct AudioAnalysisData
    {
        std::vector<double> dominantFrequencies;
        std::vector<std::pair<uint64_t, double>> amplitudeProfile;
        size_t audioSsrcCount = 0;
    };

    IntegrationTest();
    ~IntegrationTest();
    emulator::HttpdFactory* _httpd;
    std::unique_ptr<bridge::Bridge> _bridge;
    config::Config _config;

    memory::PacketPoolAllocator _sendAllocator;
    memory::AudioPacketPoolAllocator _audioAllocator;
    std::unique_ptr<jobmanager::TimerQueue> _timers;
    std::unique_ptr<jobmanager::JobManager> _jobManager;
    std::unique_ptr<memory::PacketPoolAllocator> _mainPoolAllocator;
    transport::SslDtls* _sslDtls;
    std::unique_ptr<transport::SrtpClientFactory> _srtpClientFactory;
    std::vector<std::unique_ptr<jobmanager::WorkerThread>> _workerThreads;
    std::unique_ptr<transport::RtcePoll> _network;
    ice::IceConfig _iceConfig;
    sctp::SctpConfig _sctpConfig;
    bwe::Config _bweConfig;
    bwe::RateControllerConfig _rateControlConfig;
    utils::Pacer _pacer;

    std::unique_ptr<transport::TransportFactory> _transportFactory;
    std::shared_ptr<fakenet::InternetRunner> _internet;
    std::shared_ptr<transport::EndpointFactory> _bridgeEndpointFactory;
    std::shared_ptr<transport::EndpointFactory> _clientsEndpointFactory;

    uint32_t _instanceCounter;
    const size_t _numWorkerThreads;

    void SetUp() override;
    void TearDown() override;

    void initBridge(config::Config& config);
    void finalizeSimulationWithTimeout(uint64_t rampdownTimeout);
    void finalizeSimulation();

public:
    static bool isActiveTalker(const std::vector<api::ConferenceEndpoint>& endpoints, const std::string& endpoint);
    static std::vector<api::ConferenceEndpoint> getConferenceEndpointsInfo(emulator::HttpdFactory* httpd,
        const char* baseUrl);
    static api::ConferenceEndpointExtendedInfo getEndpointExtendedInfo(emulator::HttpdFactory* httpd,
        const char* baseUrl,
        const std::string& endpointId);

    template <typename TClient>
    static IntegrationTest::AudioAnalysisData analyzeRecording(TClient* client,
        double expectedDurationSeconds,
        size_t mixedAudioSources = 0,
        bool dumpPcmData = false);

protected:
    void runTestInThread(const size_t expectedNumThreads, std::function<void()> test);
    void startSimulation();

    void initLocalTransports();

protected:
    emulator::TimeTurner _timeSource;

    struct NetworkLinkInfo
    {
        fakenet::NetworkLink* ptrLink;
        transport::SocketAddress address;
    };

    std::map<std::string, NetworkLinkInfo> _endpointNetworkLinkMap;
    const uint32_t _clientsConnectionTimeout;

private:
    size_t getNumWorkerThreads();
};

namespace
{
class ScopedFinalize
{
public:
    explicit ScopedFinalize(std::function<void()> finalizeMethod) : _method(finalizeMethod) {}
    ~ScopedFinalize() { _method(); }

private:
    std::function<void()> _method;
};
} // namespace
