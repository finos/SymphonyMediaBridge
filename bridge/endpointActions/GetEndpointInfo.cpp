#include "ApiActions.h"
#include "bridge/Mixer.h"

namespace bridge
{
httpd::Response getEndpointInfo(ActionContext* context,
    RequestLogger&,
    const httpd::Request&,
    const ::utils::StringTokenizer::Token& incomingToken)
{
    bridge::Mixer* mixer;
    auto token = utils::StringTokenizer::tokenize(incomingToken, '/');
    const auto conferenceId = token.str();
    auto scopedMixerLock = getConferenceMixer(context, conferenceId, mixer);
    return httpd::Response(httpd::StatusCode::OK);
}
} // namespace bridge
