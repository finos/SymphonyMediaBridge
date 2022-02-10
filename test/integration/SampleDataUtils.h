#pragma once

#include "bridge/RtpMap.h"
#include "memory/Packet.h"
#include "utils/Optional.h"
#include <complex>
#include <string>
#include <valarray>
#include <vector>

class SampleDataUtils
{
public:
    // returns array of first 9 packets of valid RTP stream with Opus payload with ssrc 1491699885.
    static const std::vector<const memory::Packet>& getOpusRtpSamplePackets() { return _opusRtpSamplePackets; }

    // Time span duration measured in EngineMixer::iterationDurationMs
    typedef size_t DurationIterations;
    // PCM data
    typedef std::vector<int16_t> AudioData;

    // builds an list of packets that can be fed to mixer at interval EngineMixer::iterationDurationMs
    struct RtpStreamBuilder
    {
        virtual ~RtpStreamBuilder() = default;
        virtual void addBuzz(DurationIterations length) = 0;
        virtual void addSilence(DurationIterations length) = 0;
        virtual const std::vector<std::vector<memory::Packet>>& get() = 0;
    };

    static std::unique_ptr<RtpStreamBuilder> makeOpusStreamBulder();

    // generates requested nr of RTP packets with Opus-encoded buzz
    static std::vector<memory::Packet> generateOpusRtpStream(size_t nrOfPackets);

    static AudioData decodeOpusRtpStream(const std::vector<memory::Packet>& packets);

    static bool verifyAudioLevel(const std::vector<memory::Packet>& packets, const AudioData& audio);

    static void assertSilence(const char* name,
        const AudioData& audioData,
        DurationIterations begin,
        DurationIterations end);
    static void assertBuzz(const char* name,
        const AudioData& audioData,
        DurationIterations begin,
        DurationIterations end);

    using CmplxArray = std::valarray<std::complex<double>>;
    static void fft(CmplxArray& data);
    static void ifft(CmplxArray& data);

    static void listFrequencies(CmplxArray& frequencyTransform, uint32_t sampleRate, std::vector<double>& frequencies);

private:
    static const std::vector<const memory::Packet> _opusRtpSamplePackets;
};
