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
    const api::Transport&);
std::pair<std::vector<ice::IceCandidate>, std::pair<std::string, std::string>> getIceCandidatesAndCredentials(
    const api::Ice& ice);
std::unique_lock<std::mutex> getConferenceMixer(ActionContext*,
    const std::string& conferenceId,
    bridge::Mixer*& outMixer);
api::Candidate iceCandidateToApi(const ice::IceCandidate&);
void addDefaultAudioProperties(api::Audio&);
void addDefaultVideoProperties(api::Video&);

bridge::RtpMap makeRtpMap(const api::Audio& audio);
bridge::RtpMap makeRtpMap(const api::Video& video, const api::PayloadType& payloadType);

utils::Optional<uint8_t> findAbsSendTimeExtensionId(
    const std::vector<std::pair<uint32_t, std::string>>& rtpHeaderExtensions);
utils::Optional<uint8_t> findC9InfoExtensionId(
    const std::vector<std::pair<uint32_t, std::string>>& rtpHeaderExtensions);
utils::Optional<uint8_t> findAudioLevelExtensionId(
    const std::vector<std::pair<uint32_t, std::string>>& rtpHeaderExtensions);
std::vector<bridge::SimulcastStream> makeSimulcastStreams(const api::Video&, const std::string& endpointId);
bridge::SsrcWhitelist makeWhitelistedSsrcsArray(const api::Video&);
} // namespace bridge
