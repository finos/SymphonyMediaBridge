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
httpd::Response getEndpointInfo(ActionContext* context,
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

    const auto activeTalkers = mixer->getActiveTalkers();

    const auto endpoints = mixer->getEndpoints();
    const auto& endpoint = endpoints.find(endpointId);
    if (endpoint == endpoints.end())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::NOT_FOUND,
            utils::format("Endpoint  '%s'/'%s' not found", conferenceId.c_str(), endpointId.c_str()));
    }
    api::ConferenceEndpointExtendedInfo endpointInfo;
    if (mixer->getEndpointExtendedInfo(*endpoint, endpointInfo, activeTalkers))
    {
        auto responseBodyJson = api::Generator::generateExtendedConferenceEndpoint(endpointInfo);
        httpd::Response response(httpd::StatusCode::OK, responseBodyJson.dump(4));
        response._headers["Content-type"] = "text/json";
        requestLogger.setResponse(response);
        return response;
    }

    throw httpd::RequestErrorException(httpd::StatusCode::NOT_FOUND,
        utils::format("Conference '%s' not found", conferenceId.c_str()));
}

} // namespace bridge
