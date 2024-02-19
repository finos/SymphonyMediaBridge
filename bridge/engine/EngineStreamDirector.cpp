#include "EngineStreamDirector.h"
namespace
{
constexpr uint32_t calculateMaxBitrate(uint32_t base, uint32_t overhead)
{
    // Assuming a _lastN of 9 participants. Perhaps this should be dynamic as EngineStreamDirector knows _lastN
    return base + 8 * overhead;
}

} // namespace

using namespace bridge;

// Configuration based on
// https://chromium.googlesource.com/external/webrtc/+/refs/heads/master/video/config/simulcast.cc
// Calculation based on following assumptions:
// - Video at 25fps
// - RTP header 12 bytes + 8 byte extension header (abs-time extension)
// - Until 9 bytes for VP8 payload descriptor + VP8 payload header
// - LOW_QUALITY resolution: 320x180. Target bitrate 150kbits/s, 25 packets/s (~750 bytes per packet + headers)
// - MID_QUALITY resolution: 640x360. Target bitrate 500kbits/s, 50 packets/s (~1250 bytes per packet + headers)
// - HIGH_QUALITY resolution: 1280x720. Target bitrate 2500kbits/s, 225 packets/s (~ 1389 bytes per packet + headers)
// - Round up as the layer selection does not take into account sending audio nor RTCP packets
constexpr const uint32_t EngineStreamDirector::LOW_QUALITY_BITRATE = 160;
constexpr const uint32_t EngineStreamDirector::MID_QUALITY_BITRATE = 520;
constexpr const uint32_t EngineStreamDirector::HIGH_QUALITY_BITRATE = 2600;

constexpr EngineStreamDirector::ConfigRow EngineStreamDirector::configLadder[6] = {
    // BaseRate = 0, pinnedQuality, unpinnedQuality, overheadBitrate, minBitrateMargin, maxBitrateMargin
    {HIGH_QUALITY_BITRATE - MID_QUALITY_BITRATE,
        highQuality,
        midQuality,
        MID_QUALITY_BITRATE,
        HIGH_QUALITY_BITRATE,
        calculateMaxBitrate(HIGH_QUALITY_BITRATE, MID_QUALITY_BITRATE)}, // 6760
    {MID_QUALITY_BITRATE - MID_QUALITY_BITRATE,
        midQuality,
        midQuality,
        MID_QUALITY_BITRATE,
        MID_QUALITY_BITRATE,
        calculateMaxBitrate(MID_QUALITY_BITRATE, MID_QUALITY_BITRATE)}, // 4680
    {MID_QUALITY_BITRATE - LOW_QUALITY_BITRATE,
        midQuality,
        lowQuality,
        LOW_QUALITY_BITRATE,
        MID_QUALITY_BITRATE,
        calculateMaxBitrate(MID_QUALITY_BITRATE, LOW_QUALITY_BITRATE)}, // 1800
    {LOW_QUALITY_BITRATE - LOW_QUALITY_BITRATE,
        lowQuality,
        lowQuality,
        LOW_QUALITY_BITRATE,
        LOW_QUALITY_BITRATE,
        calculateMaxBitrate(LOW_QUALITY_BITRATE, LOW_QUALITY_BITRATE)}, // 1440
    {LOW_QUALITY_BITRATE, lowQuality, dropQuality, 0, LOW_QUALITY_BITRATE, LOW_QUALITY_BITRATE},
    {0, dropQuality, dropQuality, 0, 0, 0}};
