#include "ApiActions.h"
#include "ApiHelpers.h"
#include "api/BarbellDescription.h"
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
#include "config/Config.h"
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
    api::BarbellDescription channelsDescription;
    channelsDescription.barbellId = barbellId;

    // Describe barbell transport
    api::Transport responseBundleTransport;

    TransportDescription transportDescription;
    if (!mixer.getBarbellTransportDescription(barbellId, transportDescription))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
            "Fail to get barbell transport description");
    }

    const auto& transportDescriptionIce = transportDescription.ice.get();
    api::Ice responseIce;
    responseIce.ufrag = transportDescriptionIce.iceCredentials.first;
    responseIce.pwd = transportDescriptionIce.iceCredentials.second;
    for (const auto& iceCandidate : transportDescriptionIce.iceCandidates)
    {
        if (iceCandidate.type != ice::IceCandidate::Type::PRFLX)
        {
            responseIce.candidates.emplace_back(iceCandidateToApi(iceCandidate));
        }
    }
    responseBundleTransport.ice.set(responseIce);

    api::Dtls responseDtls;
    responseDtls.type = "sha-256";
    responseDtls.hash = context->sslDtls.getLocalFingerprint();

    responseDtls.setup = dtlsClient ? "active" : "actpass";

    responseBundleTransport.dtls.set(responseDtls);

    responseBundleTransport.rtcpMux = true;
    channelsDescription.transport = responseBundleTransport;

    // Describe audio, video and data streams
    api::Audio responseAudio;
    {
        AudioStreamDescription streamDescription;
        mixer.getAudioStreamDescription(streamDescription);
        responseAudio.ssrcs = streamDescription.ssrcs;

        addDefaultAudioProperties(responseAudio, false);
        channelsDescription.audio = responseAudio;
    }

    api::Video responseVideo;
    {
        std::vector<BarbellVideoStreamDescription> streamDescriptions;
        mixer.getBarbellVideoStreamDescription(streamDescriptions);
        for (auto& group : streamDescriptions)
        {
            responseVideo.streams.emplace_back(api::VideoStream());
            auto& stream = responseVideo.streams.back();
            for (auto level : group.ssrcLevels)
            {
                stream.sources.push_back({level.main, level.feedback});
            }

            stream.content = (group.slides ? api::VideoStream::slidesContent : api::VideoStream::videoContent);
        }

        if (context->config.codec.videoCodec.get() == "H264")
        {
            addH264VideoProperties(responseVideo,
                context->config.codec.h264ProfileLevelId.get(),
                context->config.codec.h264PacketizationMode.get());
        }
        else
        {
            addVp8VideoProperties(responseVideo);
        }
        addDefaultVideoProperties(responseVideo);
        channelsDescription.video = responseVideo;
    }

    api::Data responseData;
    responseData.port = 5000;
    channelsDescription.data = responseData;

    const auto responseBody = api::Generator::generateAllocateBarbellResponse(channelsDescription);

    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response.headers["Content-type"] = "text/json";

    logger::debug("barbell response %s", "BarbellActions", response.body.c_str());
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
    const api::BarbellDescription& barbellDescription)
{
    Mixer* mixer;
    auto scopedMixerLock = getConferenceMixer(context, conferenceId, mixer);

    if (!barbellDescription.transport.ice.isSet() || !barbellDescription.transport.dtls.isSet())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            utils::format("Missing barbell ice/dtls transport description %s - %s",
                conferenceId.c_str(),
                barbellId.c_str()));
    }

    auto& transportDescription = barbellDescription.transport;
    auto& dtls = transportDescription.dtls.get();

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
    auto& videoDescription = barbellDescription.video;
    for (auto& stream : videoDescription.streams)
    {
        BarbellVideoStreamDescription barbellGroup;
        barbellGroup.ssrcLevels = stream.sources;
        barbellGroup.slides = (stream.content.compare(api::VideoStream::slidesContent) == 0);
        videoDescriptions.push_back(barbellGroup);
    }

    bridge::RtpMap audioRtpMap;
    for (auto& payloadDescription : barbellDescription.audio.payloadTypes)
    {
        auto rtpMap = makeRtpMap(barbellDescription.audio, payloadDescription);
        if (rtpMap.format == bridge::RtpMap::Format::TELEPHONE_EVENT)
        {
            logger::warn("telephone-event configured but not supported on this version. It will be ignored",
                "BarbellActions");
        }
        else
        {
            if (!audioRtpMap.isEmpty())
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Multiple audio codecs");
            }

            audioRtpMap = std::move(rtpMap);
        }
    }

    bridge::RtpMap videoRtpMap;
    bridge::RtpMap videoFeedbackRtpMap;
    for (auto& payloadDescription : barbellDescription.video.payloadTypes)
    {
        if (payloadDescription.name.compare("rtx") == 0)
        {
            videoFeedbackRtpMap = makeRtpMap(barbellDescription.video, payloadDescription);
        }
        else
        {
            videoRtpMap = makeRtpMap(barbellDescription.video, payloadDescription);
        }
    }

    mixer->configureBarbellSsrcs(barbellId,
        videoDescriptions,
        barbellDescription.audio.ssrcs,
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
    auto response = httpd::Response(httpd::StatusCode::NO_CONTENT);
    return response;
}

httpd::Response processBarbellPutAction(ActionContext* context,
    RequestLogger& requestLogger,
    const httpd::Request& request,
    const std::string& conferenceId,
    const std::string& barbellId)
{
    const auto requestBodyJson = nlohmann::json::parse(request.body.getSpan());
    const auto actionJsonItr = requestBodyJson.find("action");
    if (actionJsonItr == requestBodyJson.end())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Missing required json property: action");
    }
    const auto& action = actionJsonItr->get<std::string>();

    if (action.compare("configure") == 0)
    {
        const auto barbellDescription = api::Parser::parsePatchBarbell(requestBodyJson, barbellId);
        return configureBarbell(context, requestLogger, conferenceId, barbellId, barbellDescription);
    }

    throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
        utils::format("Unknown action '%s' on endpoint %s ", action.c_str(), request.getMethodString()));
}

httpd::Response processBarbellAction(ActionContext* context,
    RequestLogger& requestLogger,
    const httpd::Request& request,
    const std::string& conferenceId,
    const std::string& barbellId)
{
    if (request.method == httpd::Method::DELETE)
    {
        return deleteBarbell(context, requestLogger, conferenceId, barbellId);
    }
    else if (request.method == httpd::Method::POST)
    {
        const auto requestBodyJson = nlohmann::json::parse(request.body.getSpan());
        const auto actionJsonItr = requestBodyJson.find("action");
        if (actionJsonItr == requestBodyJson.end())
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
                "Missing required json property: action");
        }
        const auto& action = actionJsonItr->get<std::string>();

        if (action.compare("allocate") == 0)
        {
            bool iceControlling = requestBodyJson["bundle-transport"]["ice-controlling"];
            return allocateBarbell(context, requestLogger, iceControlling, conferenceId, barbellId);
        }
        else
        {
            return processBarbellPutAction(context, requestLogger, request, conferenceId, barbellId);
        }

        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            utils::format("Unknown action '%s' on endpoint %s ", action.c_str(), request.getMethodString()));
    }
    else if (request.method == httpd::Method::PUT)
    {
        return processBarbellPutAction(context, requestLogger, request, conferenceId, barbellId);
    }

    throw httpd::RequestErrorException(httpd::StatusCode::METHOD_NOT_ALLOWED,
        utils::format("HTTP method '%s' not allowed on this endpoint", request.getMethodString()));
}
} // namespace bridge
