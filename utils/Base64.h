#pragma once

#include <string>

namespace utils
{
class Base64
{
public:
    static std::string encode(const uint8_t* input, uint32_t inputLength);
    static size_t decode(const std::string& input, uint8_t* output, uint32_t outputLength);

    static uint32_t encodeLength(const std::string& input);
    static uint32_t decodeLength(const std::string& input);
    static uint32_t encodeLength(uint32_t inputLength);
};
} // namespace utils
