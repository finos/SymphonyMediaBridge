#pragma once
#include "ActionContext.h"
#include "httpd/Request.h"
#include "httpd/Response.h"
#include "utils/StringTokenizer.h"
#include <mutex>
#include <string>

namespace bridge
{
class Mixer;
class RequestLogger;
std::unique_lock<std::mutex> getConferenceMixer(ActionContext*, const std::string&, Mixer*&);

httpd::Response allocateConference(ActionContext*,
    RequestLogger&,
    const httpd::Request&,
    const ::utils::StringTokenizer::Token&);
httpd::Response getConferences(ActionContext* context,
    RequestLogger&,
    const httpd::Request&,
    const utils::StringTokenizer::Token&);
httpd::Response processConferenceAction(ActionContext*,
    RequestLogger&,
    const httpd::Request&,
    const utils::StringTokenizer::Token&);
httpd::Response getConferenceInfo(ActionContext*,
    RequestLogger&,
    const httpd::Request&,
    const ::utils::StringTokenizer::Token&);
httpd::Response getEndpointInfo(ActionContext*,
    RequestLogger&,
    const httpd::Request&,
    const ::utils::StringTokenizer::Token&);
httpd::Response handleStats(ActionContext*,
    RequestLogger&,
    const httpd::Request&,
    const ::utils::StringTokenizer::Token&);
httpd::Response handleAbout(ActionContext*,
    RequestLogger&,
    const httpd::Request&,
    const ::utils::StringTokenizer::Token&);
httpd::Response processBarbellAction(ActionContext*,
    RequestLogger&,
    const httpd::Request&,
    const ::utils::StringTokenizer::Token&);
} // namespace bridge
