#pragma once

#include <cstdint>
#include <string>

namespace api
{

struct RecordingChannel
{
    std::string _id;
    std::string _host;
    uint16_t _port;
    uint8_t _aesKey[32];
    uint8_t _aesSalt[12];
};

} // namespace api
