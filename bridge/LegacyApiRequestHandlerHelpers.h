#pragma once

#include <vector>

namespace legacyapi
{
struct Channel;
} // namespace legacyapi

namespace bridge
{
struct RtpMap;
struct SimulcastStream;

namespace LegacyApiRequestHandlerHelpers
{

std::vector<bridge::RtpMap> makeRtpMaps(const legacyapi::Channel& channel);
std::vector<bridge::SimulcastStream> makeSimulcastStreams(const legacyapi::Channel& channel);

} // namespace LegacyApiRequestHandlerHelpers

} // namespace bridge
