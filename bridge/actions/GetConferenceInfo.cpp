#include "ApiActions.h"
#include "api/ConferenceEndpoint.h"
#include "api/Generator.h"
#include "bridge/Mixer.h"
#include "bridge/RequestLogger.h"
#include "nlohmann/json.hpp"

namespace bridge
{
httpd::Response getConferenceInfo(ActionContext* context,
    RequestLogger& requestLogger,
    const httpd::Request& request,
    const utils::StringTokenizer::Token& incomingToken)
{
    auto token = utils::StringTokenizer::tokenize(incomingToken, '/');
    const auto conferenceId = token.str();
    bridge::Mixer* mixer;
    auto scopedMixerLock = getConferenceMixer(context, conferenceId, mixer);
    nlohmann::json responseBodyJson = nlohmann::json::array();
    for (const auto& id : mixer->getEndpoints())
    {
        api::ConferenceEndpoint endpoint;
        if (mixer->getEndpointInfo(id, endpoint))
        {
            responseBodyJson.push_back(api::Generator::generateConferenceEndpoint(endpoint));
        }
    }

    httpd::Response response(httpd::StatusCode::OK, responseBodyJson.dump(4));
    response._headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}
} // namespace bridge
