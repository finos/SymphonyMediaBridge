#include "ApiActions.h"
#include "ApiHelpers.h"
#include "api/ConferenceEndpoint.h"
#include "bridge/Mixer.h"
#include "bridge/MixerManager.h"
#include "bridge/RequestLogger.h"
#include "httpd/RequestErrorException.h"
#include "nlohmann/json.hpp"
#include "transport/ProbeServer.h"
#include "utils/Format.h"

namespace bridge
{
httpd::Response getProbingInfo(ActionContext* context, RequestLogger& requestLogger, const httpd::Request& request)
{
    if (request._method != httpd::Method::GET)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::METHOD_NOT_ALLOWED,
            utils::format("HTTP method '%s' not allowed on this endpoint", request._methodString.c_str()));
    }

    nlohmann::json responseBodyJson;
    nlohmann::json iceJson;

    auto& probeServer = context->probeServer;

    const auto credentials = probeServer.getCredentials();

    iceJson["ufrag"] = credentials.first;
    iceJson["pwd"] = credentials.second;

    nlohmann::json candidatesJson = nlohmann::json::array();

    for (const auto& candidate : probeServer.getCandidates())
    {
        api::EndpointDescription::Candidate description = iceCandidateToApi(candidate);

        nlohmann::json candidateJson;
        candidateJson["generation"] = description._generation;
        candidateJson["component"] = description._component;
        candidateJson["protocol"] = description._protocol;
        candidateJson["port"] = description._port;
        candidateJson["ip"] = description._ip;
        candidateJson["foundation"] = description._foundation;
        candidateJson["priority"] = description._priority;
        candidateJson["type"] = description._type;
        candidateJson["network"] = description._network;
        if (description._relAddr.isSet())
        {
            candidateJson["rel-addr"] = description._relAddr.get();
        }
        if (description._relPort.isSet())
        {
            candidateJson["rel-port"] = description._relPort.get();
        }

        candidatesJson.push_back(candidateJson);
    }

    iceJson["candidates"] = candidatesJson;
    responseBodyJson["ice"] = iceJson;

    httpd::Response response(httpd::StatusCode::OK, responseBodyJson.dump(4));
    response._headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}
} // namespace bridge
