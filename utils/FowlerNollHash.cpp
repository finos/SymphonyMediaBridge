#include "FowlerNollHash.h"
namespace utils
{
size_t FowlerNollVoHash(const void* data, size_t length)
{
    auto p = reinterpret_cast<const uint8_t*>(data);
    size_t result = 0x811C9DC5; // 10427 × 207743

    for (size_t i = 0; i < length; ++i)
    {
        // x 16777619 (prime)
        result = (result * 0x1000193) ^ *(p++);
    }

    return result;
}
} // namespace utils