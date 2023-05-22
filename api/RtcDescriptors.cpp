#include "RtcDescriptors.h"
#include "api/utils.h"

namespace api
{
const char* VideoStream::slidesContent = "slides";
const char* VideoStream::videoContent = "video";

utils::EnumRef<SrtpMode> srtpModes[] = {{"DISABLED", SrtpMode::Disabled},
    {"DTLS", SrtpMode::DTLS},
    {"SDES", SrtpMode::SDES}};

SrtpMode stringToSrtpMode(const std::string& s)
{
    return fromString(s, srtpModes);
}

std::string toString(SrtpMode mode)
{
    return toString(mode, srtpModes);
}

} // namespace api
