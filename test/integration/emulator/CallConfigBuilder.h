#pragma once
#include "api/RtcDescriptors.h"
#include "test/integration/emulator/AudioSource.h"
#include "utils/Span.h"
#include "utils/Time.h"
#include <vector>

namespace emulator
{
struct CallConfig
{
    std::string conferenceId;
    std::string baseUrl;
    bool dtls = true;
    bool sdes = false;
    std::vector<std::string> neighbours;
    Audio audio = Audio::None;
    bool video = false;
    std::string relayType = "ssrc-rewrite";
    bool rtx = true;
    uint32_t idleTimeout = 0;
    uint64_t ipv6CandidateDelay = 0;

    bool hasAudio() const { return audio != Audio::None; }
};

class CallConfigBuilder
{
public:
    CallConfigBuilder(std::string conferenceId) { _config.conferenceId = conferenceId; }

    CallConfigBuilder& room(const std::string id)
    {
        _config.conferenceId = id;
        return *this;
    }

    CallConfigBuilder& url(const std::string url)
    {
        _config.baseUrl = url;
        return *this;
    }

    CallConfigBuilder& disableDtls()
    {
        _config.dtls = false;
        return *this;
    }

    CallConfigBuilder& sdes()
    {
        _config.sdes = true;
        return *this;
    }

    CallConfigBuilder& neighbours(const std::vector<std::string>& n)
    {
        _config.neighbours.clear();
        _config.neighbours = n;
        return *this;
    }

    CallConfigBuilder& neighbours(const utils::Span<std::string>& groups)
    {
        _config.neighbours.clear();
        for (auto n : groups)
        {
            _config.neighbours.push_back(n);
        }
        return *this;
    }

    CallConfigBuilder& av()
    {
        _config.audio = Audio::Opus;
        _config.video = true;
        return *this;
    }

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

    CallConfigBuilder& noVideo()
    {
        _config.video = false;
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

    CallConfigBuilder& idleTimeout(uint32_t seconds)
    {
        _config.idleTimeout = seconds;
        return *this;
    }

    CallConfigBuilder& delayIpv6(uint32_t ms)
    {
        _config.ipv6CandidateDelay = ms * utils::Time::ms;
        return *this;
    }

    CallConfig build() const { return _config; }

private:
    CallConfig _config;
};
} // namespace emulator
