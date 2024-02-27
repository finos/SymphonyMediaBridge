#pragma once

#include "api/SimulcastGroup.h"
#include "transport/dtls/SrtpProfiles.h"
#include "utils/Optional.h"
#include <cstdint>
#include <string>
#include <vector>

namespace api
{

enum SrtpMode
{
    Disabled = 0,
    DTLS,
    SDES
};

SrtpMode stringToSrtpMode(const std::string& s);
std::string toString(SrtpMode mode);

struct Candidate
{
    uint32_t generation;
    uint32_t component;
    std::string protocol;
    uint32_t port;
    std::string ip;
    utils::Optional<uint32_t> relPort;
    utils::Optional<std::string> relAddr;
    std::string foundation;
    uint32_t priority;
    std::string type;
    uint32_t network;
};

struct Connection
{
    uint32_t port;
    std::string ip;
};

struct Ice
{
    std::string ufrag;
    std::string pwd;
    std::vector<Candidate> candidates;
};

struct Dtls
{
    std::string setup;
    std::string type;
    std::string hash;

    bool isClient() const { return setup.compare("active") == 0; }
};

struct Transport
{
    bool rtcpMux;
    utils::Optional<Ice> ice;
    utils::Optional<Dtls> dtls;
    utils::Optional<Connection> connection;
    std::vector<srtp::AesKey> sdesKeys;

    srtp::Mode getSrtpMode() const
    {
        if (dtls.isSet())
        {
            return srtp::Mode::DTLS;
        }
        else if (!sdesKeys.empty())
        {
            return srtp::Mode::SDES;
        }
        else
        {
            return srtp::Mode::NULL_CIPHER;
        }
    }
};

struct VideoStream
{
    static const char* slidesContent;
    static const char* videoContent;

    std::string id;
    api::SimulcastGroup sources;
    std::string content;

    bool isSlides() const { return 0 == content.compare(slidesContent); }
};

struct PayloadType
{
    uint32_t id;
    std::string name;
    uint32_t clockRate;
    utils::Optional<uint32_t> channels;
    std::vector<std::pair<std::string, std::string>> parameters;
    std::vector<std::pair<std::string, utils::Optional<std::string>>> rtcpFeedbacks;
};

struct Audio
{
    utils::Optional<Transport> transport;

    std::vector<uint32_t> ssrcs;
    std::vector<PayloadType> payloadTypes;
    std::vector<std::pair<uint32_t, std::string>> rtpHeaderExtensions;
};

struct Video
{
    utils::Optional<Transport> transport;

    std::vector<uint32_t> getSsrcs() const
    {
        std::vector<uint32_t> ssrcs;
        for (auto& stream : streams)
        {
            for (auto& level : stream.sources)
            {
                ssrcs.push_back(level.main);
                ssrcs.push_back(level.feedback);
            }
        }
        return ssrcs;
    }

    std::vector<VideoStream> streams;
    std::vector<PayloadType> payloadTypes;
    std::vector<std::pair<uint32_t, std::string>> rtpHeaderExtensions;
    utils::Optional<std::vector<uint32_t>> ssrcWhitelist;
};

struct Data
{
    uint32_t port;
};
} // namespace api
