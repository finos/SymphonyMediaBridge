#pragma once
#include <cstdint>
#include <vector>

void analyzeRecording(const std::vector<int16_t>& recording,
    std::vector<double>& frequencyPeaks,
    std::vector<std::pair<uint64_t, double>>& amplitudeProfile,
    const char* logId,
    uint64_t cutAtTime = 0);
