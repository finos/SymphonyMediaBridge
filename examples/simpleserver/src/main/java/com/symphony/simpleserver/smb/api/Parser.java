package com.symphony.simpleserver.smb.api;

import com.symphony.simpleserver.sdp.Candidate;
import com.symphony.simpleserver.sdp.ParserFailedException;
import com.symphony.simpleserver.sdp.SessionDescription;
import com.symphony.simpleserver.sdp.objects.*;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.UUID;
import org.springframework.stereotype.Component;

@Component public class Parser
{

    public Parser() {}

    public SessionDescription makeSdpOffer(SmbEndpointDescription endpointDescription,
        String endpointId,
        List<EndpointMediaStreams> endpointMediaStreams) throws ParserFailedException
    {
        int mediaDescriptionIndex = 0;

        final var offer = new SessionDescription();
        offer.group = new Group("BUNDLE");
        offer.msidSemantic = new MsidSemantic("WMS");

        final var bundleTransport = endpointDescription.bundleTransport;
        final var smbIce = bundleTransport.ice;
        final var smbDtls = bundleTransport.dtls;

        final var candidates = new ArrayList<Candidate>();
        for (var smbCandidate : smbIce.candidates)
        {
            candidates.add(new Candidate(smbCandidate.foundation,
                Candidate.Component.fromString(smbCandidate.component.toString()),
                Candidate.TransportType.fromString(smbCandidate.protocol),
                smbCandidate.priority,
                smbCandidate.ip,
                smbCandidate.port,
                Candidate.Type.fromString(smbCandidate.type)));
        }

        mediaDescriptionIndex =
            addSmbMids(endpointDescription, mediaDescriptionIndex, offer, smbIce, smbDtls, candidates);

        /*  addParticipantMids(endpointDescription,
              endpointId,
              endpointMediaStreams,
              mediaDescriptionIndex,
              offer,
              smbIce,
              smbDtls,
              candidates);
  */
        return offer;
    }

    private void addParticipantMids(SmbEndpointDescription endpointDescription,
        String endpointId,
        List<EndpointMediaStreams> endpointMediaStreams,
        int mediaDescriptionIndex,
        SessionDescription offer,
        SmbIce smbIce,
        SmbDtls smbDtls,
        List<Candidate> candidates) throws ParserFailedException
    {
        for (var endpointMediaStreamsEntry : endpointMediaStreams)
        {
            if (endpointMediaStreamsEntry.endpointId.equals(endpointId))
            {
                continue;
            }

            for (var mediaStream : endpointMediaStreamsEntry.mediaStreams)
            {
                if (mediaStream.ssrcs.isEmpty())
                {
                    continue;
                }

                if (mediaStream.type == MediaDescription.Type.AUDIO)
                {
                    final var participantAudio = makeAudioDescription(endpointDescription,
                        mediaDescriptionIndex,
                        smbIce,
                        smbDtls,
                        candidates,
                        mediaStream.ssrcs.get(0));

                    if (!endpointMediaStreamsEntry.active)
                    {
                        participantAudio.direction = Types.Direction.INACTIVE;
                        participantAudio.ssrcs.clear();
                    }

                    offer.mediaDescriptions.add(participantAudio);
                    offer.group.mids.add(participantAudio.mid);
                    ++mediaDescriptionIndex;
                }
                else if (mediaStream.type == MediaDescription.Type.VIDEO)
                {
                    final var participantVideo = makeVideoDescription(endpointDescription,
                        mediaDescriptionIndex,
                        smbIce,
                        smbDtls,
                        candidates,
                        mediaStream.ssrcs,
                        mediaStream.ssrcGroups);

                    if (!endpointMediaStreamsEntry.active)
                    {
                        participantVideo.direction = Types.Direction.INACTIVE;
                        participantVideo.ssrcs.clear();
                        participantVideo.ssrcGroups.clear();
                    }

                    offer.mediaDescriptions.add(participantVideo);
                    offer.group.mids.add(participantVideo.mid);
                    ++mediaDescriptionIndex;
                }

                for (var ssrc : mediaStream.ssrcs)
                {
                    offer.msidSemantic.ids.add(ssrc.mslabel);
                }
            }
        }
    }

    private int addSmbMids(SmbEndpointDescription endpointDescription,
        int mediaDescriptionIndex,
        SessionDescription offer,
        SmbIce smbIce,
        SmbDtls smbDtls,
        List<Candidate> candidates) throws ParserFailedException
    {

        for (final var ssrc : endpointDescription.audio.ssrcs)
        {
            final var smbAudioSsrc = new Ssrc(ssrc);
            final var uuid = UUID.randomUUID().toString();
            smbAudioSsrc.label = "label" + uuid;
            smbAudioSsrc.mslabel = "mslabel" + uuid;
            smbAudioSsrc.cname = "smbaudiocname";
            final var audio = makeAudioDescription(endpointDescription,
                mediaDescriptionIndex,
                smbIce,
                smbDtls,
                candidates,
                smbAudioSsrc);

            offer.mediaDescriptions.add(audio);
            offer.group.mids.add(audio.mid);
            offer.msidSemantic.ids.add(smbAudioSsrc.mslabel);
            mediaDescriptionIndex++;
        }

        for (final var smbVideoStream : endpointDescription.video.streams)
        {
            final var smbVideoMain = new Ssrc(smbVideoStream.sources.get(0).main);
            final var smbRtx = new Ssrc(smbVideoStream.sources.get(0).feedback);

            final var uuid = UUID.randomUUID().toString();
            smbVideoMain.label = "label" + uuid;
            smbVideoMain.mslabel = "mslabel" + uuid;
            smbVideoMain.cname = "smbvideocname";

            smbRtx.label = smbVideoMain.label;
            smbRtx.mslabel = smbVideoMain.mslabel;
            smbRtx.cname = smbVideoMain.cname;

            var ssrcs = new ArrayList<Ssrc>();
            ssrcs.add(smbVideoMain);
            var groups = new ArrayList<SsrcGroup>();
            if (!smbRtx.ssrc.equals("0"))
            {
                var fidGroup = new SsrcGroup("FID");
                fidGroup.ssrcs.add(smbVideoMain.ssrc);
                fidGroup.ssrcs.add(smbRtx.ssrc);
                groups.add(fidGroup);
                ssrcs.add(smbRtx);
            }

            final var video = makeVideoDescription(endpointDescription,
                mediaDescriptionIndex,
                smbIce,
                smbDtls,
                candidates,
                ssrcs,
                groups);
            if (smbVideoStream.content.equals("slides"))
            {
                video.content = smbVideoStream.content;
            }

            offer.mediaDescriptions.add(video);
            offer.group.mids.add(video.mid);
            offer.msidSemantic.ids.add(smbVideoMain.mslabel);
            mediaDescriptionIndex++;
        }

        final var data = new MediaDescription();
        data.connection = new Connection(Types.Net.IN, Types.Address.IP4, "0.0.0.0");
        data.type = MediaDescription.Type.APPLICATION;
        data.port = 10000;
        data.protocol = "DTLS/SCTP";
        data.payloadTypes.add(5000);
        data.label = "data" + mediaDescriptionIndex;
        data.mid = Integer.toString(mediaDescriptionIndex);
        data.rtcpMux = true;
        data.ice = new Ice();
        data.ice.ufrag = smbIce.ufrag;
        data.ice.pwd = smbIce.pwd;
        data.candidates = candidates;
        data.setup = Types.Setup.fromString(smbDtls.setup);
        data.fingerprint = new Fingerprint(smbDtls.type, smbDtls.hash);
        data.direction = Types.Direction.SEND_RECV;
        data.sctpMap = new SctpMap(5000, "webrtc-datachannel");
        data.sctpMap.maxMessageSize = 1024;

        offer.group.mids.add(data.mid);
        offer.mediaDescriptions.add(data);
        mediaDescriptionIndex++;

        return mediaDescriptionIndex;
    }

    private MediaDescription makeAudioDescription(SmbEndpointDescription endpointDescription,
        int mediaDescriptionIndex,
        SmbIce smbIce,
        SmbDtls smbDtls,
        List<Candidate> candidates,
        Ssrc ssrc) throws ParserFailedException
    {
        final var audio = new MediaDescription();
        audio.connection = new Connection(Types.Net.IN, Types.Address.IP4, "0.0.0.0");
        audio.type = MediaDescription.Type.AUDIO;
        audio.port = 10000;
        audio.protocol = "RTP/SAVPF";
        audio.label = "audio" + mediaDescriptionIndex;
        audio.mid = Integer.toString(mediaDescriptionIndex);
        audio.rtcpMux = true;
        audio.ice = new Ice();
        audio.ice.ufrag = smbIce.ufrag;
        audio.ice.pwd = smbIce.pwd;
        audio.candidates = candidates;
        audio.setup = Types.Setup.fromString(smbDtls.setup);
        audio.fingerprint = new Fingerprint(smbDtls.type, smbDtls.hash);
        audio.direction = Types.Direction.SEND_RECV;
        audio.ssrcs.add(ssrc);

        final var smbAudio = endpointDescription.audio;
        for (var smbPayloadType : smbAudio.payloadTypes)
        {
            if (smbPayloadType.name.equals("telephone-event"))
            {
                continue;
            }
            audio.payloadTypes.add(smbPayloadType.id);

            audio.rtpMaps.put(smbPayloadType.id, new RtpMap(smbPayloadType.name, smbPayloadType.clockrate, 2));

            final var rtcpFbs = new ArrayList<RtcpFb>();
            for (var smbRtcpFb : smbPayloadType.rtcpFeedbacks)
            {
                rtcpFbs.add(new RtcpFb(smbRtcpFb.type, smbRtcpFb.subtype));
            }
            audio.rtcpFbs.put(smbPayloadType.id, rtcpFbs);

            final var fmtpsStringBuilder = new StringBuilder();
            for (var parameter : smbPayloadType.parameters.entrySet())
            {
                fmtpsStringBuilder.append(parameter.getKey());
                fmtpsStringBuilder.append("=");
                fmtpsStringBuilder.append(parameter.getValue());
                fmtpsStringBuilder.append(";");
            }
            audio.fmtps.put(smbPayloadType.id, fmtpsStringBuilder.toString());

            smbAudio.rtpHeaderExtensions.forEach(smbHeaderExtension
                -> audio.headerExtensions.add(new ExtMap(smbHeaderExtension.id, smbHeaderExtension.uri)));
        }
        return audio;
    }

    private MediaDescription makeVideoDescription(SmbEndpointDescription endpointDescription,
        int mediaDescriptionIndex,
        SmbIce smbIce,
        SmbDtls smbDtls,
        List<Candidate> candidates,
        List<Ssrc> ssrcs,
        List<SsrcGroup> ssrcGroups) throws ParserFailedException
    {
        final var video = new MediaDescription();
        video.connection = new Connection(Types.Net.IN, Types.Address.IP4, "0.0.0.0");
        video.type = MediaDescription.Type.VIDEO;
        video.port = 10000;
        video.protocol = "RTP/SAVPF";
        video.label = "video" + mediaDescriptionIndex;
        video.mid = Integer.toString(mediaDescriptionIndex);
        video.rtcpMux = true;
        video.ice = new Ice();
        video.ice.ufrag = smbIce.ufrag;
        video.ice.pwd = smbIce.pwd;
        video.candidates = candidates;
        video.setup = Types.Setup.fromString(smbDtls.setup);
        video.fingerprint = new Fingerprint(smbDtls.type, smbDtls.hash);
        video.direction = Types.Direction.SEND_RECV;
        video.ssrcs.addAll(ssrcs);
        video.ssrcGroups.addAll(ssrcGroups);

        final var smbVideo = endpointDescription.video;
        for (var smbPayloadType : smbVideo.payloadTypes)
        {
            video.payloadTypes.add(smbPayloadType.id);
            video.rtpMaps.put(smbPayloadType.id, new RtpMap(smbPayloadType.name, smbPayloadType.clockrate, null));

            final var rtcpFbs = new ArrayList<RtcpFb>();
            for (var smbRtcpFb : smbPayloadType.rtcpFeedbacks)
            {
                rtcpFbs.add(new RtcpFb(smbRtcpFb.type, smbRtcpFb.subtype));
            }
            video.rtcpFbs.put(smbPayloadType.id, rtcpFbs);

            final var fmtpsStringBuilder = new StringBuilder();
            for (var parameter : smbPayloadType.parameters.entrySet())
            {
                fmtpsStringBuilder.append(parameter.getKey());
                fmtpsStringBuilder.append("=");
                fmtpsStringBuilder.append(parameter.getValue());
                fmtpsStringBuilder.append(";");
            }
            video.fmtps.put(smbPayloadType.id, fmtpsStringBuilder.toString());
        }

        smbVideo.rtpHeaderExtensions.forEach(smbHeaderExtension
            -> video.headerExtensions.add(new ExtMap(smbHeaderExtension.id, smbHeaderExtension.uri)));
        return video;
    }

    public SmbEndpointDescription makeEndpointDescription(SessionDescription sdpAnswer) throws ParserFailedException
    {

        SmbEndpointDescription endpointDescription = new SmbEndpointDescription();

        if (!sdpAnswer.group.semantics.equals("BUNDLE"))
        {
            throw new ParserFailedException();
        }

        if (sdpAnswer.mediaDescriptions.isEmpty())
        {
            throw new ParserFailedException();
        }

        final var firstMediaDesription = sdpAnswer.mediaDescriptions.get(0);

        final var bundleTransport = new SmbTransport();
        bundleTransport.rtcpMux = true;
        bundleTransport.ice = new SmbIce();
        bundleTransport.ice.ufrag = firstMediaDesription.ice.ufrag;
        bundleTransport.ice.pwd = firstMediaDesription.ice.pwd;
        bundleTransport.dtls = new SmbDtls();
        bundleTransport.dtls.setup = firstMediaDesription.setup.toString();
        bundleTransport.dtls.type = firstMediaDesription.fingerprint.type;
        bundleTransport.dtls.hash = firstMediaDesription.fingerprint.hash;

        bundleTransport.ice.candidates = new ArrayList<>();
        firstMediaDesription.candidates.forEach(candidate -> {
            final var smbCandidate = new SmbCandidate();
            smbCandidate.component = candidate.component == Candidate.Component.RTP ? 0 : 1;
            smbCandidate.generation = candidate.generation;
            smbCandidate.protocol = candidate.transportType.toString();
            smbCandidate.port = candidate.port;
            smbCandidate.ip = candidate.address;
            smbCandidate.relPort = candidate.remotePort;
            smbCandidate.relAddr = candidate.remoteAddress;
            smbCandidate.foundation = candidate.foundation;
            smbCandidate.priority = candidate.priority;
            smbCandidate.type = candidate.type.toString();
            smbCandidate.network = candidate.networkId;
            bundleTransport.ice.candidates.add(smbCandidate);
        });

        endpointDescription.bundleTransport = bundleTransport;

        for (final var mediaDescription : sdpAnswer.mediaDescriptions)
        {
            if (mediaDescription.type != MediaDescription.Type.APPLICATION &&
                mediaDescription.direction != Types.Direction.SEND_RECV)
            {
                continue;
            }

            if (mediaDescription.type == MediaDescription.Type.AUDIO)
            {
                final var audio = new SmbAudio();

                audio.ssrcs = new ArrayList<>();
                audio.payloadTypes = new ArrayList<>();
                mediaDescription.ssrcs.forEach(ssrc -> audio.ssrcs.add(Long.parseLong(ssrc.ssrc)));

                for (final var sdpPayloadType : mediaDescription.payloadTypes)
                {
                    var payloadType = new SmbPayloadType();
                    payloadType.id = sdpPayloadType;
                    payloadType.name = mediaDescription.rtpMaps.get(sdpPayloadType).codec;
                    payloadType.clockrate = mediaDescription.rtpMaps.get(sdpPayloadType).clockRate;
                    payloadType.channels = mediaDescription.rtpMaps.get(sdpPayloadType).parameter;
                    if (payloadType.name.equals("telephone-event"))
                    {
                        continue;
                    }

                    final var parameters = mediaDescription.fmtps.get(sdpPayloadType);
                    if (parameters != null)
                    {
                        final var parametersSplit = mediaDescription.fmtps.get(sdpPayloadType).split(";");
                        for (final var parameter : parametersSplit)
                        {
                            final var split = parameter.split("=");
                            payloadType.addParameter(split[0], split[1]);
                        }
                    }

                    audio.payloadTypes.add(payloadType);
                }

                audio.rtpHeaderExtensions = new ArrayList<>();
                mediaDescription.headerExtensions.forEach(element -> {
                    final var smbRtpHeaderExtension = new SmbRtpHeaderExtension();
                    smbRtpHeaderExtension.id = element.id;
                    smbRtpHeaderExtension.uri = element.value;
                    audio.rtpHeaderExtensions.add(smbRtpHeaderExtension);
                });

                endpointDescription.audio = audio;
            }
            else if (mediaDescription.type == MediaDescription.Type.VIDEO)
            {
                final var video = new SmbVideo();
                final var streamsMap = new HashMap<String, SmbVideoStream>();

                mediaDescription.ssrcs.forEach(ssrc -> {
                    final var smbVideoStream = streamsMap.computeIfAbsent(ssrc.mslabel, key -> {
                        final var value = new SmbVideoStream();
                        value.id = ssrc.mslabel;
                        value.content = "slides".equals(mediaDescription.content) ? "slides" : "video";
                        value.sources = new ArrayList<>();
                        return value;
                    });

                    final var feedbackGroup = mediaDescription.ssrcGroups.stream()
                                                  .filter(SsrcGroup::isFeedback)
                                                  .filter(element -> element.ssrcs.contains(ssrc.ssrc))
                                                  .findFirst();

                    if (feedbackGroup.isPresent() && feedbackGroup.get().isMainSsrc(ssrc))
                    {
                        final var smbVideoSource = new SmbVideoStream.SmbVideoSource();
                        smbVideoSource.main = Long.parseLong(feedbackGroup.get().ssrcs.get(0));
                        smbVideoSource.feedback = Long.parseLong(feedbackGroup.get().ssrcs.get(1));
                        smbVideoStream.sources.add(smbVideoSource);
                    }
                    else if (feedbackGroup.isEmpty())
                    {
                        final var smbVideoSource = new SmbVideoStream.SmbVideoSource();
                        smbVideoSource.main = Long.parseLong(ssrc.ssrc);
                        smbVideoStream.sources.add(smbVideoSource);
                    }
                });

                video.streams = new ArrayList<>(streamsMap.values());

                video.payloadTypes = new ArrayList<>();
                for (var payloadType : mediaDescription.payloadTypes)
                {
                    final var smbPayloadType = new SmbPayloadType();
                    smbPayloadType.id = payloadType;
                    smbPayloadType.name = mediaDescription.rtpMaps.get(payloadType).codec;
                    smbPayloadType.clockrate = mediaDescription.rtpMaps.get(payloadType).clockRate;
                    smbPayloadType.channels = null;

                    final var parameters = mediaDescription.fmtps.get(payloadType);
                    if (parameters != null)
                    {
                        final var parametersSplit = mediaDescription.fmtps.get(payloadType).split(";");
                        for (final var parameter : parametersSplit)
                        {
                            final var split = parameter.split("=");
                            smbPayloadType.addParameter(split[0], split[1]);
                        }
                    }

                    video.payloadTypes.add(smbPayloadType);
                }

                video.rtpHeaderExtensions = new ArrayList<>();
                mediaDescription.headerExtensions.forEach(element -> {
                    final var smbRtpHeaderExtension = new SmbRtpHeaderExtension();
                    smbRtpHeaderExtension.id = element.id;
                    smbRtpHeaderExtension.uri = element.value;
                    video.rtpHeaderExtensions.add(smbRtpHeaderExtension);
                });

                endpointDescription.video = video;
            }
            else if (mediaDescription.type == MediaDescription.Type.APPLICATION)
            {
                final var data = new SmbData();
                data.port = mediaDescription.sctpMap.number;
                endpointDescription.data = data;
            }
        }

        return endpointDescription;
    }
}
