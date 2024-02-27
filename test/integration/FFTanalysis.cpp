#include "FFTanalysis.h"
#include "codec/Opus.h"
#include "logger/Logger.h"
#include "test/integration/SampleDataUtils.h"
#include "utils/Trackers.h"
#include <ccomplex>
#include <thread>
#include <valarray>

using CmplxArray = std::valarray<std::complex<double>>;

void fftThreadRun(const std::vector<int16_t>& recording,
    std::vector<double>& frequencies,
    const size_t fftWindowSize,
    size_t size,
    uint32_t sampleRate,
    size_t numThreads,
    size_t threadId)
{
    for (size_t cursor = 256 * threadId; cursor < size - fftWindowSize; cursor += 256 * numThreads)
    {
        CmplxArray testVector(fftWindowSize);
        for (uint64_t x = 0; x < fftWindowSize; ++x)
        {
            testVector[x] = std::complex<double>(static_cast<double>(recording[x + cursor]), 0.0) / (256.0 * 128);
        }

        SampleDataUtils::applyHannWindow(testVector);
        SampleDataUtils::fft(testVector);
        SampleDataUtils::listPeaks(testVector, sampleRate, frequencies, -40, 90);
    }
}

void analyzeRecording(const std::vector<int16_t>& recording,
    std::vector<double>& frequencyPeaks,
    std::vector<std::pair<uint64_t, double>>& amplitudeProfile,
    const char* logId,
    uint32_t sampleRate,
    uint64_t cutAtTime)
{
    utils::RateTracker<5> amplitudeTracker(sampleRate / 10);
    const size_t fftWindowSize = 2048;

    const auto limit = cutAtTime == 0 ? recording.size() : cutAtTime * sampleRate / utils::Time::ms;
    const auto size = recording.size() > limit ? limit : recording.size();

    for (size_t t = 0; t < size; ++t)
    {
        amplitudeTracker.update(std::abs(recording[t]), t);
        if (t > sampleRate / 10)
        {
            if (amplitudeProfile.empty() ||
                (t - amplitudeProfile.back().first > sampleRate / 10 &&
                    std::abs(amplitudeProfile.back().second - amplitudeTracker.get(t, sampleRate / 5)) >
                        amplitudeProfile.back().second * 0.2))
            {
                amplitudeProfile.push_back(std::make_pair(t, amplitudeTracker.get(t, sampleRate / 5)));
            }
        }
    }

    if (size < fftWindowSize)
    {
        return;
    }

    auto start = std::chrono::high_resolution_clock::now();
#ifdef LCHECK_BUILD
    size_t const numThreads = 2;
#else
    auto const numThreads = std::max(std::thread::hardware_concurrency(), 4U);
#endif

    std::vector<std::thread> workers;
    std::vector<std::vector<double>> frequencies;
    frequencies.reserve(numThreads);
    workers.reserve(numThreads);

    for (size_t threadId = 0; threadId < numThreads; threadId++)
    {
        frequencies.emplace_back();
        workers.push_back(std::thread(fftThreadRun,
            std::ref(recording),
            std::ref(frequencies[threadId]),
            fftWindowSize,
            size,
            sampleRate,
            numThreads,
            threadId));
    }

    for (size_t threadId = 0; threadId < numThreads; threadId++)
    {
        workers[threadId].join();
    }

    for (size_t threadId = 0; threadId < numThreads; threadId++)
    {
        for (size_t i = 0; i < frequencies[threadId].size() && i < 50; ++i)
        {
            if (std::find_if(frequencyPeaks.begin(), frequencyPeaks.end(), [&](double f) {
                    return std::abs(f - frequencies[threadId][i]) < 90;
                }) == frequencyPeaks.end())
            {
                logger::debug("added new freq %.3f", logId, frequencies[threadId][i]);
                frequencyPeaks.push_back(frequencies[threadId][i]);
            }
        }
    }

    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
    logger::debug("Analysis complete in %lld us", "Threaded FFT", duration.count());
}

void fftProducer(const std::vector<int16_t>& recording,
    const size_t fftWindowSize,
    const size_t size,
    const uint32_t sampleRate,
    const size_t numThreads,
    const size_t threadId,
    CmplxArray& spectrum)
{
    for (size_t cursor = fftWindowSize * threadId; cursor < size - fftWindowSize; cursor += fftWindowSize * numThreads)
    {
        CmplxArray testVector(fftWindowSize);
        for (size_t x = 0; x < fftWindowSize; ++x)
        {
            testVector[x] = std::complex<double>(static_cast<double>(recording[x + cursor]), 0.0) / (256.0 * 128);
        }

        SampleDataUtils::applyHannWindow(testVector);
        SampleDataUtils::fft(testVector);

        spectrum += testVector;
    }
}

CmplxArray createAudioSpectrum(const std::vector<int16_t>& recording, uint32_t sampleRate)
{
    const size_t fftWindowSize = 2048;
    size_t numThreads = std::max(std::thread::hardware_concurrency(), 2U);
#ifdef LCHECK_BUILD
    numThreads = 2;
#endif
    std::vector<std::thread> workers;

    std::vector<CmplxArray> spectrums;
    spectrums.reserve(numThreads);

    for (size_t threadId = 0; threadId < numThreads; threadId++)
    {
        spectrums.push_back(CmplxArray(fftWindowSize));

        workers.push_back(std::thread(fftProducer,
            std::ref(recording),
            fftWindowSize,
            recording.size(),
            sampleRate,
            numThreads,
            threadId,
            std::ref(spectrums[threadId])));
    }

    for (size_t threadId = 0; threadId < numThreads; threadId++)
    {
        workers[threadId].join();
    }

    CmplxArray spectrum(fftWindowSize);
    for (size_t threadId = 0; threadId < numThreads; threadId++)
    {
        spectrum += spectrums[threadId];
    }

    spectrum *= 1.63; // energy correction for Hann window
    int count = recording.size() / fftWindowSize;
    spectrum /= count;

    return spectrum;
}
