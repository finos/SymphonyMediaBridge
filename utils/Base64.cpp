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

std::string Base64::encode(const uint8_t* input, const uint32_t length)
{
    std::string encodedText;
    if (length > 0)
    {
        auto outputLength = encodeLength(length);

        uint32_t bitsCollected = 0;
        uint32_t accumulator = 0;

        for (const uint8_t* p = input; p != input + length; ++p)
        {
            accumulator = (accumulator << 8) | (*p & 0xffu);
            bitsCollected += 8;
            while (bitsCollected >= 6)
            {
                bitsCollected -= 6;
                encodedText += BASE64_TABLE[(accumulator >> bitsCollected) & 0x3fu];
            }
        }
        // Any trailing bits that are missing.
        if (bitsCollected > 0)
        {
            assert(bitsCollected < 6);
            accumulator <<= 6 - bitsCollected;
            encodedText += BASE64_TABLE[accumulator & 0x3fu];
        }
        // Use = signs so the end is properly padded.
        while (encodedText.size() < outputLength)
        {
            encodedText += '=';
        }
    }

    return encodedText;
}

size_t Base64::decode(const std::string& input, uint8_t* output, const uint32_t outputLength)
{
    if (!input.empty())
    {
        const auto last = input.end();
        uint32_t bitsCollected = 0;
        uint32_t accumulator = 0;
        uint32_t index = 0;

        for (auto i = input.begin(); i != last && index < outputLength; ++i)
        {
            const auto ch = (unsigned char)*i;
            if (std::isspace(ch) || ch == '=')
            {
                // Skip whitespace and padding.
                continue;
            }
            if ((ch > 127) || (ch < 0) || (REVERSE_TABLE[ch] > 63))
            {
                return 0;
            }
            accumulator = (accumulator << 6) | REVERSE_TABLE[ch];
            bitsCollected += 6;
            if (bitsCollected >= 8)
            {
                bitsCollected -= 8;
                output[index++] = static_cast<char>((accumulator >> bitsCollected) & 0xffu);
            }
        }

        return index;
    }
    return 0;
}

uint32_t Base64::encodeLength(const std::string& input)
{
    return encodeLength(input.length());
}

uint32_t Base64::decodeLength(const std::string& input)
{
    auto paddings = std::count(input.begin(), input.end(), '=');
    return (3 * (input.length() / 4)) - paddings;
}

uint32_t Base64::encodeLength(uint32_t inputLength)
{
    return 4 * (inputLength + 2) / 3;
}

} // namespace utils
