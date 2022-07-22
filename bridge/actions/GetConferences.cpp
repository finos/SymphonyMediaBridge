#include "ApiActions.h"
#include "api/ConferenceEndpoint.h"
#include "bridge/Mixer.h"
#include "bridge/MixerManager.h"
#include "bridge/RequestLogger.h"
#include "nlohmann/json.hpp"

namespace bridge
{
httpd::Response getConferences(ActionContext* context,
    RequestLogger& requestLogger,
    const httpd::Request&,
    const utils::StringTokenizer::Token&)
{
    nlohmann::json responseBodyJson = nlohmann::json::array();
    for (const auto& mixerId : context->mixerManager.getMixerIds())
    {
        responseBodyJson.push_back(mixerId);
    }

    httpd::Response response(httpd::StatusCode::OK, responseBodyJson.dump(4));
    response._headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}
} // namespace bridge