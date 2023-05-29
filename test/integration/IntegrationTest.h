#pragma once

#include "api/ConferenceEndpoint.h"
#include "bridge/Bridge.h"
#include "config/Config.h"
#include "emulator/TimeTurner.h"
#include "test/integration/FFTanalysis.h"
#include "test/integration/emulator/Httpd.h"
#include "test/integration/emulator/SfuClient.h"
#include "test/integration/emulator/SfuGroupCall.h"
#include "transport/EndpointFactory.h"
#include "transport/RtcTransport.h"
#include "utils/Pacer.h"
#include <cstdint>
#include <gtest/gtest.h>

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
void logTransportSummary(const char* clientName, T& summary)
{
    for (auto& report : summary)
    {
        const auto bitrate = report.second.rtpFrequency * report.second.octets /
            (125 * (report.second.rtpTimestamp - report.second.initialRtpTimestamp));

        const char* modality = (report.second.rtpFrequency == 90000 ? "video" : "audio");
        logger::debug("%s ssrc %u sent %s pkts %u, %" PRIu64 " kbps",
            "Test",
            clientName,
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
        std::map<size_t, size_t> receivedBytes;
    };

    IntegrationTest();
    ~IntegrationTest();
    emulator::HttpdFactory* _httpd;
    std::unique_ptr<bridge::Bridge> _bridge;
    config::Config _config;
    config::Config _clientConfig;

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

    std::vector<transport::SocketAddress> _smbInterfaces;
    std::unique_ptr<transport::TransportFactory> _clientTransportFactory;
    std::unique_ptr<transport::TransportFactory> _publicTransportFactory;
    std::shared_ptr<fakenet::InternetRunner> _internet;
    std::shared_ptr<transport::EndpointFactory> _bridgeEndpointFactory;
    std::shared_ptr<transport::EndpointFactory> _clientsEndpointFactory;
    std::shared_ptr<fakenet::Firewall> _firewall;

    uint32_t _instanceCounter;
    const size_t _numWorkerThreads;

    struct Ips
    {
        const std::string client;
        const std::string smb;
        const std::string firewall;
    };
    Ips _ipv4;
    Ips _ipv6;
    std::string _defaultSmbConfig;

    void SetUp() override;
    void TearDown() override;

    void initBridge(config::Config& config);
    void initBridge(config::Config& config, config::Config& configClients);
    void finalizeSimulationWithTimeout(uint64_t rampdownTimeout);
    void finalizeSimulation();

    size_t expectedTestThreadCount(size_t smbCount) const
    {
        return (1 + smbCount) * (_numWorkerThreads + 3) + smbCount;
    }

    void enterRealTime(size_t expectedThreadCount, uint64_t timeout = 4 * utils::Time::sec);

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
        bool checkAmplitudeProfile = true,
        size_t mixedAudioSources = 0,
        bool dumpPcmData = false)
    {
        constexpr auto AUDIO_PACKET_SAMPLE_COUNT = codec::Opus::sampleRate / codec::Opus::packetsPerSecond;
        auto audioCounters = client->getAudioReceiveCounters(utils::Time::getAbsoluteTime());
        EXPECT_EQ(audioCounters.lostPackets, 0);

        const auto& data = client->getAudioReceiveStats();
        IntegrationTest::AudioAnalysisData result;

        for (const auto& item : data)
        {
            if (client->isRemoteVideoSsrc(item.first))
            {
                continue;
            }

            result.audioSsrcCount++;

            std::vector<double> freqVector;
            std::vector<std::pair<uint64_t, double>> amplitudeProfile;
            auto rec = item.second->getRecording();

            ::analyzeRecording(rec,
                freqVector,
                amplitudeProfile,
                item.second->getLoggableId().c_str(),
                mixedAudioSources ? expectedDurationSeconds * utils::Time::ms : 0);

            if (mixedAudioSources)
            {
                EXPECT_EQ(freqVector.size(), mixedAudioSources);
                EXPECT_GE(rec.size(), expectedDurationSeconds * codec::Opus::sampleRate);
            }
            else
            {
                EXPECT_EQ(freqVector.size(), 1);
                EXPECT_NEAR(rec.size(),
                    expectedDurationSeconds * codec::Opus::sampleRate,
                    3 * AUDIO_PACKET_SAMPLE_COUNT);

                if (checkAmplitudeProfile)
                {
                    EXPECT_EQ(amplitudeProfile.size(), 2);
                    if (amplitudeProfile.size() > 1)
                    {
                        EXPECT_NEAR(amplitudeProfile[1].second, 5725, 125);
                    }
                }
            }

            result.dominantFrequencies.insert(result.dominantFrequencies.begin(), freqVector.begin(), freqVector.end());
            if (freqVector.size())
            {
                result.receivedBytes[item.first] = rec.size();
            }

            result.amplitudeProfile.insert(result.amplitudeProfile.begin(),
                amplitudeProfile.begin(),
                amplitudeProfile.end());
            if (dumpPcmData)
            {
                item.second->dumpPcmData();
            }
        }

        std::sort(result.dominantFrequencies.begin(), result.dominantFrequencies.end());
        return result;
    }

protected:
    void runTestInThread(const size_t expectedNumThreads, std::function<void()> test);
    void startSimulation();

    void initLocalTransports(config::Config& bridgeConfig);

protected:
    emulator::TimeTurner _timeSource;

    struct NetworkLinkInfo
    {
        fakenet::NetworkLink* ptrLink;
        transport::SocketAddress address;
    };

    std::map<std::string, NetworkLinkInfo> _endpointNetworkLinkMap;
    std::map<std::string, NetworkLinkInfo> _clientNetworkLinkMap;
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

template <typename TChannel>
void make5secCallWithDefaultAudioProfile(emulator::GroupCall<emulator::SfuClient<TChannel>>& groupCall)
{
    static const double frequencies[] = {600, 1300, 2100, 3200, 4100, 4800, 5200};
    for (size_t i = 0; i < groupCall.clients.size(); ++i)
    {
        groupCall.clients[i]->_audioSource->setFrequency(frequencies[i]);
    }

    for (auto& client : groupCall.clients)
    {
        client->_audioSource->setVolume(0.6);
    }

    groupCall.run(utils::Time::sec * 5);
    utils::Time::nanoSleep(utils::Time::sec * 1);

    for (auto& client : groupCall.clients)
    {
        client->stopRecording();
    }
}

template <typename TChannel>
void makeShortCallWithDefaultAudioProfile(emulator::GroupCall<emulator::SfuClient<TChannel>>& groupCall,
    uint64_t duration)
{
    static const double frequencies[] = {600, 1300, 2100, 3200, 4100, 4800, 5200};
    for (size_t i = 0; i < groupCall.clients.size(); ++i)
    {
        groupCall.clients[i]->_audioSource->setFrequency(frequencies[i]);
    }

    for (auto& client : groupCall.clients)
    {
        client->_audioSource->setVolume(0.6);
    }

    groupCall.run(duration);
    utils::Time::nanoSleep(utils::Time::sec * 1);

    for (auto& client : groupCall.clients)
    {
        client->stopRecording();
    }
}

} // namespace
