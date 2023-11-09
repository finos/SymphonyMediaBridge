#include "SampleDataUtils.h"
#include "bridge/engine/EngineMixer.h"
#include "codec/Opus.h"
#include "codec/OpusDecoder.h"
#include "codec/OpusEncoder.h"
#include "logger/Logger.h"
#include "rtp/RtcpHeader.h"
#include "utils/ScopedFileHandle.h"
#include <algorithm>
namespace
{

template <size_t size>
static memory::Packet makePacket(const char (&s)[size])
{
    memory::Packet packet;
    packet.setLength(size);
    memcpy(packet.get(), s, size);
    return packet;
}

struct StreamSsrcVerifier
{
    uint32_t streamSsrc = 0;

    void observePacketSsrc(const uint32_t ssrc)
    {
        if (streamSsrc == 0)
        {
            streamSsrc = ssrc;
        }
        else
        {
            assert(streamSsrc == ssrc);
        }
    }
};

template <typename WORDTYPE>
void dumpAudio(const char* name, const std::vector<WORDTYPE>& audio)
{
    std::string fileName("/tmp/");
    fileName += name;
    utils::ScopedFileHandle dump(fopen(fileName.c_str(), "wr"));
    fwrite(audio.data(), sizeof(WORDTYPE), audio.size(), dump.get());
}

bool containsNear(std::vector<std::pair<double, double>>& frequencies, double frequency, double difference)
{
    for (auto& f : frequencies)
    {
        if (std::abs(frequency - f.first) < difference)
        {
            return true;
        }
    }

    return false;
}

} // namespace

const std::vector<const memory::Packet> SampleDataUtils::_opusRtpSamplePackets = {
    makePacket("\x90\x6f\x6a\x7c\x53\x93\x51\x20\x58\xe9\x88\xad\xbe\xde\x00\x01\x10\xc4\x00\x00\x78\x1b\x86\xf8\x81"
               "\x01\x78\x83\x1c\x8a\x1d\x9e\x33\x68\xdd\x3f\x87\x3f\xd2\x75\xc3\xd8\x1a\x6e\x22\x64\x26\x85\x14\x5b"
               "\x0c\x44\xab\x79\xd1\x07\x16\x7c\x02\x67\x17\xad\xfc\xc4\x12\x84\xb2\x29\x2f\xa9\x2b\xca\xaf\x5e\x50"
               "\x52\xa2\xc2\x1d\xb1\x29\x28\x23\x56\xa8\x77\xde\x65\x34\xb5\xbf\x9a\x5e"),
    makePacket("\x90\x6f\x6a\x7d\x53\x93\x54\xe0\x58\xe9\x88\xad\xbe\xde\x00\x01\x10\xc4\x00\x00\x78\x1b\x26\x77\xa9"
               "\x91\xdc\xb8\x43\xee\x72\xb5\x38\xea\x9a\x34\xe4\x71\x39\xee\xe6\x49\xc4\x3b\xa5\x6a\x8f\x45\x00\x13"
               "\x75\xaf\x6a\x30\xd1\x80\x7f\x82\xb7\x88\x37\xa5\x42\xe7\xfb\x17\x8a\xfe\xee\x4b\x5d\x75\x55\x3f\x51"
               "\x9d\x1c\x1e\x69\x90\x6d\xe3\x5d\x9a\x9f\x10\x8a\x16"),
    makePacket("\x90\x6f\x6a\x7e\x53\x93\x58\xa0\x58\xe9\x88\xad\xbe\xde\x00\x01\x10\xc5\x00\x00\x78\x18\x01\x33\xb0"
               "\x32\xaf\xa5\x0f\x71\x7e\x64\x8d\xdb\x9a\xb4\x98\x0f\x4b\x7e\x59\x33\xb9\x12\xac\x8c\x00\x9a\x2d\xdb"
               "\x55\x64\xbe\xd2\x0d\x45\x18\x39\x88\xfa\xca\xe1\x38\x9e\x9a\x37\xd5\xd8\xb9\x50\x6d\xd3\x85\xd2\x83"
               "\x9a\x9c\x5d\x45\x8a\xf8\x19\xfe\xef\x7d\x0b\x0f\x47\xf8\x82\xba"),
    makePacket("\x90\x6f\x6a\x7f\x53\x93\x5c\x60\x58\xe9\x88\xad\xbe\xde\x00\x01\x10\xc8\x00\x00\x78\x15\x31\x1b\x3e"
               "\x80\xf5\xb9\xc6\x01\x4d\x29\xc5\xc0\x96\x2c\x5b\x20\xc3\x6f\x7b\x25\x7f\x66\x65\x9b\x15\x90\x8b\xb6"
               "\x69\x22\xcd\x93\x3e\x31\x82\xa3\x23\x5c\x94\x2c\x89\xa1\x00\x9a\x03\x79\xe4\xf8\x9a\xc1\xb2\xf0\xa0"
               "\x29\xc9\x5f\xf3\xb7\x7a\x11\x00\x08\x0a\x74\x55\x3f\x8b\x3b"),
    makePacket(
        "\x90\x6f\x6a\x80\x53\x93\x60\x20\x58\xe9\x88\xad\xbe\xde\x00\x01\x10\xa0\x00\x00\x78\x80\x30\x1d\xb4\x1e\x4b"
        "\x6f\x24\x54\xd0\xe7\xf0\x97\x30\x0e\xeb\x08\x6f\x39\x0a\xb9\xed\xb9\x99\x5b\x00\x09\xee\xd1\xa7\xd4\x67\xb5"
        "\x92\xe8\xf6\x46\x61\xf3\x52\xc1\x75\x38\x69\x54\x4f\x1d\xa0\x20\xf8\xa4\xc1\xb9\x53\xa7\x73\xb7\x50\x85\xa9"
        "\x44\x80\x36\x77\xa9\x8e\xec\x85\xd6\x4c\x3d\xf6\xf5\xb7\xf0\x4a\xce\x92\x3d\x8a\x70\xbd"),
    makePacket("\x90\x6f\x6a\x81\x53\x93\x63\xe0\x58\xe9\x88\xad\xbe\xde\x00\x01\x10\xa4\x00\x00\x78\x83\xd2\x8b\xe2"
               "\x3a\xdd\x5d\x51\x05\x3d\x05\x0e\x70\xe0\x13\xa5\x75\xfd\x36\xf0\xd7\xf2\x18\x8d\xb4\x16\x8f\x73\xa5"
               "\x84\x89\x14\x7e\x07\xcb\xf9\xc5\x99\x9c\x72\x45\xe2\xbe\x20\x74\x41\x0c\x1d\x57\x5a\xf2\x92\x7e\xdd"
               "\xfe\xbd\xed\x23\x65\xf5\x66\xdf\x6b\x53\x52\x15\x1c\x8f\x6c\x01\x44\x6f\xf8\x9a\xc2\x55\x14\x48\x24"
               "\xfd\x32\x96\xfc\xa2\xbe\xda\x07\xe0\x41\x97\x0c\x0c\x51\x20\xdc"),
    makePacket("\x90\x6f\x6a\x82\x53\x93\x67\xa0\x58\xe9\x88\xad\xbe\xde\x00\x01\x10\x8f\x00\x00\x78\x82\x87\x33\x70"
               "\x28\x70\x92\x98\x48\x37\x0d\xba\xa2\xe9\xcb\x1e\xf9\x70\x1a\x83\xc5\x61\x00\x4b\x1f\x26\xf5\x15\xe0"
               "\x40\xe6\x36\xf4\x5f\x21\x7c\x3a\x5b\x5b\xc0\xbf\x86\xb3\x15\x4d\x19\x9a\xdb\xc7\xa5\x1d\x5f\x1b\xe8"
               "\xe2\x3d\x43\x44\x22\xd5\xaa\x0b\x8e\xf2\xf6\x0b\x13\x6a\x57\x00\xa9\xa7\x1b\xac\x72\xf2\x58\xb5\x4f"
               "\x54\x58\xb3\xd6\xbf\x4f\xee\x4f\xb3\xb6\xd5\xfb\x61\xcf"),
    makePacket("\x90\x6f\x6a\x83\x53\x93\x6b\x60\x58\xe9\x88\xad\xbe\xde\x00\x01\x10\x8d\x00\x00\x78\x83\xc6\xb0\xf1"
               "\xd5\xb3\x41\x7d\x12\x5d\xd4\xca\xff\x41\xbe\xc9\x61\xd6\xeb\xe9\xb8\xd6\xfe\x03\xad\xee\xb3\x1c\x30"
               "\x01\xe3\x2e\xb9\x66\x8e\x66\x65\x47\x85\xdf\x31\xcb\xdd\x2d\x99\xeb\xa2\x12\x15\xe0\x88\x68\xeb\xdd"
               "\xfd\x85\x18\xda\x99\x2b\x8e\xd2\xba\xae\x52\xc6\x2c\xeb\x68\x3a\x12\xf1\x8e\xe7\x01\xd1\xd2\x5d\xcc"
               "\x83\x6d\x62\x4a\xfc\xdd\xc2\x86\x06"),
    makePacket("\x90\x6f\x6a\x84\x53\x93\x6f\x20\x58\xe9\x88\xad\xbe\xde\x00\x01\x10\x8f\x00\x00\x78\xb1\xb0\x60\x14"
               "\x81\x4b\x79\x9a\x96\x06\x56\xa5\x83\x5f\x64\x30\x70\x51\xe0\x54\x56\xff\x7a\xe6\xf9\xe4\xcb\xbb\xe8"
               "\xcd\xf4\x51\x3e\xef\x77\x2a\xc0\xc3\xca\xf5\xde\x52\x14\xf2\x04\x20\x2a\xef\x6f\xe1\x58\xcd\xb7\xd7"
               "\x73\xf7\xd5\x3d\xb8\xc0\xff\x6a\xbe\x09\xe1\xad\xd4\xc2\x61\xc1\xa2\x5c\x08\x27\xb0\x5b\x72\xcf\x12"
               "\x44\x18\x5e\x75\xf4\x96\x11\x4c\xa3\xba\xa5\x0b"),
};

void buzzFunction(int16_t* iterationBuff, size_t iterationBuffSize)
{
    for (size_t i = 0; i < iterationBuffSize; ++i)
    {
        iterationBuff[i] = static_cast<int16_t>(sin( // extract to func
                                                    (M_PI * 20.0) * double(i) / double(iterationBuffSize - 1)) *
            20000.0);
    }
}

void silenceFunction(int16_t* iterationBuff, size_t iterationBuffSize)
{
    for (size_t i = 0; i < iterationBuffSize; ++i)
    {
        iterationBuff[i] = 0;
    }
}

struct OpusBuilder : SampleDataUtils::RtpStreamBuilder
{
    std::vector<std::vector<memory::Packet>> _packets;
    uint16_t _sequenceNumber = 1;
    uint32_t _clientTimestamp = 0;
    const uint32_t _ssrc = 0;
    codec::OpusEncoder _encoder;

    OpusBuilder() { assert(_encoder.isInitialized()); }

    void addBuzz(SampleDataUtils::DurationIterations length) override { add(length, buzzFunction); }

    void addSilence(SampleDataUtils::DurationIterations length) override { add(length, silenceFunction); }

    const std::vector<std::vector<memory::Packet>>& get() override { return _packets; }

    void add(SampleDataUtils::DurationIterations length,
        std::function<void(int16_t* iterationBuff, size_t iterationBuffSize)> generator)
    {
        using namespace bridge;

        for (size_t i = 0; i < length; ++i)
        {
            std::vector<memory::Packet> batch(1);
            memory::Packet& packet = batch[0];
            memset(packet.get(), 0, memory::Packet::size);

            auto rtpHeader = rtp::RtpHeader::create(packet);
            rtpHeader->payloadType = codec::Opus::payloadType;
            rtpHeader->sequenceNumber = _sequenceNumber;
            rtpHeader->ssrc = _ssrc;
            rtpHeader->timestamp = _clientTimestamp;

            auto payloadStart = rtpHeader->getPayload();
            const auto headerLength = rtpHeader->headerLength();

            const int32_t pcmDataSize = EngineMixer::samplesPerIteration * EngineMixer::bytesPerSample;
            int16_t pcmData[pcmDataSize / sizeof(int16_t)];

            generator(pcmData, pcmDataSize / sizeof(int16_t));

            const size_t frames = pcmDataSize / EngineMixer::bytesPerSample / EngineMixer::channelsPerFrame;
            const size_t payloadMaxSize = memory::Packet::size - headerLength;
            const size_t payloadMaxFrames =
                payloadMaxSize / codec::Opus::channelsPerFrame / codec::Opus::bytesPerSample;

            uint8_t encodedData[memory::Packet::size];
            const auto encodedBytes = _encoder.encode(pcmData, frames, encodedData, payloadMaxFrames);
            assert(encodedBytes > 0);
            memcpy(payloadStart, encodedData, encodedBytes);

            packet.setLength(headerLength + encodedBytes);

            ++_sequenceNumber;
            _clientTimestamp += bridge::EngineMixer::framesPerIteration48kHz;

            _packets.emplace_back(std::move(batch));
        }
    }
};

std::unique_ptr<SampleDataUtils::RtpStreamBuilder> SampleDataUtils::makeOpusStreamBulder()
{
    return std::make_unique<OpusBuilder>();
}

std::vector<memory::Packet> SampleDataUtils::generateOpusRtpStream(DurationIterations length)
{
    auto builder = makeOpusStreamBulder();
    builder->addBuzz(length);

    const auto& list = builder->get();
    assert(list.size() == length);

    std::vector<memory::Packet> result;
    result.reserve(length);
    for (const auto& batch : list)
    {
        assert(batch.size() == 1);
        result.push_back(batch[0]);
    }

    return result;
}

SampleDataUtils::AudioData SampleDataUtils::decodeOpusRtpStream(const std::vector<memory::Packet>& packets)
{
    std::vector<int16_t> result;
    static_assert(sizeof(decltype(result)::value_type) == codec::Opus::bytesPerSample, "bad PCM data type in tests");
    codec::OpusDecoder decoder;
    assert(decoder.isInitialized());
    StreamSsrcVerifier streamSsrcVerifier;

    for (const auto& packet : packets)
    {
        const bool isRtp = rtp::isRtpPacket(packet);
        assert(isRtp || rtp::isRtcpPacket(packet));
        if (!isRtp)
        {
            continue;
        }
        auto rtpHeader = rtp::RtpHeader::fromPacket(packet);
        const uint32_t ssrc = rtpHeader->ssrc;
        const uint16_t sequenceNumber = rtpHeader->sequenceNumber;
        streamSsrcVerifier.observePacketSsrc(ssrc);
        assert(rtpHeader->payloadType == codec::Opus::payloadType);

        uint8_t decodedData[memory::Packet::size];
        const uint32_t headerLength = rtpHeader->headerLength();
        const uint32_t payloadLength = packet.getLength() - headerLength;
        auto payloadStart = rtpHeader->getPayload();

        if (decoder.getExpectedSequenceNumber() != 0)
        {
            assert(sequenceNumber == decoder.getExpectedSequenceNumber());
        }

        const auto framesInPacketBuffer =
            memory::Packet::size / codec::Opus::channelsPerFrame / codec::Opus::bytesPerSample;
        const auto decodedFrames =
            decoder.decode(sequenceNumber, payloadStart, payloadLength, decodedData, framesInPacketBuffer);
        assert(decodedFrames > 0);
        const auto decodedSamplesCount = decodedFrames * codec::Opus::channelsPerFrame;

        result.resize(result.size() + decodedSamplesCount);
        memcpy(&result[result.size() - decodedSamplesCount],
            decodedData,
            decodedSamplesCount * codec::Opus::bytesPerSample);
    }

    return result;
}

void SampleDataUtils::fft(CmplxArray& x)
{
    const size_t N = x.size();
    if (N <= 1)
        return;

    std::valarray<std::complex<double>> even = x[std::slice(0, N / 2, 2)];
    std::valarray<std::complex<double>> odd = x[std::slice(1, N / 2, 2)];

    fft(even);
    fft(odd);

    for (size_t k = 0; k < N / 2; ++k)
    {
        std::complex<double> t = std::polar(1.0, -2 * M_PI * k / N) * odd[k];
        x[k] = even[k] + t;
        x[k + N / 2] = even[k] - t;
    }
}

void SampleDataUtils::ifft(SampleDataUtils::CmplxArray& x)
{
    x = x.apply(std::conj);
    fft(x);
    x = x.apply(std::conj);

    // scale the numbers
    x /= x.size();
}

void SampleDataUtils::applyHannWindow(SampleDataUtils::CmplxArray& x)
{
    for (size_t t = 0; t < x.size(); ++t)
    {
        x[t] = x[t] * 0.5 * (1.0 - std::cos(2.0 * M_PI * t / x.size()));
    }
}

std::vector<double> SampleDataUtils::powerSpectrum(const CmplxArray& fftVector)
{
    std::vector<double> spectrum;
    spectrum.reserve(fftVector.size() / 2);
    const double scale = 2.0 / (fftVector.size() * sqrt(2));
    for (size_t i = 0; i < fftVector.size() / 2; ++i)
    {
        spectrum.push_back(std::abs(fftVector[i]) * scale);
    }
    return spectrum;
}

std::valarray<double> SampleDataUtils::powerSpectrumDB(const CmplxArray& fftVector)
{
    std::valarray<double> spectrum(fftVector.size() / 2);

    const double scale = 2.0 / (fftVector.size() * sqrt(2));
    for (size_t i = 0; i < fftVector.size() / 2; ++i)
    {
        spectrum[i] = 10 * log(std::abs(fftVector[i]) * scale);
    }
    return spectrum;
}

std::vector<std::pair<double, double>> SampleDataUtils::toPowerVector(std::valarray<double>& powerSpectrum,
    size_t sampleRate)
{
    std::vector<std::pair<double, double>> v;
    const double freqDelta = sampleRate / 2.0 / powerSpectrum.size();
    double f = 0;
    for (auto m : powerSpectrum)
    {
        v.push_back({f, m});
        f += freqDelta;
    }
    return v;
}

std::vector<std::pair<double, double>> SampleDataUtils::isolatePeaks(std::vector<std::pair<double, double>>& powerFreq,
    double threshold,
    size_t sampleRate)
{
    const double freqDelta = static_cast<double>(sampleRate) / powerFreq.size() / 2;
    std::vector<std::pair<double, double>> v;
    for (auto m : powerFreq)
    {
        if (m.second > threshold && !containsNear(v, m.first, freqDelta * 8))
        {
            v.push_back(m);
        }
    }

    return v;
}

void SampleDataUtils::listFrequencies(CmplxArray& frequencyTransform,
    uint32_t sampleRate,
    std::vector<double>& frequencies)
{
    const double freqDelta = static_cast<double>(sampleRate) / frequencyTransform.size();
    double delta = 0;
    const double threshold = frequencyTransform.size() * 0.01;
    for (size_t i = 1; i < frequencyTransform.size() / 2; ++i)
    {
        auto prevDelta = delta;
        delta = std::abs(frequencyTransform[i]) - std::abs(frequencyTransform[i - 1]);
        if (prevDelta > 0 && delta < 0 && std::abs(frequencyTransform[i - 1]) > threshold && prevDelta > threshold / 2)
        {
            frequencies.push_back(freqDelta * (i - 1));
        }
    }
}

void SampleDataUtils::listFrequenciesNew(CmplxArray& frequencyTransform,
    uint32_t sampleRate,
    std::vector<double>& frequencies)
{
    const double freqDelta = static_cast<double>(sampleRate) / frequencyTransform.size();

    size_t maxItem = 0;
    for (size_t i = 1; i < frequencyTransform.size() / 2; ++i)
    {
        if (std::abs(frequencyTransform[i]) > std::abs(frequencyTransform[maxItem]))
        {
            maxItem = i;
        }
    }
    const double scale = 1.0 / frequencyTransform.size();
    if (scale * std::abs(frequencyTransform[maxItem]) < 0.01)
    {
        return;
    }

    const double peakPower = std::abs(frequencyTransform[maxItem]) * scale;

    double delta = 0;
    const double threshold = 0.01;
    for (size_t i = 1; i < frequencyTransform.size() / 2; ++i)
    {
        const auto prevFrequency = freqDelta * (i - 1);

        auto prevDelta = delta;
        delta = std::abs(frequencyTransform[i]) - std::abs(frequencyTransform[i - 1]);
        if (prevDelta > 0 && delta < 0 && std::abs(frequencyTransform[i - 1]) * scale > threshold &&
            prevFrequency < 10000)
        {
            logger::debug("%.1fHz, power %.5f, delta %.8f-%.8f",
                "",
                prevFrequency,
                std::abs(frequencyTransform[i - 1]) * scale,
                prevDelta * scale,
                delta * scale);
        }
        if (prevDelta > 0 && delta < 0 && std::abs(frequencyTransform[i - 1]) * scale > threshold &&
            prevDelta * scale > threshold / 2 && std::abs(frequencyTransform[i - 1]) * scale > peakPower / 2)
        {
            frequencies.push_back(prevFrequency);
        }
    }
}

struct Frequency
{
    double freq = 0;
    double power = 0;
};

bool containsNear(std::vector<double>& frequencies, double frequency, double difference = 300)
{
    for (auto& f : frequencies)
    {
        if (std::abs(frequency - f) < difference)
        {
            return true;
        }
    }

    return false;
}

void SampleDataUtils::listPeaks(const CmplxArray& frequencyTransform,
    uint32_t sampleRate,
    std::vector<double>& frequencies,
    const double minPowerdB,
    const double peakWidth)
{
    const double freqDelta = static_cast<double>(sampleRate) / frequencyTransform.size();
    auto freqPowers = powerSpectrum(frequencyTransform);

    std::vector<Frequency> spectrum;

    for (size_t i = 1; i < freqPowers.size() / 2; ++i)
    {
        spectrum.push_back(Frequency{freqDelta * i, freqPowers[i]});
    }

    std::sort(spectrum.begin(), spectrum.end(), [](const Frequency& f0, const Frequency& f1) {
        return f0.power > f1.power;
    });
    if (spectrum[0].power < 0.0001)
    {
        return;
    }

    Frequency peak = spectrum[0];
    const auto dB = 10 * log(peak.power);
    if (dB > minPowerdB && !containsNear(frequencies, peak.freq, peakWidth))
    {
        frequencies.push_back(peak.freq);
        logger::info("1st found %.2fHz, pwr %.6fdB", "", peak.freq, 10 * log(peak.power));
    }
    for (auto& f : spectrum)
    {
        const auto dB = 10 * log(f.power);
        if (dB < minPowerdB)
        {
            break;
        }
        if (f.freq > 30 && dB > minPowerdB && !containsNear(frequencies, f.freq, peakWidth))
        {
            frequencies.push_back({f.freq});
            logger::info("found %.2fHz, pwr %.6fdB", "", f.freq, dB);
        }
    }
}

void SampleDataUtils::topFrequencyPeaks(const CmplxArray& frequencyTransform,
    uint32_t sampleRate,
    uint32_t topN,
    std::vector<double>& frequencies)
{
    const double freqDelta = static_cast<double>(sampleRate) / frequencyTransform.size();

    size_t maxItem = 0;
    for (size_t i = 1; i < frequencyTransform.size() / 2; ++i)
    {
        if (std::abs(frequencyTransform[i]) > std::abs(frequencyTransform[maxItem]))
        {
            maxItem = i;
        }
    }
    const double scale = 1.0 / frequencyTransform.size();
    if (scale * std::abs(frequencyTransform[maxItem]) < 0.01)
    {
        return;
    }

    const double threshold = 0.002;
    double delta = 0;

    const double peakFreq = maxItem * freqDelta;

    for (size_t i = 1; i < frequencyTransform.size() / 2; ++i)
    {
        const auto prevFrequency = freqDelta * (i - 1);

        auto prevDelta = delta;
        delta = std::abs(frequencyTransform[i]) - std::abs(frequencyTransform[i - 1]);
        if (prevDelta > 0 && delta < 0 && std::abs(frequencyTransform[i - 1]) * scale > threshold &&
            prevFrequency < 10000 &&
            (prevFrequency < peakFreq * 0.9 || prevFrequency > peakFreq * 1.1 || prevFrequency == peakFreq))
        {
            /*    logger::debug("%.1fHz, power %.5f, delta %.8f-%.8f",
                    "",
                    prevFrequency,
                    std::abs(frequencyTransform[i - 1]) * scale,
                    prevDelta * scale,
                    delta * scale);*/
        }

        if (prevDelta > 0 && delta < 0 && std::abs(frequencyTransform[i - 1]) * scale > threshold &&
            prevDelta * scale > threshold / 100 &&
            (prevFrequency < peakFreq * 0.9 || prevFrequency > peakFreq * 1.1 || prevFrequency == peakFreq))
        {
            logger::debug("added %.1f power %.5f", "", prevFrequency, std::abs(frequencyTransform[i - 1]) * scale);
            frequencies.push_back(prevFrequency);
        }
    }
}

bool SampleDataUtils::dumpPayload(FILE* h, const memory::Packet& packet)
{
    const auto header = rtp::RtpHeader::fromPacket(packet);
    const auto toWrite = packet.getLength() - header->headerLength();
    return toWrite == ::fwrite(header->getPayload(), 1, toWrite, h);
}

bool SampleDataUtils::dumpPayload(FILE* h, const int16_t* audio, size_t samples)
{
    return samples == ::fwrite(audio, 2, samples, h);
}
