#include "ApiActions.h"
#include "api/Generator.h"
#include "api/Parser.h"
#include "bridge/AudioStreamDescription.h"
#include "bridge/DataStreamDescription.h"
#include "bridge/Mixer.h"
#include "bridge/MixerManager.h"
#include "bridge/RequestLogger.h"
#include "bridge/TransportDescription.h"
#include "bridge/VideoStreamDescription.h"
#include "bridge/endpointActions/ApiHelpers.h"
#include "config/Config.h"
#include "httpd/RequestErrorException.h"
#include "nlohmann/json.hpp"
#include "transport/dtls/SslDtls.h"
#include "utils/CheckedCast.h"
#include "utils/Format.h"

namespace bridge
{

const uint32_t gatheringCompleteMaxWaitMs = 5000;
const uint32_t gatheringCompleteWaitMs = 100;
const uint32_t GUUID_LENGTH = 36;

namespace
{

httpd::Response generateAllocateEndpointResponse(ActionContext* context,
    RequestLogger& requestLogger,
    const api::AllocateEndpoint& allocateChannel,
    const Mixer& mixer,
    const std::string& conferenceId,
    const std::string& endpointId)
{
    api::EndpointDescription channelsDescription;
    channelsDescription.endpointId = endpointId;

    // Describe bundle transport
    if (allocateChannel.bundleTransport.isSet())
    {
        const auto& bundleTransport = allocateChannel.bundleTransport.get();
        api::Transport responseBundleTransport;

        TransportDescription transportDescription;
        if (!mixer.getTransportBundleDescription(endpointId, transportDescription))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to get bundled description");
        }

        if (!bundleTransport.ice)
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
                utils::format("Bundling without ice is not supported, conference %s", conferenceId.c_str()));
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

        if (bundleTransport.dtls)
        {
            api::Dtls responseDtls;
            responseDtls.type = "sha-256";
            responseDtls.hash = context->sslDtls.getLocalFingerprint();
            responseDtls.setup = "actpass";
            responseBundleTransport.dtls.set(responseDtls);
        }
        if (bundleTransport.sdes)
        {
            responseBundleTransport.sdesKeys = transportDescription.sdesKeys;
        }

        responseBundleTransport.rtcpMux = true;
        channelsDescription.bundleTransport.set(responseBundleTransport);
    }

    // Describe audio, video and data streams
    if (allocateChannel.audio.isSet())
    {
        const auto& allocAudio = allocateChannel.audio.get();
        api::Audio responseAudio;

        AudioStreamDescription streamDescription;
        TransportDescription transportDescription;

        if (!mixer.getAudioStreamDescription(endpointId, streamDescription) ||
            !mixer.getAudioStreamTransportDescription(endpointId, transportDescription))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to get audio description");
        }

        responseAudio.ssrcs = streamDescription.ssrcs;

        if (allocAudio.transport.isSet())
        {
            api::Transport responseTransport;

            if (allocAudio.transport.get().ice)
            {
                api::Ice responseIce;
                const auto& transportDescriptionIce = transportDescription.ice.get();
                responseIce.ufrag = transportDescriptionIce.iceCredentials.first;
                responseIce.pwd = transportDescriptionIce.iceCredentials.second;
                for (const auto& iceCandidate : transportDescriptionIce.iceCandidates)
                {
                    if (iceCandidate.type != ice::IceCandidate::Type::PRFLX)
                    {
                        responseIce.candidates.emplace_back(iceCandidateToApi(iceCandidate));
                    }
                }
                responseTransport.ice.set(responseIce);
                responseTransport.rtcpMux = true;
            }
            else
            {
                if (transportDescription.localPeer.isSet() && !transportDescription.localPeer.get().empty())
                {
                    api::Connection responseConnection;
                    responseConnection.ip = transportDescription.localPeer.get().ipToString();
                    responseConnection.port = transportDescription.localPeer.get().getPort();
                    responseTransport.connection.set(responseConnection);
                }

                responseTransport.rtcpMux = false;
            }

            if (allocAudio.transport.get().dtls)
            {
                api::Dtls responseDtls;
                responseDtls.setup = "active";
                responseDtls.type = "sha-256";
                responseDtls.hash = context->sslDtls.getLocalFingerprint();
                responseTransport.dtls.set(responseDtls);
            }
            if (allocAudio.transport.get().sdes)
            {
                responseTransport.sdesKeys = transportDescription.sdesKeys;
            }

            responseAudio.transport.set(responseTransport);
        }

        addDefaultAudioProperties(responseAudio, true);
        channelsDescription.audio.set(responseAudio);
    }

    if (allocateChannel.video.isSet() && mixer.hasVideoEnabled())
    {
        const auto& allocVideo = allocateChannel.video.get();
        api::Video responseVideo;

        VideoStreamDescription streamDescription;
        TransportDescription transportDescription;

        if (!mixer.getVideoStreamDescription(endpointId, streamDescription) ||
            !mixer.getVideoStreamTransportDescription(endpointId, transportDescription))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to get video description");
        }

        if (streamDescription.localSsrc != 0)
        {
            api::VideoStream videoStream;
            videoStream.sources.push_back({streamDescription.localSsrc, 0});
            videoStream.content = "local";
            responseVideo.streams.push_back(videoStream);
        }

        size_t index = 0;
        for (auto& level : streamDescription.sources)
        {
            api::VideoStream videoStream;
            videoStream.sources.push_back({level.main, level.feedback});
            if (index++ == 0)
            {
                videoStream.content = "slides";
            }
            else
            {
                videoStream.content = "video";
            }
            responseVideo.streams.push_back(videoStream);
        }

        if (allocVideo.transport.isSet())
        {
            const auto& transport = allocVideo.transport.get();
            api::Transport responseTransport;

            if (transport.ice)
            {
                api::Ice responseIce;
                const auto& transportDescriptionIce = transportDescription.ice.get();
                responseIce.ufrag = transportDescriptionIce.iceCredentials.first;
                responseIce.pwd = transportDescriptionIce.iceCredentials.second;
                for (const auto& iceCandidate : transportDescriptionIce.iceCandidates)
                {
                    if (iceCandidate.type != ice::IceCandidate::Type::PRFLX)
                    {
                        responseIce.candidates.emplace_back(iceCandidateToApi(iceCandidate));
                    }
                }
                responseTransport.ice.set(responseIce);
                responseTransport.rtcpMux = true;
            }
            else
            {
                if (transportDescription.localPeer.isSet() && !transportDescription.localPeer.get().empty())
                {
                    api::Connection responseConnection;
                    responseConnection.ip = transportDescription.localPeer.get().ipToString();
                    responseConnection.port = transportDescription.localPeer.get().getPort();
                    responseTransport.connection.set(responseConnection);
                }

                responseTransport.rtcpMux = false;
            }

            if (allocVideo.transport.get().dtls)
            {
                api::Dtls responseDtls;
                responseDtls.setup = "active";
                responseDtls.type = "sha-256";
                responseDtls.hash = context->sslDtls.getLocalFingerprint();
                responseTransport.dtls.set(responseDtls);
            }
            if (allocVideo.transport.get().sdes)
            {
                responseTransport.sdesKeys = transportDescription.sdesKeys;
            }

            responseVideo.transport.set(responseTransport);
        }

        if (context->config.codec.videoCodec.get() == "H264" || mixer.isH264Enabled())
        {
            addH264VideoProperties(responseVideo,
                context->config.codec.h264ProfileLevelId.get(),
                context->config.codec.h264PacketizationMode.get());
        }
        else if (context->config.codec.videoCodec.get() == "VP8")
        {
            addVp8VideoProperties(responseVideo);
        }

        addDefaultVideoProperties(responseVideo);
        channelsDescription.video.set(responseVideo);
    }

    if (allocateChannel.data.isSet())
    {
        api::Data responseData;

        DataStreamDescription streamDescription;
        if (!mixer.getDataStreamDescription(endpointId, streamDescription))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to get data stream description");
        }

        responseData.port = streamDescription.sctpPort.isSet() ? streamDescription.sctpPort.get() : 5000;
        channelsDescription.data.set(responseData);
    }

    const auto responseBody = api::Generator::generateAllocateEndpointResponse(channelsDescription);
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response.headers["Content-type"] = "text/json";
    logger::debug("POST response %s", "RequestHandler", response.body.c_str());
    requestLogger.setResponse(response);
    return response;
}

httpd::Response allocateEndpoint(ActionContext* context,
    RequestLogger& requestLogger,
    const api::AllocateEndpoint& allocateChannel,
    const std::string& conferenceId,
    const std::string& endpointId)
{
    Mixer* mixer;
    if (endpointId.size() > GUUID_LENGTH)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            "Endpoint id must be less in length than a GUUID excluding curly braces");
    }

    auto scopedMixerLock = getConferenceMixer(context, conferenceId, mixer);

    utils::Optional<std::string> audioChannelId;
    utils::Optional<std::string> videoChannelId;
    utils::Optional<std::string> dataChannelId;

    if (allocateChannel.bundleTransport.isSet())
    {
        const auto& bundleTransport = allocateChannel.bundleTransport.get();
        if (!bundleTransport.ice)
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Bundle transports requires ICE");
        }

        const auto iceRole = bundleTransport.iceControlling.isSet() && !bundleTransport.iceControlling.get()
            ? ice::IceRole::CONTROLLED
            : ice::IceRole::CONTROLLING;

        const bool hasVideoEnabled = allocateChannel.video.isSet();

        mixer->addBundleTransportIfNeeded(endpointId, iceRole, hasVideoEnabled);

        if (allocateChannel.audio.isSet())
        {
            const auto& audio = allocateChannel.audio.get();

            std::string outChannelId;
            if (!mixer->addBundledAudioStream(outChannelId,
                    endpointId,
                    audio.getMediaMode(),
                    allocateChannel.idleTimeoutSeconds))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
                    "It was not possible to add bundled audio stream. Usually happens due to an already existing audio "
                    "stream");
            }
            audioChannelId.set(outChannelId);
        }

        if (allocateChannel.video.isSet())
        {
            const auto& video = allocateChannel.video.get();
            const auto ssrcRewrite = video.relayType.compare("ssrc-rewrite") == 0;

            std::string outChannelId;
            if (mixer->addBundledVideoStream(outChannelId, endpointId, ssrcRewrite, allocateChannel.idleTimeoutSeconds))
            {
                videoChannelId.set(outChannelId);
            }
            else if (mixer->hasVideoEnabled())
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Add bundled video stream has failed");
            }
        }

        if (allocateChannel.data.isSet())
        {
            std::string outChannelId;
            if (!mixer->addBundledDataStream(outChannelId, endpointId, allocateChannel.idleTimeoutSeconds))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Add bundled data channel has failed");
            }
            dataChannelId.set(outChannelId);
        }
    }
    else
    {
        if (allocateChannel.audio.isSet())
        {
            const auto& audio = allocateChannel.audio.get();
            if (!audio.transport.isSet())
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
                    "Transport specification of audio channel is required");
            }

            const auto& transport = audio.transport.get();
            utils::Optional<ice::IceRole> iceRole;
            if (transport.ice)
            {
                iceRole.set(transport.iceControlling.isSet() && !transport.iceControlling.get()
                        ? ice::IceRole::CONTROLLED
                        : ice::IceRole::CONTROLLING);
            }

            std::string outChannelId;
            if (!mixer->addAudioStream(outChannelId,
                    endpointId,
                    iceRole,
                    audio.getMediaMode(),
                    allocateChannel.idleTimeoutSeconds))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Adding audio stream has failed");
            }
            audioChannelId.set(outChannelId);
        }

        if (allocateChannel.video.isSet())
        {
            const auto& video = allocateChannel.video.get();
            if (!video.transport.isSet())
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
                    "Transport specification of video channel is required");
            }

            const auto& transport = video.transport.get();
            utils::Optional<ice::IceRole> iceRole;
            if (transport.ice)
            {
                iceRole.set(transport.iceControlling.isSet() && !transport.iceControlling.get()
                        ? ice::IceRole::CONTROLLED
                        : ice::IceRole::CONTROLLING);
            }

            std::string outChannelId;
            const auto ssrcRewrite = video.relayType.compare("ssrc-rewrite") == 0;

            if (mixer->addVideoStream(outChannelId,
                    endpointId,
                    iceRole,
                    ssrcRewrite,
                    allocateChannel.idleTimeoutSeconds))
            {
                videoChannelId.set(outChannelId);
            }
            else if (mixer->hasVideoEnabled())
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Adding video stream has failed");
            }
        }

        if (allocateChannel.data.isSet())
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
                "Data channels are not supported for non-bundled transports");
        }
    }

    uint32_t totalSleepTimeMs = 0;

    if (allocateChannel.audio.isSet())
    {
        while (!mixer->isAudioStreamGatheringComplete(endpointId) && totalSleepTimeMs < gatheringCompleteMaxWaitMs)
        {
            totalSleepTimeMs += gatheringCompleteWaitMs;
            usleep(gatheringCompleteWaitMs * 1000);
        }
    }

    if (allocateChannel.video.isSet() && mixer->hasVideoEnabled())
    {
        while (!mixer->isVideoStreamGatheringComplete(endpointId) && totalSleepTimeMs < gatheringCompleteMaxWaitMs)
        {
            totalSleepTimeMs += gatheringCompleteWaitMs;
            usleep(gatheringCompleteWaitMs * 1000);
        }
    }

    if (allocateChannel.data.isSet())
    {
        while (!mixer->isDataStreamGatheringComplete(endpointId) && totalSleepTimeMs < gatheringCompleteMaxWaitMs)
        {
            totalSleepTimeMs += gatheringCompleteWaitMs;
            usleep(gatheringCompleteWaitMs * 1000);
        }
    }

    if (totalSleepTimeMs >= gatheringCompleteWaitMs)
    {
        logger::error("Allocate endpoint id %s, mixer %s, gathering did not complete in time",
            "RequestHandler",
            endpointId.c_str(),
            conferenceId.c_str());

        if (audioChannelId.isSet())
        {
            mixer->removeAudioStream(audioChannelId.get());
        }

        if (videoChannelId.isSet())
        {
            mixer->removeVideoStream(videoChannelId.get());
        }

        if (dataChannelId.isSet())
        {
            mixer->removeDataStream(dataChannelId.get());
        }

        throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR, "Candidates gathering timeout");
    }

    return generateAllocateEndpointResponse(context, requestLogger, allocateChannel, *mixer, conferenceId, endpointId);
}

std::vector<uint32_t> convertGroupIds(const std::vector<std::string>& groupIds)
{
    std::vector<uint32_t> result;
    result.reserve(groupIds.size());
    for (auto& gid : groupIds)
    {
        if (utils::isNumber(gid))
        {
            result.push_back(std::atoll(gid.c_str()) & 0xFFFFFFFFu);
        }
        else
        {
            result.push_back(utils::hash<std::string>()(gid));
        }
    }
    return result;
}
} // namespace

void configureAudioEndpoint(const api::EndpointDescription& endpointDescription,
    Mixer& mixer,
    const std::string& endpointId)
{
    const auto& audio = endpointDescription.audio.get();
    if (mixer.isAudioStreamConfigured(endpointId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Audio stream was configured already");
    }

    if (audio.payloadTypes.empty())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Audio payload type is required");
    }

    RtpMap audioRtpMap;
    RtpMap telephoneEventRtpMap;
    for (const auto& payloadType : audio.payloadTypes)
    {
        RtpMap rtpMap = makeRtpMap(audio, payloadType);
        if (rtpMap.format == bridge::RtpMap::Format::TELEPHONE_EVENT)
        {
            if (!telephoneEventRtpMap.isEmpty())
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Multiple telephone-event payload");
            }

            telephoneEventRtpMap = std::move(rtpMap);
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

    utils::Optional<uint32_t> remoteSsrc;
    if (!audio.ssrcs.empty())
    {
        remoteSsrc.set(audio.ssrcs.front());
    }

    std::vector<uint32_t> neighbours;
    if (endpointDescription.neighbours.isSet())
    {
        neighbours = convertGroupIds(endpointDescription.neighbours.get());
    }

    if (!mixer.configureAudioStream(endpointId, audioRtpMap, telephoneEventRtpMap, remoteSsrc, neighbours))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            utils::format("Audio stream not found for endpoint '%s'", endpointId.c_str()));
    }

    if (audio.transport.isSet())
    {
        const auto& transport = audio.transport.get();
        if (transport.ice.isSet())
        {
            const auto& candidatesAndCredentials = getIceCandidatesAndCredentials(transport);
            if (!mixer.configureAudioStreamTransportIce(endpointId,
                    candidatesAndCredentials.second,
                    candidatesAndCredentials.first))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "ICE configuration for audio stream has failed");
            }
        }
        else if (transport.connection.isSet())
        {
            const auto& connection = transport.connection.get();
            const auto remotePeer = transport::SocketAddress::parse(connection.ip, connection.port);
            if (!mixer.configureAudioStreamTransportConnection(endpointId, remotePeer))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Transport connection configuration for audio stream has failed");
            }
        }

        if (transport.dtls.isSet() && !transport.dtls.get().hash.empty())
        {
            const auto& dtls = transport.dtls.get();
            const bool isRemoteSideDtlsClient = dtls.isClient();

            if (!mixer.configureAudioStreamTransportDtls(endpointId, dtls.type, dtls.hash, !isRemoteSideDtlsClient))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "DTLS configuration for audio stream has failed");
            }
        }
        else if (!transport.sdesKeys.empty())
        {
            if (!mixer.configureAudioStreamTransportSdes(endpointId, transport.sdesKeys.front()))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "DTLS configuration for audio stream has failed");
            }
        }
        else
        {
            if (!mixer.getConfig().enableSrtpNullCipher)
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Null cipher not allowed");
            }
            if (!mixer.configureAudioStreamTransportDisableSrtp(endpointId))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Disabling DTLS for audio stream has failed");
            }
        }

        if (!mixer.addAudioStreamToEngine(endpointId) || !mixer.startAudioStreamTransport(endpointId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR, "Fail to start audio stream");
        }
    }
    else
    {
        if (!mixer.addAudioStreamToEngine(endpointId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR, "Fail to add audio stream");
        }
    }
}

void configureVideoEndpoint(const api::EndpointDescription& endpointDescription,
    Mixer& mixer,
    const std::string& endpointId)
{
    const auto& video = endpointDescription.video.get();
    if (mixer.isVideoStreamConfigured(endpointId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Video stream was configured already");
    }

    if (video.payloadTypes.empty())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Video payload type is required");
    }

    std::vector<RtpMap> rtpMaps;
    for (const auto& payloadType : video.payloadTypes)
    {
        rtpMaps.emplace_back(makeRtpMap(video, payloadType));
    }

    const auto feedbackRtpMap = rtpMaps.size() > 1 ? rtpMaps[1] : RtpMap();
    auto simulcastStreams = makeSimulcastStreams(video, endpointId);
    if (simulcastStreams.size() > 2)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Max simulcast streams allowed is 2");
    }

    utils::Optional<SimulcastStream> secondarySimulcastStream;
    if (simulcastStreams.size() == 2)
    {
        secondarySimulcastStream.set(simulcastStreams[1]);
    }
    else if (simulcastStreams.empty())
    {
        SimulcastStream emptySimulcastStream;
        memset(&emptySimulcastStream, 0, sizeof(SimulcastStream));
        simulcastStreams.emplace_back(emptySimulcastStream);
    }

    const auto ssrcWhitelist = makeWhitelistedSsrcsArray(video);

    if (!mixer.configureVideoStream(endpointId,
            rtpMaps.front(),
            feedbackRtpMap,
            simulcastStreams[0],
            secondarySimulcastStream,
            ssrcWhitelist))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Max simulcast streams allowed is 2");
    }

    if (video.transport.isSet())
    {
        const auto& transport = video.transport.get();

        if (transport.ice.isSet())
        {
            const auto& candidatesAndCredentials = getIceCandidatesAndCredentials(transport);
            if (!mixer.configureVideoStreamTransportIce(endpointId,
                    candidatesAndCredentials.second,
                    candidatesAndCredentials.first))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Video stream configuration has failed");
            }
        }
        else if (transport.connection.isSet())
        {
            const auto& connection = transport.connection.get();
            const auto remotePeer = transport::SocketAddress::parse(connection.ip, connection.port);
            if (!mixer.configureVideoStreamTransportConnection(endpointId, remotePeer))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Transport connection configuration for video stream has failed");
            }
        }

        if (transport.dtls.isSet() && !transport.dtls.get().hash.empty())
        {
            const auto& dtls = transport.dtls.get();
            const bool isRemoteSideDtlsClient = dtls.isClient();

            if (!mixer.configureVideoStreamTransportDtls(endpointId, dtls.type, dtls.hash, !isRemoteSideDtlsClient))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Disabling DTLS for video stream has failed");
            }
        }
        else if (!transport.sdesKeys.empty())
        {
            if (!mixer.configureVideoStreamTransportSdes(endpointId, transport.sdesKeys.front()))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "DTLS configuration for video stream has failed");
            }
        }
        else
        {
            if (!mixer.getConfig().enableSrtpNullCipher)
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Null cipher not allowed");
            }

            if (!mixer.configureVideoStreamTransportDisableSrtp(endpointId))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Disabling DTLS for video stream has failed");
            }
        }

        if (!mixer.addVideoStreamToEngine(endpointId) || !mixer.startVideoStreamTransport(endpointId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR, "Fail to start video stream");
        }
    }
    else
    {
        if (!mixer.addVideoStreamToEngine(endpointId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR, "Fail to add video stream");
        }
    }
}

void configureDataEndpoint(const api::EndpointDescription& endpointDescription,
    Mixer& mixer,
    const std::string& endpointId)
{
    const auto& data = endpointDescription.data.get();
    if (!mixer.configureDataStream(endpointId, data.port) || !mixer.addDataStreamToEngine(endpointId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR, "Fail to add data stream");
    }
}

httpd::Response configureEndpoint(ActionContext* context,
    RequestLogger& requestLogger,
    const api::EndpointDescription& endpointDescription,
    const std::string& conferenceId,
    const std::string& endpointId)
{
    Mixer* mixer;
    auto scopedMixerLock = getConferenceMixer(context, conferenceId, mixer);

    if (endpointDescription.audio.isSet())
    {
        configureAudioEndpoint(endpointDescription, *mixer, endpointId);
    }

    if (endpointDescription.video.isSet())
    {
        configureVideoEndpoint(endpointDescription, *mixer, endpointId);
    }

    if (endpointDescription.data.isSet())
    {
        configureDataEndpoint(endpointDescription, *mixer, endpointId);
    }

    if (endpointDescription.bundleTransport.isSet())
    {
        const auto& transport = endpointDescription.bundleTransport.get();

        if (transport.ice.isSet())
        {
            const auto candidatesAndCredentials = getIceCandidatesAndCredentials(transport);
            if (!mixer->configureBundleTransportIce(endpointId,
                    candidatesAndCredentials.second,
                    candidatesAndCredentials.first))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Bundled transport ICE configuration has failed");
            }
        }

        if (transport.dtls.isSet() && !transport.dtls.get().hash.empty())
        {
            const auto& dtls = transport.dtls.get();
            const bool isRemoteSideDtlsClient = dtls.isClient();

            if (!mixer->configureBundleTransportDtls(endpointId, dtls.type, dtls.hash, !isRemoteSideDtlsClient))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Bundled transport DTLS configuration has failed");
            }
        }
        else if (!transport.sdesKeys.empty())
        {
            mixer->configureBundleTransportSdes(endpointId, transport.sdesKeys[0]);
        }
        else
        {
            if (!mixer->getConfig().enableSrtpNullCipher)
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Null cipher not allowed");
            }
            mixer->configureBundleTransportSdes(endpointId, srtp::AesKey());
        }

        if (!mixer->startBundleTransport(endpointId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Start bundled transport has failed");
        }
    }

    const auto responseBody = nlohmann::json::object();
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response.headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}

httpd::Response reconfigureEndpoint(ActionContext* context,
    RequestLogger& requestLogger,
    const api::EndpointDescription& endpointDescription,
    const std::string& conferenceId,
    const std::string& endpointId)
{
    Mixer* mixer;
    auto scopedMixerLock = getConferenceMixer(context, conferenceId, mixer);

    const bool isAudioSet = endpointDescription.audio.isSet();
    const bool isVideoSet = endpointDescription.video.isSet();
    const bool isNeighboursSet = endpointDescription.neighbours.isSet();

    if (isAudioSet && !mixer->isAudioStreamConfigured(endpointId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            "Can't reconfigure audio because it was not configured in first place");
    }

    if (isNeighboursSet && !mixer->isAudioStreamConfigured(endpointId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            "Can't reconfigure neighbours because audio stream was not configured in first place");
    }

    if (isVideoSet && !mixer->isVideoStreamConfigured(endpointId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            "Can't reconfigure video because it was not configured in first place");
    }

    if (isAudioSet)
    {
        const auto& audio = endpointDescription.audio.get();
        utils::Optional<uint32_t> remoteSsrc;
        if (!audio.ssrcs.empty())
        {
            remoteSsrc.set(audio.ssrcs.front());
        }

        if (!mixer->reconfigureAudioStream(endpointId, remoteSsrc))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to reconfigure audio stream");
        }
    }

    if (isNeighboursSet)
    {
        auto neighbours = convertGroupIds(endpointDescription.neighbours.get());
        if (!mixer->reconfigureAudioStreamNeighbours(endpointId, neighbours))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to reconfigure audio stream's neighbours setting");
        }
    }

    if (isVideoSet)
    {
        const auto& video = endpointDescription.video.get();
        auto simulcastStreams = makeSimulcastStreams(video, endpointId);
        if (simulcastStreams.size() > 2)
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Max simulcast streams allowed is 2");
        }

        if (simulcastStreams.empty())
        {
            SimulcastStream emptySimulcastStream;
            memset(&emptySimulcastStream, 0, sizeof(SimulcastStream));
            simulcastStreams.emplace_back(emptySimulcastStream);
        }

        utils::Optional<SimulcastStream> secondarySimulcastStream;
        if (simulcastStreams.size() == 2)
        {
            secondarySimulcastStream.set(simulcastStreams[1]);
        }

        const auto ssrcWhitelist = makeWhitelistedSsrcsArray(video);
        if (!mixer->reconfigureVideoStream(endpointId, simulcastStreams[0], secondarySimulcastStream, ssrcWhitelist))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to reconfigure video stream");
        }
    }

    const auto responseBody = nlohmann::json::object();
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response.headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}

httpd::Response recordEndpoint(ActionContext* context,
    RequestLogger& requestLogger,
    const api::Recording& recording,
    const std::string& conferenceId)
{
    Mixer* mixer;
    auto scopedMixerLock = getConferenceMixer(context, conferenceId, mixer);

    const bool isRecordingStart =
        recording.isAudioEnabled || recording.isVideoEnabled || recording.isScreenshareEnabled;

    if (isRecordingStart)
    {
        RecordingDescription description;
        description.ownerId = recording.userId;
        description.recordingId = recording.recordingId;
        description.isAudioEnabled = recording.isAudioEnabled;
        description.isVideoEnabled = recording.isVideoEnabled;
        description.isScreenSharingEnabled = recording.isScreenshareEnabled;

        if (!mixer->addOrUpdateRecording(conferenceId, recording.channels, description))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to create/update recording streams");
        }
    }
    else if (!recording.channels.empty())
    {
        if (!mixer->removeRecordingTransports(conferenceId, recording.channels))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to remove recording channels");
        }
    }
    else
    {
        if (!mixer->removeRecording(recording.recordingId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                utils::format("Fail to remove recording id '%s'", recording.recordingId.c_str()));
        }
    }

    const auto responseBody = nlohmann::json::object();
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response.headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}

httpd::Response expireEndpoint(ActionContext* context,
    RequestLogger& requestLogger,
    const std::string& conferenceId,
    const std::string& endpointId)
{
    Mixer* mixer;
    auto scopedMixerLock = getConferenceMixer(context, conferenceId, mixer);

    mixer->removeAudioStream(endpointId);
    mixer->removeVideoStream(endpointId);
    mixer->removeDataStream(endpointId);

    const auto responseBody = nlohmann::json::object();
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response.headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}

httpd::Response processEndpointPutRequest(ActionContext* context,
    RequestLogger& requestLogger,
    const httpd::Request& request,
    const std::string& conferenceId,
    const std::string& endpointId)
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
        const auto endpointDescription = api::Parser::parsePatchEndpoint(requestBodyJson, endpointId);
        return configureEndpoint(context, requestLogger, endpointDescription, conferenceId, endpointId);
    }
    else if (action.compare("reconfigure") == 0)
    {
        const auto endpointDescription = api::Parser::parsePatchEndpoint(requestBodyJson, endpointId);
        return reconfigureEndpoint(context, requestLogger, endpointDescription, conferenceId, endpointId);
    }
    else if (action.compare("record") == 0)
    {
        const auto recording = api::Parser::parseRecording(requestBodyJson);
        return recordEndpoint(context, requestLogger, recording, conferenceId);
    }

    throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
        utils::format("Action '%s' is not supported", action.c_str()));
}

httpd::Response processEndpointPostRequest(ActionContext* context,
    RequestLogger& requestLogger,
    const httpd::Request& request,
    const std::string& conferenceId,
    const std::string& endpointId)
{
    const auto requestBodyJson = nlohmann::json::parse(request.body.getSpan());
    const auto actionJsonItr = requestBodyJson.find("action");
    if (actionJsonItr == requestBodyJson.end())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Missing required json property: action");
    }
    const auto& action = actionJsonItr->get<std::string>();

    if (action.compare("allocate") == 0)
    {
        const auto allocateChannel = api::Parser::parseAllocateEndpoint(requestBodyJson);
        return allocateEndpoint(context, requestLogger, allocateChannel, conferenceId, endpointId);
    }
    else if (action.compare("expire") == 0)
    {
        return expireEndpoint(context, requestLogger, conferenceId, endpointId);
    }
    else
    {
        return processEndpointPutRequest(context, requestLogger, request, conferenceId, endpointId);
    }
}

} // namespace bridge
