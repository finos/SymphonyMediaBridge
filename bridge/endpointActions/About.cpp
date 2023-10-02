#include "bridge/RequestLogger.h"
#include "bridge/endpointActions/ApiActions.h"
#include "config/Config.h"
#include "git_version.h"
#include "openssl/opensslv.h"
#include "transport/dtls/SrtpClient.h"
#include "utils/Format.h"
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
        std::string versionString = utils::format(R"({"revision":"%s", "srtp":"%s", "openssl":"%s"})",
            kGitHash,
            srtp_get_version_string(),
            OPENSSL_VERSION_TEXT);
        httpd::Response response(httpd::StatusCode::OK, versionString);
        response.headers["Content-type"] = "text/json";
        return response;
    }
    else if (utils::StringTokenizer::isEqual(nextToken, "health"))
    {
        httpd::Response response(httpd::StatusCode::OK, "{}");
        response.headers["Content-type"] = "text/json";
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
        response.headers["Content-type"] = "text/json";
        return response;
    }
    else
    {
        return httpd::Response(httpd::StatusCode::NOT_FOUND);
    }
}
} // namespace bridge
