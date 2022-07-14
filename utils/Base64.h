#pragma once

#include <string>

namespace utils
{
class Base64
{
public:
    static void encode(const std::string& input, uint8_t* output, uint32_t outputLength);
    static void decode(const std::string& input, uint8_t* output, uint32_t outputLength);

    static uint32_t encodeLength(const std::string& input);
    static uint32_t decodeLength(const std::string& input);
};
} // namespace utils
