
#include <stdio.h>
#include <array>
#include <string>

namespace utils
{

__attribute__((format(printf, 1, 2)))
inline std::string format(const char* format, ...)
{
    std::array<char, 2048> buff;
    std::string formattedValue;
    va_list args;
    va_start(args, format);
    const auto stringLen = vsnprintf(buff.data(), buff.size(), format, args);
    // stringLen not count null terminor
    if (stringLen >= 0 && static_cast<uint32_t>(stringLen) < buff.size())
    {
        formattedValue.append(buff.begin(), buff.begin() + stringLen);
    }
    else if (stringLen >= 0)
    {
        // When this happens is because it not fit in buff.size(). But as we already know the stringLen
        // we are going try again directly in formattedValue

        // We only need to resize to stringLen, std::string will reserve the space for the null terminator
        formattedValue.resize(stringLen);
        vsnprintf(&formattedValue.front() , stringLen + 1, format, args);
    }

    va_end(args);
    return formattedValue;
}

} // namespace utils