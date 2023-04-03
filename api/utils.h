#pragma once
#include "transport/dtls/SrtpClient.h"
#include "transport/ice/IceSession.h"

namespace api
{
namespace utils
{
const char* toString(ice::IceSession::State state);
ice::IceSession::State stringToIceState(const std::string& state);
const char* toString(transport::SrtpClient::State state);
transport::SrtpClient::State stringToDtlsState(const std::string& state);
const char* toString(srtp::Profile profile);
srtp::Profile stringToSrtpProfile(const std::string& profile);

template <typename T>
struct EnumRef
{
    const char* name;
    T value;
};

// make sure dictionary contains default / null value at position 0
template <typename T, size_t N>
T fromString(const std::string& name, const EnumRef<T> (&dictionary)[N])
{
    for (size_t i = 0; i < N; ++i)
    {
        if (name == dictionary[i].name)
        {
            return dictionary[i].value;
        }
    }
    return dictionary[0].value;
}

template <typename T, size_t N>
const char* toString(T enumValue, const EnumRef<T> (&dictionary)[N])
{
    for (size_t i = 0; i < N; ++i)
    {
        if (enumValue == dictionary[i].value)
        {
            return dictionary[i].name;
        }
    }
    return dictionary[0].name;
}

} // namespace utils
} // namespace api
