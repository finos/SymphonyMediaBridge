#include "ApiActions.h"
#include "bridge/MixerManager.h"
#include "bridge/RequestLogger.h"

namespace bridge
{
httpd::Response handleStats(ActionContext* context, RequestLogger&, const httpd::Request& request)
{
    auto stats = context->mixerManager.getStats();
    const auto statsDescription = stats.describe();
    httpd::Response response(httpd::StatusCode::OK, statsDescription);
    response._headers["Content-type"] = "text/json";
    return response;
}
} // namespace bridge
