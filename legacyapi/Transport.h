#pragma once

#include "legacyapi/Candidate.h"
#include "legacyapi/Connection.h"
#include "legacyapi/Fingerprint.h"
#include "utils/Optional.h"
#include <vector>

namespace legacyapi
{

struct Transport
{
    utils::Optional<std::string> _xmlns;
    bool _rtcpMux;
    utils::Optional<std::string> _ufrag;
    utils::Optional<std::string> _pwd;
    std::vector<Fingerprint> _fingerprints;
    std::vector<Candidate> _candidates;
    utils::Optional<Connection> _connection;
};

} // namespace legacyapi
