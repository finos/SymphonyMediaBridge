#include "ApiActions.h"
#include "api/Generator.h"
#include "api/Parser.h"
#include "bridge/DataStreamDescription.h"
#include "bridge/Mixer.h"
#include "bridge/MixerManager.h"
#include "bridge/RequestLogger.h"
#include "bridge/StreamDescription.h"
#include "bridge/TransportDescription.h"
#include "bridge/actions/ApiHelpers.h"
#include "httpd/RequestErrorException.h"
#include "nlohmann/json.hpp"
#include "transport/dtls/SslDtls.h"
#include "utils/CheckedCast.h"
#include "utils/Format.h"

namespace bridge
{

const uint32_t gatheringCompleteMaxWaitMs = 5000;
const uint32_t gatheringCompleteWaitMs = 100;

httpd::Response generateAllocateEndpointResponse(ActionContext* context,
    RequestLogger& requestLogger,
    const api::AllocateEndpoint& allocateChannel,
    Mixer& mixer,
    const std::string& conferenceId,
    const std::string& endpointId)
{
    api::EndpointDescription channelsDescription;
    channelsDescription._endpointId = endpointId;

    // Describe bundle transport
    if (allocateChannel._bundleTransport.isSet())
    {
        const auto& bundleTransport = allocateChannel._bundleTransport.get();
        api::EndpointDescription::Transport responseBundleTransport;

        TransportDescription transportDescription;
        if (!mixer.getTransportBundleDescription(endpointId, transportDescription))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to get bundled description");
        }

        if (!bundleTransport._ice || !bundleTransport._dtls)
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
                utils::format("Bundling without ice is not supported, conference %s", conferenceId.c_str()));
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
        responseDtls.setup = "actpass";
        responseBundleTransport._dtls.set(responseDtls);

        responseBundleTransport._rtcpMux = true;
        channelsDescription._bundleTransport.set(responseBundleTransport);
    }

    // Describe audio, video and data streams
    if (allocateChannel._audio.isSet())
    {
        const auto& audio = allocateChannel._audio.get();
        api::EndpointDescription::Audio responseAudio;

        StreamDescription streamDescription;
        TransportDescription transportDescription;

        if (!mixer.getAudioStreamDescription(endpointId, streamDescription) ||
            !mixer.getAudioStreamTransportDescription(endpointId, transportDescription))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to get audio description");
        }

        responseAudio._ssrcs = streamDescription._localSsrcs;

        if (audio._transport.isSet())
        {
            const auto& transport = audio._transport.get();
            api::EndpointDescription::Transport responseTransport;

            if (transport._ice)
            {
                api::EndpointDescription::Ice responseIce;
                const auto& transportDescriptionIce = transportDescription._ice.get();
                responseIce._ufrag = transportDescriptionIce._iceCredentials.first;
                responseIce._pwd = transportDescriptionIce._iceCredentials.second;
                for (const auto& iceCandidate : transportDescriptionIce._iceCandidates)
                {
                    if (iceCandidate.type != ice::IceCandidate::Type::PRFLX)
                    {
                        responseIce._candidates.emplace_back(iceCandidateToApi(iceCandidate));
                    }
                }
                responseTransport._ice.set(responseIce);
                responseTransport._rtcpMux = true;
            }
            else
            {
                if (transportDescription._localPeer.isSet() && !transportDescription._localPeer.get().empty())
                {
                    api::EndpointDescription::Connection responseConnection;
                    responseConnection._ip = transportDescription._localPeer.get().ipToString();
                    responseConnection._port = transportDescription._localPeer.get().getPort();
                    responseTransport._connection.set(responseConnection);
                }

                responseTransport._rtcpMux = false;
            }

            if (transportDescription._dtls.isSet())
            {
                api::EndpointDescription::Dtls responseDtls;
                responseDtls.setup = "active";
                responseDtls.type = "sha-256";
                responseDtls.hash = context->sslDtls.getLocalFingerprint();
                responseTransport._dtls.set(responseDtls);
            }

            responseAudio._transport.set(responseTransport);
        }

        addDefaultAudioProperties(responseAudio);
        channelsDescription._audio.set(responseAudio);
    }

    if (allocateChannel._video.isSet())
    {
        const auto& video = allocateChannel._video.get();
        api::EndpointDescription::Video responseVideo;

        StreamDescription streamDescription;
        TransportDescription transportDescription;

        if (!mixer.getVideoStreamDescription(endpointId, streamDescription) ||
            !mixer.getVideoStreamTransportDescription(endpointId, transportDescription))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to get video description");
        }

        responseVideo._ssrcs = streamDescription._localSsrcs;
        if (responseVideo._ssrcs.size() > 1)
        {
            for (size_t i = 1; i < responseVideo._ssrcs.size() - 1; i += 2)
            {
                api::EndpointDescription::SsrcGroup responseSsrcGroup;
                responseSsrcGroup._ssrcs.push_back(responseVideo._ssrcs[i]);
                responseSsrcGroup._ssrcs.push_back(responseVideo._ssrcs[i + 1]);
                responseSsrcGroup._semantics = "FID";
                responseVideo._ssrcGroups.push_back(responseSsrcGroup);
            }

            api::EndpointDescription::SsrcAttribute responseSsrcAttribute;
            responseSsrcAttribute._ssrcs.push_back(responseVideo._ssrcs[1]);
            responseSsrcAttribute._content = "slides";
            responseVideo._ssrcAttributes.push_back(responseSsrcAttribute);
        }

        if (video._transport.isSet())
        {
            const auto& transport = video._transport.get();
            api::EndpointDescription::Transport responseTransport;

            if (transport._ice)
            {
                api::EndpointDescription::Ice responseIce;
                const auto& transportDescriptionIce = transportDescription._ice.get();
                responseIce._ufrag = transportDescriptionIce._iceCredentials.first;
                responseIce._pwd = transportDescriptionIce._iceCredentials.second;
                for (const auto& iceCandidate : transportDescriptionIce._iceCandidates)
                {
                    if (iceCandidate.type != ice::IceCandidate::Type::PRFLX)
                    {
                        responseIce._candidates.emplace_back(iceCandidateToApi(iceCandidate));
                    }
                }
                responseTransport._ice.set(responseIce);
                responseTransport._rtcpMux = true;
            }
            else
            {
                if (transportDescription._localPeer.isSet() && !transportDescription._localPeer.get().empty())
                {
                    api::EndpointDescription::Connection responseConnection;
                    responseConnection._ip = transportDescription._localPeer.get().ipToString();
                    responseConnection._port = transportDescription._localPeer.get().getPort();
                    responseTransport._connection.set(responseConnection);
                }

                responseTransport._rtcpMux = false;
            }

            if (transportDescription._dtls.isSet())
            {
                api::EndpointDescription::Dtls responseDtls;
                responseDtls.setup = "active";
                responseDtls.type = "sha-256";
                responseDtls.hash = context->sslDtls.getLocalFingerprint();
                responseTransport._dtls.set(responseDtls);
            }

            responseVideo._transport.set(responseTransport);
        }

        addDefaultVideoProperties(responseVideo);
        channelsDescription._video.set(responseVideo);
    }

    if (allocateChannel._data.isSet())
    {
        api::EndpointDescription::Data responseData;

        DataStreamDescription streamDescription;
        if (!mixer.getDataStreamDescription(endpointId, streamDescription))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to get data stream description");
        }

        responseData._port = streamDescription._sctpPort.isSet() ? streamDescription._sctpPort.get() : 5000;
        channelsDescription._data.set(responseData);
    }

    const auto responseBody = api::Generator::generateAllocateEndpointResponse(channelsDescription);
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response._headers["Content-type"] = "text/json";
    logger::debug("POST response %s", "RequestHandler", response._body.c_str());
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
    auto scopedMixerLock = getConferenceMixer(context, conferenceId, mixer);

    utils::Optional<std::string> audioChannelId;
    utils::Optional<std::string> videoChannelId;
    utils::Optional<std::string> dataChannelId;

    if (allocateChannel._bundleTransport.isSet())
    {
        const auto& bundleTransport = allocateChannel._bundleTransport.get();
        if (!bundleTransport._ice || !bundleTransport._dtls)
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
                "Bundle transports requires both ICE and DTLS");
        }

        const auto iceRole = bundleTransport._iceControlling.isSet() && !bundleTransport._iceControlling.get()
            ? ice::IceRole::CONTROLLED
            : ice::IceRole::CONTROLLING;

        mixer->addBundleTransportIfNeeded(endpointId, iceRole);

        if (allocateChannel._audio.isSet())
        {
            const auto& audio = allocateChannel._audio.get();
            const auto mixed = audio._relayType.compare("mixed") == 0;
            const auto ssrcRewrite = audio._relayType.compare("ssrc-rewrite") == 0;

            std::string outChannelId;
            if (!mixer->addBundledAudioStream(outChannelId, endpointId, mixed, ssrcRewrite))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
                    "It was not possible to add bundled audio stream. Usually happens due to an already existing audio "
                    "stream");
            }
            audioChannelId.set(outChannelId);
        }

        if (allocateChannel._video.isSet())
        {
            const auto& video = allocateChannel._video.get();
            const auto ssrcRewrite = video._relayType.compare("ssrc-rewrite") == 0;

            std::string outChannelId;
            if (!mixer->addBundledVideoStream(outChannelId, endpointId, ssrcRewrite))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Add bundled video stream has failed");
            }
            videoChannelId.set(outChannelId);
        }

        if (allocateChannel._data.isSet())
        {
            std::string outChannelId;
            if (!mixer->addBundledDataStream(outChannelId, endpointId))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Add bundled data channel has failed");
            }
            dataChannelId.set(outChannelId);
        }
    }
    else
    {
        if (allocateChannel._audio.isSet())
        {
            const auto& audio = allocateChannel._audio.get();
            if (!audio._transport.isSet())
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
                    "Transport specification of audio channel is required");
            }

            const auto& transport = audio._transport.get();
            utils::Optional<ice::IceRole> iceRole;
            if (transport._ice)
            {
                iceRole.set(transport._iceControlling.isSet() && !transport._iceControlling.get()
                        ? ice::IceRole::CONTROLLED
                        : ice::IceRole::CONTROLLING);
            }
            const auto mixed = audio._relayType.compare("mixed") == 0;

            std::string outChannelId;
            if (!mixer->addAudioStream(outChannelId, endpointId, iceRole, mixed, false))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Adding audio stream has failed");
            }
            audioChannelId.set(outChannelId);
        }

        if (allocateChannel._video.isSet())
        {
            const auto& video = allocateChannel._video.get();
            if (!video._transport.isSet())
            {
                throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
                    "Transport specification of video channel is required");
            }

            const auto& transport = video._transport.get();
            utils::Optional<ice::IceRole> iceRole;
            if (transport._ice)
            {
                iceRole.set(transport._iceControlling.isSet() && !transport._iceControlling.get()
                        ? ice::IceRole::CONTROLLED
                        : ice::IceRole::CONTROLLING);
            }

            std::string outChannelId;
            if (!mixer->addVideoStream(outChannelId, endpointId, iceRole, false))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Adding video stream has failed");
            }
            videoChannelId.set(outChannelId);
        }

        if (allocateChannel._data.isSet())
        {
            throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
                "Data channels are not supported for non-bundled transports");
        }
    }

    uint32_t totalSleepTimeMs = 0;

    if (allocateChannel._audio.isSet())
    {
        while (!mixer->isAudioStreamGatheringComplete(endpointId) && totalSleepTimeMs < gatheringCompleteMaxWaitMs)
        {
            totalSleepTimeMs += gatheringCompleteWaitMs;
            usleep(gatheringCompleteWaitMs * 1000);
        }
    }

    if (allocateChannel._video.isSet())
    {
        while (!mixer->isVideoStreamGatheringComplete(endpointId) && totalSleepTimeMs < gatheringCompleteMaxWaitMs)
        {
            totalSleepTimeMs += gatheringCompleteWaitMs;
            usleep(gatheringCompleteWaitMs * 1000);
        }
    }

    if (allocateChannel._data.isSet())
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

void configureAudioEndpoint(const api::EndpointDescription& endpointDescription,
    Mixer& mixer,
    const std::string& endpointId)
{
    const auto& audio = endpointDescription._audio.get();
    if (mixer.isAudioStreamConfigured(endpointId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Audio stream was configured already");
    }

    if (!audio._payloadType.isSet())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Audio payload type is required");
    }

    const auto rtpMap = makeRtpMap(audio._payloadType.get());
    const auto absSendTimeExtensionId = findAbsSendTimeExtensionId(audio._rtpHeaderExtensions);

    utils::Optional<uint32_t> remoteSsrc;
    if (!audio._ssrcs.empty())
    {
        remoteSsrc.set(audio._ssrcs.front());
    }

    utils::Optional<uint8_t> audioLevelExtensionId;
    for (const auto& rtpHeaderExtension : audio._rtpHeaderExtensions)
    {
        if (rtpHeaderExtension.second.compare("urn:ietf:params:rtp-hdrext:ssrc-audio-level") == 0)
        {
            audioLevelExtensionId.set(utils::checkedCast<uint8_t>(rtpHeaderExtension.first));
        }
    }

    if (!mixer.configureAudioStream(endpointId, rtpMap, remoteSsrc, audioLevelExtensionId, absSendTimeExtensionId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            utils::format("Audio stream not found for endpoint '%s'", endpointId.c_str()));
    }

    if (audio._transport.isSet())
    {
        const auto& transport = audio._transport.get();
        if (transport._ice.isSet())
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
        else if (transport._connection.isSet())
        {
            const auto& connection = transport._connection.get();
            const auto remotePeer = transport::SocketAddress::parse(connection._ip, connection._port);
            if (!mixer.configureAudioStreamTransportConnection(endpointId, remotePeer))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Transport connection configuration for audio stream has failed");
            }
        }

        if (!transport._dtls.isSet())
        {
            const auto& dtls = transport._dtls.get();
            const bool isRemoteSideDtlsClient = dtls.isClient();

            if (!mixer.configureAudioStreamTransportDtls(endpointId, dtls.type, dtls.hash, !isRemoteSideDtlsClient))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "DTLS configuration for audio stream has failed");
            }
        }
        else
        {
            if (!mixer.configureAudioStreamTransportDisableDtls(endpointId))
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
    const auto& video = endpointDescription._video.get();
    if (mixer.isVideoStreamConfigured(endpointId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Video stream was configured already");
    }

    if (video._payloadTypes.empty())
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Video payload type is required");
    }

    std::vector<RtpMap> rtpMaps;
    for (const auto& payloadType : video._payloadTypes)
    {
        rtpMaps.emplace_back(makeRtpMap(payloadType));
    }
    const auto absSendTimeExtensionId = findAbsSendTimeExtensionId(video._rtpHeaderExtensions);

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
            absSendTimeExtensionId,
            ssrcWhitelist))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST, "Max simulcast streams allowed is 2");
    }

    if (video._transport.isSet())
    {
        const auto& transport = video._transport.get();

        if (transport._ice.isSet())
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
        else if (transport._connection.isSet())
        {
            const auto& connection = transport._connection.get();
            const auto remotePeer = transport::SocketAddress::parse(connection._ip, connection._port);
            if (!mixer.configureVideoStreamTransportConnection(endpointId, remotePeer))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Transport connection configuration for video stream has failed");
            }
        }

        if (!transport._dtls.isSet())
        {
            const auto& dtls = transport._dtls.get();
            const bool isRemoteSideDtlsClient = dtls.isClient() == 0;

            if (!mixer.configureVideoStreamTransportDtls(endpointId, dtls.type, dtls.hash, !isRemoteSideDtlsClient))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Disabling DTLS for video stream has failed");
            }
        }
        else
        {
            if (!mixer.configureVideoStreamTransportDisableDtls(endpointId))
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
    const auto& data = endpointDescription._data.get();
    if (!mixer.configureDataStream(endpointId, data._port) || !mixer.addDataStreamToEngine(endpointId))
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

    if (endpointDescription._audio.isSet())
    {
        configureAudioEndpoint(endpointDescription, *mixer, endpointId);
    }

    if (endpointDescription._video.isSet())
    {
        configureVideoEndpoint(endpointDescription, *mixer, endpointId);
    }

    if (endpointDescription._data.isSet())
    {
        configureDataEndpoint(endpointDescription, *mixer, endpointId);
    }

    if (endpointDescription._bundleTransport.isSet())
    {
        const auto& transport = endpointDescription._bundleTransport.get();

        if (transport._ice.isSet())
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

        if (transport._dtls.isSet())
        {
            const auto& dtls = transport._dtls.get();
            const bool isRemoteSideDtlsClient = dtls.isClient();

            if (!mixer->configureBundleTransportDtls(endpointId, dtls.type, dtls.hash, !isRemoteSideDtlsClient))
            {
                throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                    "Bundled transport DTLS configuration has failed");
            }
        }

        if (!mixer->startBundleTransport(endpointId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Start bundled transport has failed");
        }
    }

    const auto responseBody = nlohmann::json::object();
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response._headers["Content-type"] = "text/json";
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

    if (!mixer->isAudioStreamConfigured(endpointId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            "Can't reconfigure audio because it was not configured in first place");
    }

    if (!mixer->isVideoStreamConfigured(endpointId))
    {
        throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
            "Can't reconfigure video because it was not configured in first place");
    }

    if (endpointDescription._audio.isSet())
    {
        const auto& audio = endpointDescription._audio.get();
        utils::Optional<uint32_t> remoteSsrc;
        if (!audio._ssrcs.empty())
        {
            remoteSsrc.set(audio._ssrcs.front());
        }

        if (!mixer->reconfigureAudioStream(endpointId, remoteSsrc))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to reconfigure audio stream");
        }
    }

    if (endpointDescription._video.isSet())
    {
        const auto& video = endpointDescription._video.get();
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
    response._headers["Content-type"] = "text/json";
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
        recording._isAudioEnabled || recording._isVideoEnabled || recording._isScreenshareEnabled;

    if (isRecordingStart)
    {
        RecordingDescription description;
        description._ownerId = recording._userId;
        description._recordingId = recording._recordingId;
        description._isAudioEnabled = recording._isAudioEnabled;
        description._isVideoEnabled = recording._isVideoEnabled;
        description._isScreenSharingEnabled = recording._isScreenshareEnabled;

        if (!mixer->addOrUpdateRecording(conferenceId, recording._channels, description))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to create/update recording streams");
        }
    }
    else if (!recording._channels.empty())
    {
        if (!mixer->removeRecordingTransports(conferenceId, recording._channels))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                "Fail to remove recording channels");
        }
    }
    else
    {
        if (!mixer->removeRecording(recording._recordingId))
        {
            throw httpd::RequestErrorException(httpd::StatusCode::INTERNAL_SERVER_ERROR,
                utils::format("Fail to remove recording id '%s'", recording._recordingId.c_str()));
        }
    }

    const auto responseBody = nlohmann::json::object();
    auto response = httpd::Response(httpd::StatusCode::OK, responseBody.dump());
    response._headers["Content-type"] = "text/json";
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
    response._headers["Content-type"] = "text/json";
    requestLogger.setResponse(response);
    return response;
}

httpd::Response processConferenceAction(ActionContext* context,
    RequestLogger& requestLogger,
    const httpd::Request& request,
    const utils::StringTokenizer::Token& incomingToken)
{
    auto token = utils::StringTokenizer::tokenize(incomingToken, '/');
    const auto conferenceId = token.str();
    if (!token.next)
    {
        throw httpd::RequestErrorException(httpd::StatusCode::NOT_FOUND, "Endpoint not found");
    }
    token = utils::StringTokenizer::tokenize(token, '/');
    const auto endpointId = token.str();

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
        const auto allocateChannel = api::Parser::parseAllocateEndpoint(requestBodyJson);
        return allocateEndpoint(context, requestLogger, allocateChannel, conferenceId, endpointId);
    }
    else if (action.compare("configure") == 0)
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
    else if (action.compare("expire") == 0)
    {
        return expireEndpoint(context, requestLogger, conferenceId, endpointId);
    }

    throw httpd::RequestErrorException(httpd::StatusCode::BAD_REQUEST,
        utils::format("Action '%s' is not supported", action.c_str()));
}
} // namespace bridge