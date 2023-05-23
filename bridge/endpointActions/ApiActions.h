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

httpd::Response allocateConference(ActionContext*, RequestLogger&, const httpd::Request&);

httpd::Response getConferences(ActionContext* context, RequestLogger&);
httpd::Response fetchBriefConferenceList(ActionContext* context, RequestLogger& requestLogger);

httpd::Response processEndpointPostRequest(ActionContext* context,
    RequestLogger& requestLogger,
    const httpd::Request& request,
    const std::string& conferenceId,
    const std::string& endpointId);

httpd::Response processEndpointPutRequest(ActionContext* context,
    RequestLogger& requestLogger,
    const httpd::Request& request,
    const std::string& conferenceId,
    const std::string& endpointId);

httpd::Response getConferenceInfo(ActionContext*,
    RequestLogger&,
    const httpd::Request&,
    const std::string& conferenceId);
httpd::Response getEndpointInfo(ActionContext*,
    RequestLogger&,
    const httpd::Request&,
    const std::string& conferenceId,
    const std::string& endpointId);
httpd::Response handleStats(ActionContext*, RequestLogger&, const httpd::Request&);
httpd::Response handleAbout(ActionContext*,
    RequestLogger&,
    const httpd::Request&,
    const ::utils::StringTokenizer::Token&);
httpd::Response processBarbellAction(ActionContext*,
    RequestLogger&,
    const httpd::Request&,
    const std::string& conferenceId,
    const std::string& barbellId);

httpd::Response processBarbellPutAction(ActionContext* context,
    RequestLogger& requestLogger,
    const httpd::Request& request,
    const std::string& conferenceId,
    const std::string& barbellId);

httpd::Response getProbingInfo(ActionContext*, RequestLogger&, const httpd::Request&);

httpd::Response expireEndpoint(ActionContext* context,
    RequestLogger& requestLogger,
    const std::string& conferenceId,
    const std::string& endpointId);
} // namespace bridge
