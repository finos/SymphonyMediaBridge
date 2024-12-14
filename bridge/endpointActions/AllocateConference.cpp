#include "ApiActions.h"
#include "api/Parser.h"
#include "bridge/Mixer.h"
#include "bridge/MixerManager.h"
#include "bridge/RequestLogger.h"
#include "httpd/RequestErrorException.h"
#include "nlohmann/json.hpp"

namespace bridge
{

httpd::Response allocateConference(ActionContext* context, RequestLogger& requestLogger, const httpd::Request& request)
{
    const auto requestBodyJson = nlohmann::json::parse(request.body.getSpan());
    if (!requestBodyJson.is_object())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            "Allocate conference endpoint expects a json object");
    }

    const auto allocateConference = api::Parser::parseAllocateConference(requestBodyJson);

    auto mixer = allocateConference.lastN.isSet()
        ? context->mixerManager.create(allocateConference.lastN.get(),
              allocateConference.useGlobalPort,
              allocateConference.useH264)
        : context->mixerManager.create(allocateConference.useGlobalPort, allocateConference.useH264);

    if (!mixer)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR, "Conference creation has failed");
    }

    logger::info("Allocate conference %s, mixer %s, last-n %d, global-port %c, h264 %c",
        "ApiRequestHandler",
        mixer->getId().c_str(),
        mixer->getLoggableId().c_str(),
        allocateConference.lastN.isSet() ? allocateConference.lastN.get() : -1,
        allocateConference.useGlobalPort ? 't' : 'f',
        allocateConference.useH264 ? 't' : 'f');

    nlohmann::json responseJson;
    responseJson["id"] = mixer->getId();

    httpd::Response response(httpd::StatusCode::OK, responseJson.dump(4));
    response.headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}
} // namespace bridge
