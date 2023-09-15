#include "FFTanalysis.h"
#include "api/Parser.h"
#include "api/utils.h"
#include "bridge/Mixer.h"
#include "bridge/endpointActions/ApiHelpers.h"
#include "bridge/engine/SsrcInboundContext.h"
#include "codec/Opus.h"
#include "codec/OpusDecoder.h"
#include "concurrency/MpmcHashmap.h"
#include "emulator/FakeEndpointFactory.h"
#include "external/http.h"
#include "jobmanager/JobManager.h"
#include "jobmanager/WorkerThread.h"
#include "memory/PacketPoolAllocator.h"
#include "nlohmann/json.hpp"
#include "test/bwe/FakeVideoSource.h"
#include "test/integration/IntegrationTest.h"
#include "test/integration/SampleDataUtils.h"
#include "test/integration/emulator/AudioSource.h"
#include "test/integration/emulator/HttpRequests.h"
#include "transport/DataReceiver.h"
#include "transport/EndpointFactoryImpl.h"
#include "transport/RtcePoll.h"
#include "transport/Transport.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SslDtls.h"
#include "utils/IdGenerator.h"
#include "utils/SimpleJson.h"
#include "utils/StringBuilder.h"
#include <chrono>
#include <complex>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <unordered_set>

class FFTtest : public testing::TestWithParam<std::string>
{
public:
    void SetUp() override {}
    void ShutDown() {}
};

TEST_P(FFTtest, runner)
{
    std::string filename = "_bwelogs/" + GetParam();
    memory::PacketPoolAllocator allocator(4096 * 4, "JitterTest");

    auto pcm16File = ::fopen(filename.c_str(), "r");
    if (!pcm16File)
    {
        return;
    }

    std::vector<int16_t> samples;
    samples.resize(15 * 16000);
    int samplesRead = fread(samples.data(), sizeof(int16_t), 15 * 16000, pcm16File);

    samples.resize(samplesRead);
    std::vector<double> freqPeaks;
    std::vector<std::pair<uint64_t, double>> ampProfile;
    //::analyzeRecording(samples, freqPeaks, ampProfile, "FFTtest", 16000, 0);
    auto spectrum = ::createAudioSpectrum(samples, 16000);
    auto powerSpectrum = SampleDataUtils::powerSpectrumDB(spectrum);

    auto pwrFreq = SampleDataUtils::toPowerVector(powerSpectrum, 16000);
    std::sort(pwrFreq.begin(),
        pwrFreq.end(),
        [](const std::pair<double, double>& f1, const std::pair<double, double>& f2) { return f2.second < f1.second; });

    auto peaks = SampleDataUtils::isolatePeaks(pwrFreq, -25, 16000);
    for (auto p : peaks)
    {
        logger::info("peaks %.f, %.2fdB", "", p.first, p.second);
    }
}

INSTANTIATE_TEST_SUITE_P(DISABLED_FFTrerun,
    FFTtest,
    ::testing::Values("tutv-1000.raw",
        "tutv2-800-1000.raw",
        "tutv1-sound-1200-800.raw",
        "olof-800-1000.raw",
        "sPK-0537-11a7bf23464a9c2666e97567e18e-tuvantran1-30324173351765032421695075552-1.raw",
        "Tu1200_1500.raw",
        "should1500.raw"));
