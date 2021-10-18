#pragma once

#include <string>

namespace utils
{
class Base64
{
public:
    static void encode(std::string& input, uint8_t* output, uint32_t outputLength);
    static void decode(std::string& input, uint8_t* output, uint32_t outputLength);

    static uint32_t encodeLength(std::string& input);
    static uint32_t decodeLength(std::string& input);
};
} // namespace utils
