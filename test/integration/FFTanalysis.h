#pragma once
#include <ccomplex>
#include <cstdint>
#include <valarray>
#include <vector>

void analyzeRecording(const std::vector<int16_t>& recording,
    std::vector<double>& frequencyPeaks,
    std::vector<std::pair<uint64_t, double>>& amplitudeProfile,
    const char* logId,
    uint32_t sampleRate = 48000,
    uint64_t cutAtTime = 0);

std::valarray<std::complex<double>> createAudioSpectrum(const std::vector<int16_t>& recording, uint32_t sampleRate);
