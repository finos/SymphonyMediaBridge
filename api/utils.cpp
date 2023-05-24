#include "utils.h"

namespace api
{
namespace utils
{

api::utils::EnumRef<ice::IceSession::State> iceStates[] = {{"unknown", ice::IceSession::State::LAST},
    {"IDLE", ice::IceSession::State::IDLE},
    {"GATHERING", ice::IceSession::State::GATHERING},
    {"READY", ice::IceSession::State::READY},
    {"CONNECTING", ice::IceSession::State::CONNECTING},
    {"CONNECTED", ice::IceSession::State::CONNECTED},
    {"FAILED", ice::IceSession::State::FAILED}};

const char* toString(ice::IceSession::State state)
{
    return toString(state, iceStates);
}

ice::IceSession::State stringToIceState(const std::string& state)
{
    return fromString(state, iceStates);
}

api::utils::EnumRef<transport::SrtpClient::State> dtlsStates[] = {{"unknown", transport::SrtpClient::State::LAST},
    {"IDLE", transport::SrtpClient::State::IDLE},
    {"READY", transport::SrtpClient::State::READY},
    {"CONNECTED", transport::SrtpClient::State::CONNECTED},
    {"CONNECTING", transport::SrtpClient::State::CONNECTING},
    {"FAILED", transport::SrtpClient::State::FAILED}};

const char* toString(transport::SrtpClient::State state)
{
    return toString(state, dtlsStates);
}

transport::SrtpClient::State stringToDtlsState(const std::string& state)
{
    return fromString(state, dtlsStates);
}

api::utils::EnumRef<srtp::Profile> sdesProfiles[] = {{"", srtp::Profile::NULL_CIPHER},
    {"AES_128_CM_HMAC_SHA1_80", srtp::Profile::AES128_CM_SHA1_80},
    {"AES_128_CM_HMAC_SHA1_32", srtp::Profile::AES128_CM_SHA1_32},
    {"AES_256_CM_HMAC_SHA1_80", srtp::Profile::AES_256_CM_SHA1_80},
    {"AES_256_CM_HMAC_SHA1_32", srtp::Profile::AES_256_CM_SHA1_32},
    {"AES_192_CM_HMAC_SHA1_80", srtp::Profile::AES_192_CM_SHA1_80},
    {"AES_192_CM_HMAC_SHA1_32", srtp::Profile::AES_192_CM_SHA1_32},
    {"AEAD_AES_128_GCM", srtp::Profile::AEAD_AES_128_GCM},
    {"AEAD_AES_256_GCM", srtp::Profile::AEAD_AES_256_GCM}};

const char* toString(srtp::Profile profile)
{
    return toString(profile, sdesProfiles);
}

srtp::Profile stringToSrtpProfile(const std::string& profile)
{
    return fromString(profile, sdesProfiles);
}

} // namespace utils
} // namespace api
