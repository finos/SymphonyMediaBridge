#pragma once

#include "bridge/RtpMap.h"
#include "bridge/engine/SimulcastStream.h"
#include <cstdint>
#include <vector>

namespace legacyapi
{
struct Channel;
struct SsrcGroup;
} // namespace legacyapi

namespace bridge
{

namespace LegacyApiRequestHandlerHelpers
{

std::vector<bridge::RtpMap> makeRtpMaps(const legacyapi::Channel& channel);
std::vector<bridge::SimulcastStream> makeSimulcastStreams(const legacyapi::Channel& channel);

} // namespace LegacyApiRequestHandlerHelpers

} // namespace bridge
