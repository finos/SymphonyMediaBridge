#include "utils.h"

namespace api
{
namespace utils
{

const char* toString(const ice::IceSession::State state)
{
    switch (state)
    {
    case ice::IceSession::State::IDLE:
        return "IDLE";
    case ice::IceSession::State::GATHERING:
        return "GATHERING";
    case ice::IceSession::State::READY:
        return "READY";
    case ice::IceSession::State::CONNECTING:
        return "CONNECTING";
    case ice::IceSession::State::CONNECTED:
        return "CONNECTED";
    case ice::IceSession::State::FAILED:
        return "FAILED";
    default:
        return "unknown";
    }
}

const ice::IceSession::State stringToIceState(const std::string& state)
{
    if (state == "IDLE")
        return ice::IceSession::State::IDLE;
    if (state == "GATHERING")
        return ice::IceSession::State::GATHERING;
    if (state == "READY")
        return ice::IceSession::State::READY;
    if (state == "CONNECTING")
        return ice::IceSession::State::CONNECTING;
    if (state == "CONNECTED")
        return ice::IceSession::State::CONNECTED;
    if (state == "FAILED")
        return ice::IceSession::State::FAILED;
    return ice::IceSession::State::LAST;
}

const char* toString(const transport::SrtpClient::State state)
{
    switch (state)
    {
    case transport::SrtpClient::State::IDLE:
        return "IDLE";
    case transport::SrtpClient::State::READY:
        return "READY";
    case transport::SrtpClient::State::CONNECTED:
        return "CONNECTED";
    case transport::SrtpClient::State::CONNECTING:
        return "CONNECTING";
    case transport::SrtpClient::State::FAILED:
        return "FAILED";
    default:
        return "unknown";
    }
}

const transport::SrtpClient::State stringToDtlsState(const std::string& state)
{
    if (state == "IDLE")
        return transport::SrtpClient::State::IDLE;
    if (state == "READY")
        return transport::SrtpClient::State::READY;
    if (state == "CONNECTED")
        return transport::SrtpClient::State::CONNECTED;
    if (state == "CONNECTING")
        return transport::SrtpClient::State::CONNECTING;
    if (state == "FAILED")
        return transport::SrtpClient::State::FAILED;
    return transport::SrtpClient::State::LAST;
}
} // namespace utils
} // namespace api