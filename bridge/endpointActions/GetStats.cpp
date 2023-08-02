#include "ApiActions.h"
#include "bridge/MixerManager.h"
#include "bridge/RequestLogger.h"
#include "bridge/Mixer.h"

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

httpd::Response handleBarbellStats(ActionContext* context, RequestLogger&, const httpd::Request& request)
{
    auto barbellStats = context->mixerManager.getBarbellStats();
    const auto statsDescription = barbellStats.describe();
    httpd::Response response(httpd::StatusCode::OK, statsDescription);
    response._headers["Content-type"] = "text/json";
    return response;
}

httpd::Response handleBarbellStats(ActionContext* context, RequestLogger&, const httpd::Request& request, const std::string& confId)
{
    Mixer* mixer;
    auto scopedMixerLock = getConferenceMixer(context, confId, mixer);

    Stats::AggregatedBarbellStats barbellStats;
    barbellStats._stats.emplace(confId, mixer->gatherBarbellStats(utils::Time::getAbsoluteTime()));

    const auto statsDescription = barbellStats.describe();
    httpd::Response response(httpd::StatusCode::OK, statsDescription);
    response._headers["Content-type"] = "text/json";
    return response;
}

} // namespace bridge
