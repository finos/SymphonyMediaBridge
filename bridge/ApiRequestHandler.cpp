#include "bridge/ApiRequestHandler.h"
#include "bridge/RequestLogger.h"
#include "endpointActions/ApiHelpers.h"
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

ApiRequestHandler::ApiRequestHandler(bridge::MixerManager& mixerManager,
    transport::SslDtls& sslDtls,
    transport::ProbeServer& probeServer,
    const config::Config& config)
    : ActionContext(mixerManager, sslDtls, probeServer, config),
      _lastAutoRequestId(0)

#if ENABLE_LEGACY_API
      ,
      _legacyApiRequestHandler(std::make_unique<LegacyApiRequestHandler>(mixerManager, sslDtls))
#endif
{
}

httpd::Response ApiRequestHandler::handleConferenceRequest(RequestLogger& requestLogger, const httpd::Request& request)
{
    if (request._method == httpd::Method::POST)
    {
        return allocateConference(this, requestLogger, request);
    }
    else if (request._method == httpd::Method::GET)
    {
        return getConferences(this, requestLogger);
    }

    throw httpd::RequestErrorException(httpd::StatusCode::METHOD_NOT_ALLOWED,
        utils::format("HTTP method '%s' not allowed on this endpoint", request._methodString.c_str()));
}

httpd::Response ApiRequestHandler::handleConferenceRequest(RequestLogger& requestLogger,
    const httpd::Request& request,
    const std::string& conferenceId)
{
    if (request._method == httpd::Method::GET)
    {
        return getConferenceInfo(this, requestLogger, request, conferenceId);
    }

    throw httpd::RequestErrorException(httpd::StatusCode::METHOD_NOT_ALLOWED,
        utils::format("HTTP method '%s' not allowed on this endpoint", request._methodString.c_str()));
}

httpd::Response ApiRequestHandler::handleEndpointRequest(RequestLogger& requestLogger,
    const httpd::Request& request,
    const std::string& conferenceId,
    const std::string& endpointId)
{
    if (request._method == httpd::Method::GET)
    {
        return getEndpointInfo(this, requestLogger, request, conferenceId, endpointId);
    }
    else if (request._method == httpd::Method::POST)
    {
        return processEndpointPostRequest(this, requestLogger, request, conferenceId, endpointId);
    }
    else if (request._method == httpd::Method::DELETE)
    {
        return expireEndpoint(this, requestLogger, conferenceId, endpointId);
    }

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

#if ENABLE_LEGACY_API
        auto token = utils::StringTokenizer::tokenize(request._url.c_str(), request._url.length(), '/');
        if (utils::StringTokenizer::isEqual(token, "colibri"))
        {
            return _legacyApiRequestHandler->onRequest(request);
        }
#endif

        RequestLogger requestLogger(request, _lastAutoRequestId);
        try
        {
            auto token = utils::StringTokenizer::tokenize(request._url.c_str(), request._url.length(), "/?");
            if (utils::StringTokenizer::isEqual(token, "about") && token.next && request._method == httpd::Method::GET)
            {
                return handleAbout(this, requestLogger, request, token);
            }
            else if (utils::StringTokenizer::isEqual(token, "stats") && request._method == httpd::Method::GET)
            {
                return handleStats(this, requestLogger, request);
            }
            else if (utils::StringTokenizer::isEqual(token, "conferences") && !token.next)
            {
                return handleConferenceRequest(requestLogger, request);
            }
            else if (utils::StringTokenizer::isEqual(token, "conferences") && token.next && token.delimiter == '?')
            {
                token = utils::StringTokenizer::tokenize(token, '?');
                const auto listType = token.str();
                if (!token.next && listType == "brief")
                {
                    return fetchBriefConferenceList(this, requestLogger);
                }
            }
            else if (utils::StringTokenizer::isEqual(token, "conferences") && token.next && token.delimiter == '/')
            {
                token = utils::StringTokenizer::tokenize(token, '/');
                const auto conferenceId = token.str();

                if (token.next)
                {
                    token = utils::StringTokenizer::tokenize(token, '/');
                    const auto endpointId = token.str();
                    return handleEndpointRequest(requestLogger, request, conferenceId, endpointId);
                }
                else
                {
                    return handleConferenceRequest(requestLogger, request, conferenceId);
                }
            }
            else if (utils::StringTokenizer::isEqual(token, "barbell") && token.next && token.delimiter == '/')
            {
                token = utils::StringTokenizer::tokenize(token, '/');
                const auto conferenceId = token.str();
                if (token.next)
                {
                    token = utils::StringTokenizer::tokenize(token, '/');
                    const auto barbellId = token.str();
                    return processBarbellAction(this, requestLogger, request, conferenceId, barbellId);
                }
            }
            else if (utils::StringTokenizer::isEqual(token, "ice-candidates"))
            {
                return getProbingInfo(this, requestLogger, request);
            }

            throw httpd::RequestErrorException(httpd::StatusCode::NOT_FOUND, utils::format("Resource not found"));
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

        const auto errorMessage = utils::format("URL not found: '%s'", request._url.c_str());
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
