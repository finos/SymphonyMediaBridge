#pragma once

#include <cstdint>
#include <string>

namespace api
{

struct RecordingChannel
{
    std::string id;
    std::string host;
    uint16_t port;
    uint8_t aesKey[32];
    uint8_t aesSalt[12];
};

} // namespace api
