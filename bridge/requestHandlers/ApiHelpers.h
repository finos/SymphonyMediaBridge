#pragma once
#include "ApiActions.h"
#include "api/EndpointDescription.h"
#include "bridge/RtpMap.h"
#include "bridge/engine/SimulcastStream.h"
#include "bridge/engine/SsrcWhitelist.h"
#include <mutex>
#include <vector>

namespace ice
{
class IceCandidate;
}

namespace bridge
{

std::pair<std::vector<ice::IceCandidate>, std::pair<std::string, std::string>> getIceCandidatesAndCredentials(
    const api::EndpointDescription::Transport&);
std::unique_lock<std::mutex> getConferenceMixer(ActionContext*,
    const std::string& conferenceId,
    bridge::Mixer*& outMixer);
api::EndpointDescription::Candidate iceCandidateToApi(const ice::IceCandidate&);
void addDefaultAudioProperties(api::EndpointDescription::Audio&);
void addDefaultVideoProperties(api::EndpointDescription::Video&);

bridge::RtpMap makeRtpMap(const api::EndpointDescription::Audio& audio);
bridge::RtpMap makeRtpMap(const api::EndpointDescription::Video& video,
    const api::EndpointDescription::PayloadType& payloadType);

utils::Optional<uint8_t> findAbsSendTimeExtensionId(
    const std::vector<std::pair<uint32_t, std::string>>& rtpHeaderExtensions);
utils::Optional<uint8_t> findC9InfoExtensionId(
    const std::vector<std::pair<uint32_t, std::string>>& rtpHeaderExtensions);
utils::Optional<uint8_t> findAudioLevelExtensionId(
    const std::vector<std::pair<uint32_t, std::string>>& rtpHeaderExtensions);
std::vector<bridge::SimulcastStream> makeSimulcastStreams(const api::EndpointDescription::Video&,
    const std::string& endpointId);
bridge::SsrcWhitelist makeWhitelistedSsrcsArray(const api::EndpointDescription::Video&);
} // namespace bridge
