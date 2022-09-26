#include "FFTanalysis.h"
#include "codec/Opus.h"
#include "logger/Logger.h"
#include "test/integration/SampleDataUtils.h"
#include "utils/Trackers.h"
#include <ccomplex>
#include <thread>
#include <valarray>

void fftThreadRun(const std::vector<int16_t>& recording,
    std::vector<double>& frequencies,
    const size_t fftWindowSize,
    size_t size,
    size_t numThreads,
    size_t threadId)
{
    for (size_t cursor = 256 * threadId; cursor < size - fftWindowSize; cursor += 256 * numThreads)
    {
        std::valarray<std::complex<double>> testVector(fftWindowSize);
        for (uint64_t x = 0; x < fftWindowSize; ++x)
        {
            testVector[x] = std::complex<double>(static_cast<double>(recording[x + cursor]), 0.0) / (256.0 * 128);
        }

        SampleDataUtils::fft(testVector);
        SampleDataUtils::listFrequencies(testVector, codec::Opus::sampleRate, frequencies);
    }
}

void analyzeRecording(const std::vector<int16_t>& recording,
    std::vector<double>& frequencyPeaks,
    std::vector<std::pair<uint64_t, double>>& amplitudeProfile,
    const char* logId,
    uint64_t cutAtTime)
{
    utils::RateTracker<5> amplitudeTracker(codec::Opus::sampleRate / 10);
    const size_t fftWindowSize = 2048;

    const auto limit = cutAtTime == 0 ? recording.size() : cutAtTime * codec::Opus::sampleRate / utils::Time::ms;
    const auto size = recording.size() > limit ? limit : recording.size();

    for (size_t t = 0; t < size; ++t)
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

    if (size < fftWindowSize)
    {
        return;
    }

    auto start = std::chrono::high_resolution_clock::now();

    auto const numThreads = std::max(std::thread::hardware_concurrency(), 4U);
    std::vector<std::thread> workers;
    std::vector<std::vector<double>> frequencies;
    frequencies.reserve(numThreads);

    for (size_t threadId = 0; threadId < numThreads; threadId++)
    {
        frequencies.push_back(std::vector<double>());
        workers.push_back(std::thread(fftThreadRun,
            std::ref(recording),
            std::ref(frequencies[threadId]),
            fftWindowSize,
            size,
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
            if (std::find(frequencyPeaks.begin(), frequencyPeaks.end(), frequencies[threadId][i]) ==
                frequencyPeaks.end())
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
