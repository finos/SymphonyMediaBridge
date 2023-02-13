#include "bridge/RequestLogger.h"
#include "bridge/endpointActions/ApiActions.h"
#include "config/Config.h"
#include "git_version.h"
#include "utils/StringTokenizer.h"

namespace bridge
{
httpd::Response handleAbout(ActionContext* context,
    RequestLogger&,
    const httpd::Request&,
    const utils::StringTokenizer::Token& token)
{
    const auto nextToken = ::utils::StringTokenizer::tokenize(token.next, token.remainingLength, '/');
    if (utils::StringTokenizer::isEqual(nextToken, "version"))
    {
        std::string versionString = "{\"revision\":\"" + std::string(kGitHash) + "\"}";
        httpd::Response response(httpd::StatusCode::OK, versionString);
        response._headers["Content-type"] = "text/json";
        return response;
    }
    else if (utils::StringTokenizer::isEqual(nextToken, "health"))
    {
        httpd::Response response(httpd::StatusCode::OK, "{}");
        response._headers["Content-type"] = "text/json";
        return response;
    }
    else if (utils::StringTokenizer::isEqual(nextToken, "capabilities"))
    {
        std::string body;
        body.reserve(128);

        body.append("[").append("\"smb_api.1\",\"ssrc_rewriting.1\"");
        if (context->config.capabilities.barbelling)
        {
            body.append(",\"barbelling.1\"");
        }

        body.append("]");

        httpd::Response response(httpd::StatusCode::OK, body);
        response._headers["Content-type"] = "text/json";
        return response;
    }
    else
    {
        return httpd::Response(httpd::StatusCode::NOT_FOUND);
    }
}
} // namespace bridge
