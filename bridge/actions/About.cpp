#include "ApiActions.h"
#include "bridge/RequestLogger.h"
#include "utils/StringTokenizer.h"

namespace bridge
{
httpd::Response handleAbout(ActionContext* context,
    RequestLogger&,
    const httpd::Request& request,
    const utils::StringTokenizer::Token& token)
{
    const auto nextToken = ::utils::StringTokenizer::tokenize(token.next, token.remainingLength, '/');
    if (utils::StringTokenizer::isEqual(nextToken, "version"))
    {
        httpd::Response response(httpd::StatusCode::OK, "{}");
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
        httpd::Response response(httpd::StatusCode::OK, "[\"smb_api.1\", \"ssrc_rewriting.1\"]");
        response._headers["Content-type"] = "text/json";
        return response;
    }
    else
    {
        return httpd::Response(httpd::StatusCode::NOT_FOUND);
    }
}
} // namespace bridge