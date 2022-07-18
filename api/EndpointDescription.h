#pragma once

#include "utils/Optional.h"
#include <cstdint>
#include <string>
#include <vector>

namespace api
{

struct EndpointDescription
{
    struct Candidate
    {
        uint32_t _generation;
        uint32_t _component;
        std::string _protocol;
        uint32_t _port;
        std::string _ip;
        utils::Optional<uint32_t> _relPort;
        utils::Optional<std::string> _relAddr;
        std::string _foundation;
        uint32_t _priority;
        std::string _type;
        uint32_t _network;
    };

    struct Connection
    {
        uint32_t _port;
        std::string _ip;
    };

    struct Ice
    {
        std::string _ufrag;
        std::string _pwd;
        std::vector<Candidate> _candidates;
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
        bool _rtcpMux;
        utils::Optional<Ice> _ice;
        utils::Optional<Dtls> _dtls;
        utils::Optional<Connection> _connection;
    };

    struct SsrcGroup
    {
        std::vector<uint32_t> _ssrcs;
        std::string _semantics;
    };

    struct PayloadType
    {
        uint32_t _id;
        std::string _name;
        uint32_t _clockRate;
        utils::Optional<uint32_t> _channels;
        std::vector<std::pair<std::string, std::string>> _parameters;
        std::vector<std::pair<std::string, utils::Optional<std::string>>> _rtcpFeedbacks;
    };

    struct SsrcAttribute
    {
        static const char* slidesContent;
        static const char* videoContent;

        std::vector<uint32_t> _ssrcs;
        std::string _content;
    };

    struct Audio
    {
        utils::Optional<Transport> _transport;

        std::vector<uint32_t> _ssrcs;
        utils::Optional<PayloadType> _payloadType;
        std::vector<std::pair<uint32_t, std::string>> _rtpHeaderExtensions;
    };

    struct Video
    {
        utils::Optional<Transport> _transport;

        std::vector<uint32_t> _ssrcs;
        std::vector<SsrcGroup> _ssrcGroups;
        std::vector<PayloadType> _payloadTypes;
        std::vector<std::pair<uint32_t, std::string>> _rtpHeaderExtensions;
        utils::Optional<std::vector<uint32_t>> _ssrcWhitelist;
        std::vector<SsrcAttribute> _ssrcAttributes;
    };

    struct Data
    {
        uint32_t _port;
    };

    std::string _endpointId;

    utils::Optional<Transport> _bundleTransport;

    utils::Optional<Audio> _audio;
    utils::Optional<Video> _video;
    utils::Optional<Data> _data;
};

} // namespace api
