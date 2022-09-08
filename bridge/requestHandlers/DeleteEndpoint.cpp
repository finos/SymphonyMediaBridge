#include "ApiActions.h"
#include "api/ConferenceEndpoint.h"
#include "api/Generator.h"
#include "bridge/Mixer.h"
#include "bridge/RequestLogger.h"
#include "httpd/RequestErrorException.h"
#include "nlohmann/json.hpp"
#include "utils/Format.h"

namespace bridge
{

httpd::Response deleteEndpoint(ActionContext* context,
    RequestLogger& requestLogger,
    const httpd::Request&,
    const ::utils::StringTokenizer::Token& incomingToken)
{
    bridge::Mixer* mixer;
    auto token = utils::StringTokenizer::tokenize(incomingToken, '/');
    const auto conferenceId = token.str();
    auto scopedMixerLock = getConferenceMixer(context, conferenceId, mixer);
    token = utils::StringTokenizer::tokenize(token, '/');
    const auto endpointId = token.str();

    bool removedAny = mixer->removeAudioStream(endpointId);
    removedAny |= mixer->removeVideoStream(endpointId);
    removedAny |= mixer->removeDataStream(endpointId);

    if (!removedAny)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::NOT_FOUND,
            utils::format("Endpoint  '%s'/'%s' not found", conferenceId.c_str(), endpointId.c_str()));
    }

    return httpd::Response(httpd::StatusCode::OK);
}

} // namespace bridge
