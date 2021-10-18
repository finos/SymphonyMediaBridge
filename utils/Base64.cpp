#include "utils/Base64.h"
#include <algorithm>
#include <cassert>

namespace utils
{

static const char BASE64_TABLE[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// clang-format off
static const char REVERSE_TABLE[128] = {
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
    64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
    64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64
};
// clang-format on

void Base64::encode(std::string& input, uint8_t* output, uint32_t outputLength)
{
    if (!input.empty())
    {
        assert(outputLength == encodeLength(input));
        if (input.size() > (std::numeric_limits<std::string::size_type>::max() / 4u) * 3u)
        {
            throw std::length_error("Converting too large a string to Base64.");
        }

        uint32_t index = 0;
        uint32_t bitsCollected = 0;
        uint32_t accumulator = 0;

        for (auto i : input)
        {
            accumulator = (accumulator << 8) | (i & 0xffu);
            bitsCollected += 8;
            while (bitsCollected >= 6)
            {
                bitsCollected -= 6;
                output[index++] = BASE64_TABLE[(accumulator >> bitsCollected) & 0x3fu];
            }
        }
        // Any trailing bits that are missing.
        if (bitsCollected > 0)
        {
            assert(bitsCollected < 6);
            accumulator <<= 6 - bitsCollected;
            output[index++] = BASE64_TABLE[accumulator & 0x3fu];
        }
        // Use = signs so the end is properly padded.
        while(index < outputLength)
        {
            output[index++] = '=';
        }
    }
}

void Base64::decode(std::string& input, uint8_t* output, uint32_t outputLength)
{
    if (!input.empty())
    {
        assert(outputLength <= decodeLength(input));
        const auto last = input.end();
        uint32_t bitsCollected = 0;
        uint32_t accumulator = 0;
        uint32_t index = 0;

        for (auto i = input.begin(); i != last; ++i)
        {
            const auto ch = (unsigned char)*i;
            if (std::isspace(ch) || ch == '=')
            {
                // Skip whitespace and padding.
                continue;
            }
            if ((ch > 127) || (ch < 0) || (REVERSE_TABLE[ch] > 63))
            {
                throw std::invalid_argument("This contains characters not legal in a Base64 encoded string.");
            }
            accumulator = (accumulator << 6) | REVERSE_TABLE[ch];
            bitsCollected += 6;
            if (bitsCollected >= 8)
            {
                bitsCollected -= 8;
                output[index++] = static_cast<char>((accumulator >> bitsCollected) & 0xffu);
            }
        }
    }
}

uint32_t Base64::encodeLength(std::string& input)
{
    return 4 * (input.length() + 2) / 3;
}

uint32_t Base64::decodeLength(std::string& input)
{
    auto paddings = std::count(input.begin(), input.end(), '=');
    return (3 * (input.length() / 4)) - paddings;
}
} // namespace utils
