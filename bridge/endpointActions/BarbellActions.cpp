#include "ApiActions.h"
#include "ApiHelpers.h"
#include "api/EndpointDescription.h"
#include "api/Generator.h"
#include "api/Parser.h"
#include "bridge/AudioStreamDescription.h"
#include "bridge/BarbellVideoStreamDescription.h"
#include "bridge/Mixer.h"
#include "bridge/MixerManager.h"
#include "bridge/RequestLogger.h"
#include "bridge/TransportDescription.h"
#include "bridge/VideoStreamDescription.h"
#include "httpd/RequestErrorException.h"
#include "nlohmann/json.hpp"
#include "transport/dtls/SslDtls.h"
#include "utils/Format.h"

namespace bridge
{

httpd::Response generateBarbellResponse(ActionContext* context,
    RequestLogger& requestLogger,
    Mixer& mixer,
    const std::string& conferenceId,
    const std::string& barbellId,
    const bool dtlsClient)
{
    api::EndpointDescription channelsDescription;
    channelsDescription._endpointId = barbellId;

    // Describe barbell transport
    api::EndpointDescription::Transport responseBundleTransport;

    TransportDescription transportDescription;
    if (!mixer.getBarbellTransportDescription(barbellId, transportDescription))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
            "Fail to get barbell transport description");
    }

    const auto& transportDescriptionIce = transportDescription._ice.get();
    api::EndpointDescription::Ice responseIce;
    responseIce._ufrag = transportDescriptionIce._iceCredentials.first;
    responseIce._pwd = transportDescriptionIce._iceCredentials.second;
    for (const auto& iceCandidate : transportDescriptionIce._iceCandidates)
    {
        if (iceCandidate.type != ice::IceCandidate::Type::PRFLX)
        {
            responseIce._candidates.emplace_back(iceCandidateToApi(iceCandidate));
        }
    }
    responseBundleTransport._ice.set(responseIce);

    api::EndpointDescription::Dtls responseDtls;
    responseDtls.type = "sha-256";
    responseDtls.hash = context->sslDtls.getLocalFingerprint();

    responseDtls.setup = dtlsClient ? "active" : "actpass";

    responseBundleTransport._dtls.set(responseDtls);

    responseBundleTransport._rtcpMux = true;
    channelsDescription._bundleTransport.set(responseBundleTransport);

    // Describe audio, video and data streams
    api::EndpointDescription::Audio responseAudio;
    {
        AudioStreamDescription streamDescription;
        mixer.getAudioStreamDescription(streamDescription);
        responseAudio._ssrcs = streamDescription.ssrcs;

        addDefaultAudioProperties(responseAudio);
        channelsDescription._audio.set(responseAudio);
    }

    api::EndpointDescription::Video responseVideo;
    {
        std::vector<BarbellVideoStreamDescription> streamDescriptions;
        mixer.getBarbellVideoStreamDescription(streamDescriptions);
        for (auto& group : streamDescriptions)
        {
            responseVideo.streams.emplace_back(api::EndpointDescription::VideoStream());
            auto& stream = responseVideo.streams.back();
            for (auto level : group.ssrcLevels)
            {
                stream.sources.push_back({level.main, level.feedback});
            }

            stream.content =
                (group.slides ? api::EndpointDescription::slidesContent : api::EndpointDescription::videoContent);
        }

        addDefaultVideoProperties(responseVideo);
        channelsDescription._video.set(responseVideo);
    }

    api::EndpointDescription::Data responseData;
    responseData._port = 5000;
    channelsDescription._data.set(responseData);

    const auto responseBody = api::Generator::generateAllocateEndpointResponse(channelsDescription);

    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response._headers["Content-type"] = "text/json";

    logger::debug("barbell response %s", "", response._body.c_str());
    return response;
}

httpd::Response allocateBarbell(ActionContext* context,
    RequestLogger& requestLogger,
    bool iceControlling,
    const std::string& conferenceId,
    const std::string& barbellId)
{
    Mixer* mixer;
    auto scopedMixerLock = getConferenceMixer(context, conferenceId, mixer);

    const auto iceRole = iceControlling ? ice::IceRole::CONTROLLING : ice::IceRole::CONTROLLED;
    if (!mixer->addBarbell(barbellId, iceRole))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
            utils::format("Failed to create barbell leg for conference'%s'", conferenceId.c_str()));
    }

    return generateBarbellResponse(context,
        requestLogger,
        *mixer,
        conferenceId,
        barbellId,
        iceRole == ice::IceRole::CONTROLLED);
}

httpd::Response configureBarbell(ActionContext* context,
    RequestLogger& requestLogger,
    const std::string& conferenceId,
    const std::string& barbellId,
    const api::EndpointDescription& barbellDescription)
{
    Mixer* mixer;
    auto scopedMixerLock = getConferenceMixer(context, conferenceId, mixer);

    if (!barbellDescription._bundleTransport.isSet() || !barbellDescription._bundleTransport.get()._ice.isSet() ||
        !barbellDescription._bundleTransport.get()._dtls.isSet() || !barbellDescription._audio.isSet() ||
        !barbellDescription._video.isSet())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            utils::format("Missing barbell ice/dtls transport description %s - %s",
                conferenceId.c_str(),
                barbellId.c_str()));
    }

    if (!barbellDescription._audio.isSet())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            utils::format("Missing barbell audio description %s - %s", conferenceId.c_str(), barbellId.c_str()));
    }

    if (!barbellDescription._video.isSet() || barbellDescription._video.get().payloadTypes.size() < 2)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            utils::format("Missing barbell video description %s - %s", conferenceId.c_str(), barbellId.c_str()));
    }

    auto& transportDescription = barbellDescription._bundleTransport.get();
    auto& dtls = transportDescription._dtls.get();

    const bool isRemoteSideDtlsClient = dtls.isClient();
    const auto candidatesAndCredentials = getIceCandidatesAndCredentials(transportDescription);

    if (!mixer->configureBarbellTransport(barbellId,
            candidatesAndCredentials.second,
            candidatesAndCredentials.first,
            dtls.type,
            dtls.hash,
            !isRemoteSideDtlsClient))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
            utils::format("Failed to configure barbell transport %s - %s", conferenceId.c_str(), barbellId.c_str()));
    }

    std::vector<BarbellVideoStreamDescription> videoDescriptions;
    auto& videoDescription = barbellDescription._video.get();
    for (auto& stream : videoDescription.streams)
    {
        BarbellVideoStreamDescription barbellGroup;
        barbellGroup.ssrcLevels = stream.sources;
        barbellGroup.slides = (stream.content.compare(api::EndpointDescription::slidesContent) == 0);
        videoDescriptions.push_back(barbellGroup);
    }

    const auto audioRtpMap = makeRtpMap(barbellDescription._audio.get());
    bridge::RtpMap videoRtpMap;
    bridge::RtpMap videoFeedbackRtpMap;
    for (auto& payloadDescription : barbellDescription._video.get().payloadTypes)
    {
        if (payloadDescription._name.compare("rtx") == 0)
        {
            videoFeedbackRtpMap = makeRtpMap(barbellDescription._video.get(), payloadDescription);
        }
        else
        {
            videoRtpMap = makeRtpMap(barbellDescription._video.get(), payloadDescription);
        }
    }

    mixer->configureBarbellSsrcs(barbellId,
        videoDescriptions,
        barbellDescription._audio.get()._ssrcs,
        audioRtpMap,
        videoRtpMap,
        videoFeedbackRtpMap);

    mixer->addBarbellToEngine(barbellId);
    mixer->startBarbellTransport(barbellId);

    return generateBarbellResponse(context, requestLogger, *mixer, conferenceId, barbellId, !isRemoteSideDtlsClient);
}

httpd::Response deleteBarbell(ActionContext* context,
    RequestLogger& requestLogger,
    const std::string& conferenceId,
    const std::string& barbellId)
{
    Mixer* mixer;
    auto scopedMixerLock = getConferenceMixer(context, conferenceId, mixer);
    mixer->removeBarbell(barbellId);
    auto response = httpd::Response(httpd::StatusCode::OK, "");
    return response;
}

httpd::Response processBarbellAction(ActionContext* context,
    RequestLogger& requestLogger,
    const httpd::Request& request,
    const ::utils::StringTokenizer::Token& incomingToken)
{
    auto token = utils::StringTokenizer::tokenize(incomingToken, '/');
    const auto conferenceId = token.str();
    if (!token.next)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::NOT_FOUND, "Endpoint not found");
    }
    token = utils::StringTokenizer::tokenize(token, '/');
    const auto barbellId = token.str();

    if (request._method == httpd::Method::DELETE)
    {
        return deleteBarbell(context, requestLogger, conferenceId, barbellId);
    }

    const auto requestBody = request._body.build();
    const auto requestBodyJson = nlohmann::json::parse(requestBody);
    const auto actionJsonItr = requestBodyJson.find("action");
    if (actionJsonItr == requestBodyJson.end())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Missing required json property: action");
    }
    const auto& action = actionJsonItr->get<std::string>();

    if (action.compare("allocate") == 0)
    {
        bool iceControlling = requestBodyJson["bundle-transport"]["ice-controlling"];
        return allocateBarbell(context, requestLogger, iceControlling, conferenceId, barbellId);
    }
    else if (action.compare("configure") == 0)
    {
        const auto endpointDescription = api::Parser::parsePatchEndpoint(requestBodyJson, barbellId);
        return configureBarbell(context, requestLogger, conferenceId, barbellId, endpointDescription);
    }

    throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
        utils::format("Unknown action '%s' on endpoint %s ", action.c_str(), request._methodString.c_str()));
}
} // namespace bridge
