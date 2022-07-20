#include "transport/dtls/SrtpClient.h"
#include "transport/ice/IceSession.h"

namespace api
{
namespace utils
{
const char* toString(const ice::IceSession::State state);
const ice::IceSession::State stringToIceState(const std::string& state);
const char* toString(const transport::SrtpClient::State state);
const transport::SrtpClient::State stringToDtlsState(const std::string& state);
} // namespace utils
} // namespace api