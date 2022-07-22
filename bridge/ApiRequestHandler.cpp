#include "bridge/ApiRequestHandler.h"
#include "actions/ApiHelpers.h"
#include "bridge/RequestLogger.h"
#include "httpd/RequestErrorException.h"
#include "nlohmann/json.hpp"
#include "utils/Format.h"

namespace
{

httpd::Response makeErrorResponse(httpd::StatusCode statusCode, const std::string& message)
{
    nlohmann::json errorBody;
    errorBody["status_code"] = static_cast<uint32_t>(statusCode);
    errorBody["message"] = message;
    auto response = httpd::Response(statusCode, errorBody.dump());
    response._headers["Content-type"] = "text/json";
    return response;
}

} // namespace

namespace bridge
{

ApiRequestHandler::ApiRequestHandler(bridge::MixerManager& mixerManager, transport::SslDtls& sslDtls)
    : ActionContext(mixerManager, sslDtls),
      _lastAutoRequestId(0)

#if ENABLE_LEGACY_API
      ,
      _legacyApiRequestHandler(std::make_unique<LegacyApiRequestHandler>(mixerManager, sslDtls))
#endif
{
    _actionMap.emplace(ApiActions::ABOUT, ::bridge::handleAbout);
    _actionMap.emplace(ApiActions::STATS, ::bridge::handleStats);
    _actionMap.emplace(ApiActions::GET_CONFERENCES, ::bridge::getConferences);
    _actionMap.emplace(ApiActions::ALLOCATE_CONFERENCE, ::bridge::allocateConference);
    _actionMap.emplace(ApiActions::GET_CONFERENCE_INFO, ::bridge::getConferenceInfo);
    _actionMap.emplace(ApiActions::PROCESS_CONFERENCE_ACTION, ::bridge::processConferenceAction);
    _actionMap.emplace(ApiActions::PROCESS_BARBELL_ACTION, ::bridge::processBarbellAction);
}

ApiRequestHandler::ApiActions ApiRequestHandler::getAction(const httpd::Request& request,
    utils::StringTokenizer::Token& outToken)
{
    outToken = utils::StringTokenizer::tokenize(request._url.c_str(), request._url.length(), '/');
    if (utils::StringTokenizer::isEqual(outToken, "about") && outToken.next && request._method == httpd::Method::GET)
        return ApiActions::ABOUT;

    if (utils::StringTokenizer::isEqual(outToken, "stats") && request._method == httpd::Method::GET)
        return ApiActions::STATS;

    if (utils::StringTokenizer::isEqual(outToken, "conferences"))
    {
        if (outToken.next)
        {
            if (request._method == httpd::Method::GET)
            {
                auto nextToken = utils::StringTokenizer::tokenize(outToken, '/');
                if (nextToken.next)
                    return ApiActions::GET_ENDPOINT_INFO;
                else
                    return ApiActions::GET_CONFERENCE_INFO;
            }
            if (request._method == httpd::Method::POST)
                return ApiActions::PROCESS_CONFERENCE_ACTION;
        }
        else
        {
            if (request._method == httpd::Method::GET)
                return ApiActions::GET_CONFERENCES;
            else if (request._method == httpd::Method::POST)
                return ApiActions::ALLOCATE_CONFERENCE;
        }
    }

    if (utils::StringTokenizer::isEqual(outToken, "barbell") && outToken.next &&
        (request._method == httpd::Method::POST || request._method == httpd::Method::DELETE))
        return ApiActions::PROCESS_BARBELL_ACTION;

    throw httpd::RequestErrorException(httpd::StatusCode::METHOD_NOT_ALLOWED,
        utils::format("HTTP method '%s' not allowed on this endpoint", request._methodString.c_str()));
}

httpd::Response ApiRequestHandler::onRequest(const httpd::Request& request)
{
    try
    {
        if (request._method == httpd::Method::OPTIONS)
        {
            return httpd::Response(httpd::StatusCode::NO_CONTENT);
        }

        auto token = utils::StringTokenizer::tokenize(request._url.c_str(), request._url.length(), '/');

#if ENABLE_LEGACY_API
        if (utils::StringTokenizer::isEqual(token, "colibri"))
        {
            return _legacyApiRequestHandler->onRequest(request);
        }
#endif

        RequestLogger requestLogger(request, _lastAutoRequestId);
        auto action = getAction(request, token);
        assert(action != ApiActions::LAST);
        assert(_actionMap.find(action) != _actionMap.end());
        try
        {
            return _actionMap[action](this, requestLogger, request, token);
        }
        catch (httpd::RequestErrorException e)
        {
            auto response = makeErrorResponse(e.getStatusCode(), e.getMessage());
            requestLogger.setResponse(response);
            requestLogger.setErrorMessage(e.getMessage());
            return response;
        }
        catch (nlohmann::detail::parse_error e)
        {
            logger::warn("Error parsing json", "RequestHandler");
            std::string errorMessage = "Invalid json format";
            auto response = makeErrorResponse(httpd::StatusCode::BAD_REQUEST, errorMessage);
            requestLogger.setResponse(response);
            requestLogger.setErrorMessage(errorMessage);
            return response;
        }
        catch (nlohmann::detail::exception e)
        {
            logger::warn("Error processing json", "RequestHandler");
            std::string message = "nlohmann:" + std::string(e.what());
            auto response = makeErrorResponse(httpd::StatusCode::BAD_REQUEST, message);
            requestLogger.setResponse(response);
            requestLogger.setErrorMessage(message);
            return response;
        }
        catch (std::exception e)
        {
            logger::error("Exception in createConference", "RequestHandler");
            auto response = makeErrorResponse(httpd::StatusCode::BAD_REQUEST, e.what());
            requestLogger.setResponse(response);
            requestLogger.setErrorMessage(e.what());
            return response;
        }

        const auto errorMessage = utils::format("URL is not point to a valid endpoint: '%s'", request._url.c_str());
        auto response = makeErrorResponse(httpd::StatusCode::NOT_FOUND, errorMessage);
        requestLogger.setResponse(response);
        requestLogger.setErrorMessage(errorMessage);
        return response;
    }
    catch (...)
    {
        logger::error("Uncaught exception in onRequest", "ApiRequestHandler");
        return httpd::Response(httpd::StatusCode::INTERNAL_SERVER_ERROR);
    }
}

} // namespace bridge
