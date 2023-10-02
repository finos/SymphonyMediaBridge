#include "ApiActions.h"
#include "api/ConferenceEndpoint.h"
#include "bridge/Mixer.h"
#include "bridge/MixerManager.h"
#include "bridge/RequestLogger.h"
#include "nlohmann/json.hpp"

namespace bridge
{
httpd::Response getConferences(ActionContext* context, RequestLogger& requestLogger)
{
    nlohmann::json responseBodyJson = nlohmann::json::array();
    for (const auto& mixerId : context->mixerManager.getMixerIds())
    {
        responseBodyJson.push_back(mixerId);
    }

    httpd::Response response(httpd::StatusCode::OK, responseBodyJson.dump(4));
    response.headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}

httpd::Response fetchBriefConferenceList(ActionContext* context, RequestLogger& requestLogger)
{
    nlohmann::json responseBodyJson = nlohmann::json::array();
    auto& mixerManager = context->mixerManager;

    auto mixerIds = mixerManager.getMixerIds();

    for (const auto& mixerId : mixerIds)
    {
        Mixer* mixer;
        auto mixerLock = context->mixerManager.getMixer(mixerId, mixer);
        if (mixer)
        {
            nlohmann::json mixJson = {{"id", mixerId}};
            auto endpoints = mixer->getEndpoints();
            mixJson["usercount"] = endpoints.size();
            mixJson["users"] = nlohmann::json::array();
            auto& endpointArray = mixJson["users"];
            for (auto& uid : endpoints)
            {
                endpointArray.push_back(uid);
            }
            responseBodyJson.push_back(mixJson);
        };
    }

    httpd::Response response(httpd::StatusCode::OK, responseBodyJson.dump(4));
    response.headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}
} // namespace bridge
