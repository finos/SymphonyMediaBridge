#pragma once
#include "api/EndpointDescription.h"
#include "bridge/RtpMap.h"
#include "bridge/endpointActions/ApiActions.h"
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
void addDefaultAudioProperties(api::Audio& audio, bool includeTelephoneEvent);
void addVp8VideoProperties(api::Video& video);
void addH264VideoProperties(api::Video& video, const std::string& profileLevelId, const uint32_t packetizationMode);
void addDefaultVideoProperties(api::Video& video);

bridge::RtpMap makeRtpMap(const api::Audio& audio, const api::PayloadType& payloadType);
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
