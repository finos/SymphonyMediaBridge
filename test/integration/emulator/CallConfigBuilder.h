#pragma once
#include "api/RtcDescriptors.h"
#include "test/integration/emulator/AudioSource.h"
#include "utils/Span.h"
#include <vector>

namespace emulator
{
struct CallConfig
{
    std::string conferenceId;
    std::string baseUrl;
    api::SrtpMode srtpMode = api::SrtpMode::DTLS;
    std::vector<std::string> neighbours;
    Audio audio = Audio::None;
    bool video = false;
    std::string relayType = "ssrc-rewrite";
    bool rtx = true;
    uint32_t idleTimeout = 0;

    bool hasAudio() const { return audio != Audio::None; }
};

class CallConfigBuilder
{
public:
    CallConfigBuilder(std::string conferenceId) { _config.conferenceId = conferenceId; }

    CallConfigBuilder& url(const std::string url)
    {
        _config.baseUrl = url;
        return *this;
    }

    CallConfigBuilder& url(api::SrtpMode mode)
    {
        _config.srtpMode = mode;
        return *this;
    }

    CallConfigBuilder& neighbours(const std::vector<std::string>& n)
    {
        _config.neighbours = n;
        return *this;
    };

    CallConfigBuilder& withAudio()
    {
        _config.audio = Audio::Fake;
        return *this;
    }

    CallConfigBuilder& withOpus()
    {
        _config.audio = Audio::Opus;
        return *this;
    }

    CallConfigBuilder& muted()
    {
        _config.audio = Audio::Muted;
        return *this;
    }

    CallConfigBuilder& withVideo()
    {
        _config.video = true;
        return *this;
    }

    CallConfigBuilder& disableRtx()
    {
        _config.rtx = false;
        return *this;
    }

    CallConfigBuilder& enableRtx()
    {
        _config.rtx = true;
        return *this;
    }

    CallConfigBuilder& mixed()
    {
        _config.relayType = "mixed";
        return *this;
    }

    CallConfigBuilder& idleTimeout(uint32_t idleTimeoutMs)
    {
        _config.idleTimeout = idleTimeoutMs;
        return *this;
    }

    CallConfig build() const { return _config; }

private:
    CallConfig _config;
};
} // namespace emulator
