package com.symphony.simpleserver.smb.api;

import com.symphony.simpleserver.sdp.Candidate;
import com.symphony.simpleserver.sdp.ParserFailedException;
import com.symphony.simpleserver.sdp.SessionDescription;
import com.symphony.simpleserver.sdp.objects.*;
import org.springframework.stereotype.Component;
import org.springframework.util.StringUtils;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

@Component
public class Parser {

    public Parser() {
    }

    public SessionDescription makeSdpOffer(
            SmbEndpointDescription endpointDescription,
            String endpointId,
            List<EndpointMediaStreams> endpointMediaStreams) throws ParserFailedException
    {
        int mediaDescriptionIndex = 0;

        final var offer = new SessionDescription();
        offer.group = new Group("BUNDLE");
        offer.msidSemantic = new MsidSemantic("WMS");

        SmbIce bundleTransportIce = null;
        SmbDtls bundleTransportDtls = null;
        final var candidates = new ArrayList<Candidate>();

        final var bundleTransport = endpointDescription.bundleTransport;
        if (bundleTransport != null) {
            bundleTransportIce = bundleTransport.ice;
            bundleTransportDtls = bundleTransport.dtls;

            for (var smbCandidate : bundleTransportIce.candidates) {
                candidates.add(new Candidate(smbCandidate.foundation,
                        Candidate.Component.fromString(smbCandidate.component.toString()),
                        Candidate.TransportType.fromString(smbCandidate.protocol),
                        smbCandidate.priority,
                        smbCandidate.ip,
                        smbCandidate.port,
                        Candidate.Type.fromString(smbCandidate.type)));
            }
        }

        mediaDescriptionIndex = addSmbMids(endpointDescription,
                mediaDescriptionIndex,
                offer,
                bundleTransportIce,
                bundleTransportDtls,
                candidates);

        addParticipantMids(endpointDescription,
                endpointId,
                endpointMediaStreams,
                mediaDescriptionIndex,
                offer,
                bundleTransportIce,
                bundleTransportDtls,
                candidates);

        return offer;
    }

    private void addParticipantMids(
            SmbEndpointDescription endpointDescription,
            String endpointId,
            List<EndpointMediaStreams> endpointMediaStreams,
            int mediaDescriptionIndex,
            SessionDescription offer,
            SmbIce smbIce,
            SmbDtls smbDtls,
            List<Candidate> candidates) throws ParserFailedException
    {
        for (var endpointMediaStreamsEntry : endpointMediaStreams) {
            if (endpointMediaStreamsEntry.endpointId.equals(endpointId)) {
                continue;
            }

            for (var mediaStream : endpointMediaStreamsEntry.mediaStreams) {
                if (mediaStream.ssrcs.isEmpty()) {
                    continue;
                }

                if (mediaStream.type == MediaDescription.Type.AUDIO) {
                    final var participantAudio = makeAudioDescription(endpointDescription,
                            mediaDescriptionIndex,
                            smbIce,
                            smbDtls,
                            candidates,
                            mediaStream.ssrcs.get(0));

                    if (!endpointMediaStreamsEntry.active) {
                        participantAudio.direction = Types.Direction.INACTIVE;
                        participantAudio.ssrcs.clear();
                    }

                    offer.mediaDescriptions.add(participantAudio);
                    offer.group.mids.add(participantAudio.mid);
                    ++mediaDescriptionIndex;

                } else if (mediaStream.type == MediaDescription.Type.VIDEO) {
                    final var participantVideo = makeVideoDescription(endpointDescription,
                            mediaDescriptionIndex,
                            smbIce,
                            smbDtls,
                            candidates,
                            mediaStream.ssrcs,
                            mediaStream.ssrcGroups);

                    if (!endpointMediaStreamsEntry.active) {
                        participantVideo.direction = Types.Direction.INACTIVE;
                        participantVideo.ssrcs.clear();
                        participantVideo.ssrcGroups.clear();
                    }

                    offer.mediaDescriptions.add(participantVideo);
                    offer.group.mids.add(participantVideo.mid);
                    ++mediaDescriptionIndex;
                }

                for (var ssrc : mediaStream.ssrcs) {
                    offer.msidSemantic.ids.add(ssrc.mslabel);
                }
            }
        }
    }

    private int addSmbMids(
            SmbEndpointDescription endpointDescription,
            int mediaDescriptionIndex,
            SessionDescription offer,
            SmbIce bundleTransportIce,
            SmbDtls bundleTransportDtls,
            List<Candidate> bundleTransportCandidates) throws ParserFailedException
    {

        final var smbAudioSsrc = new Ssrc(endpointDescription.audio.ssrcs.get(0));
        smbAudioSsrc.label = "smbaudiolabel";
        smbAudioSsrc.mslabel = "smbaudiomslabel";
        smbAudioSsrc.cname = "smbaudiocname";
        final var audio = makeAudioDescription(endpointDescription,
                mediaDescriptionIndex,
                bundleTransportIce,
                bundleTransportDtls,
                bundleTransportCandidates,
                smbAudioSsrc);

        offer.mediaDescriptions.add(audio);
        offer.group.mids.add(audio.mid);
        offer.msidSemantic.ids.add(smbAudioSsrc.mslabel);
        mediaDescriptionIndex++;

        final var smbVideoStream = endpointDescription.video.streams.get(0);
        final var smbVideoSsrc = new Ssrc(smbVideoStream.sources.get(0).main);

        smbVideoSsrc.label = "smbvideolabel";
        smbVideoSsrc.mslabel = "smbvideomslabel";
        smbVideoSsrc.cname = "smbvideocname";
        final var video = makeVideoDescription(endpointDescription,
                mediaDescriptionIndex,
                bundleTransportIce,
                bundleTransportDtls,
                bundleTransportCandidates,
                List.of(smbVideoSsrc),
                List.of());

        offer.mediaDescriptions.add(video);
        offer.group.mids.add(video.mid);
        offer.msidSemantic.ids.add(smbVideoSsrc.mslabel);
        mediaDescriptionIndex++;

        if (endpointDescription.data != null) {
            final var data = new MediaDescription();
            data.connection = new Connection(Types.Net.IN, Types.Address.IP4, "0.0.0.0");
            data.type = MediaDescription.Type.APPLICATION;
            data.port = 10000;
            data.protocol = "DTLS/SCTP";
            data.payloadTypes.add(endpointDescription.data.port);
            data.label = "data" + mediaDescriptionIndex;
            data.mid = Integer.toString(mediaDescriptionIndex);
            data.rtcpMux = true;
            data.ice = new Ice();
            data.ice.ufrag = bundleTransportIce.ufrag;
            data.ice.pwd = bundleTransportIce.pwd;
            data.candidates = bundleTransportCandidates;
            data.setup = Types.Setup.fromString(bundleTransportDtls.setup);
            data.fingerprint = new Fingerprint(bundleTransportDtls.type, bundleTransportDtls.hash);
            data.direction = Types.Direction.SEND_RECV;
            data.sctpMap = new SctpMap(5000, "webrtc-datachannel");
            data.sctpMap.maxMessageSize = 1024;

            offer.group.mids.add(data.mid);
            offer.mediaDescriptions.add(data);
            mediaDescriptionIndex++;
        }

        return mediaDescriptionIndex;
    }

    private MediaDescription makeAudioDescription(
            SmbEndpointDescription endpointDescription,
            int mediaDescriptionIndex,
            SmbIce bundleTransportIce,
            SmbDtls bundleTransportDtls,
            List<Candidate> bundleTransportCandidates,
            Ssrc ssrc) throws ParserFailedException
    {
        final var smbAudio = endpointDescription.audio;

        SmbDtls dtls = bundleTransportDtls;
        SmbIce ice = bundleTransportIce;
        if (smbAudio.transport != null) {
            dtls = smbAudio.transport.dtls;
            ice = smbAudio.transport.ice;
        }

        final var audio = new MediaDescription();

        if (dtls == null) {
            audio.protocol = "RTP/AVPF";
        } else {
            audio.protocol = "UDP/TLS/RTP/SAVPF";
        }

        if (smbAudio.transport != null && smbAudio.transport.connection != null) {
            audio.connection = new Connection(Types.Net.IN, Types.Address.IP4, smbAudio.transport.connection.ip);
            audio.port = smbAudio.transport.connection.port;
            audio.rtcpMux = smbAudio.transport.isRtcpMux();
        } else {
            audio.connection = new Connection(Types.Net.IN, Types.Address.IP4, "0.0.0.0");
            audio.port = 10000;
            audio.rtcpMux = true;
        }

        if (ice != null) {
            audio.ice = new Ice();
            audio.ice.ufrag = ice.ufrag;
            audio.ice.pwd = ice.pwd;
            audio.candidates = bundleTransportCandidates;
        }

        if (dtls != null) {
            audio.setup = Types.Setup.fromString(dtls.setup);
            audio.fingerprint = new Fingerprint(dtls.type, dtls.hash);
        }

        audio.type = MediaDescription.Type.AUDIO;
        audio.label = "audio" + mediaDescriptionIndex;
        audio.mid = Integer.toString(mediaDescriptionIndex);

        audio.direction = Types.Direction.SEND_RECV;
        audio.ssrcs.add(ssrc);

        audio.payloadTypes.add(smbAudio.payloadType.id);
        audio.rtpMaps.put(smbAudio.payloadType.id,
                new RtpMap(smbAudio.payloadType.name, smbAudio.payloadType.clockrate, smbAudio.payloadType.channels));

        final var rtcpFbs = new ArrayList<RtcpFb>();
        for (var smbRtcpFb : smbAudio.payloadType.rtcpFeedbacks) {
            rtcpFbs.add(new RtcpFb(smbRtcpFb.type, smbRtcpFb.subtype));
        }
        audio.rtcpFbs.put(smbAudio.payloadType.id, rtcpFbs);

        final var fmtpsStringBuilder = new StringBuilder();
        for (var parameter : smbAudio.payloadType.parameters.entrySet()) {
            fmtpsStringBuilder.append(parameter.getKey());
            fmtpsStringBuilder.append("=");
            fmtpsStringBuilder.append(parameter.getValue());
            fmtpsStringBuilder.append(";");
        }
        audio.fmtps.put(smbAudio.payloadType.id, fmtpsStringBuilder.toString());

        smbAudio.rtpHeaderExtensions.forEach(smbHeaderExtension -> audio.headerExtensions.add(new ExtMap(smbHeaderExtension.id,
                smbHeaderExtension.uri)));
        return audio;
    }

    private MediaDescription makeVideoDescription(
            SmbEndpointDescription endpointDescription,
            int mediaDescriptionIndex,
            SmbIce bundleTransportIce,
            SmbDtls bundleTransportDtls,
            List<Candidate> bundleTransportCandidates,
            List<Ssrc> ssrcs,
            List<SsrcGroup> ssrcGroups) throws ParserFailedException
    {
        final var smbVideo = endpointDescription.video;

        SmbDtls dtls = bundleTransportDtls;
        SmbIce ice = bundleTransportIce;
        if (smbVideo.transport != null) {
            dtls = smbVideo.transport.dtls;
            ice = smbVideo.transport.ice;
        }

        final var video = new MediaDescription();
        if (dtls == null) {
            video.protocol = "RTP/AVPF";
        } else {
            video.protocol = "UDP/TLS/RTP/SAVPF";
        }

        if (smbVideo.transport != null && smbVideo.transport.connection != null) {
            video.connection = new Connection(Types.Net.IN, Types.Address.IP4, smbVideo.transport.connection.ip);
            video.port = smbVideo.transport.connection.port;
            video.rtcpMux = smbVideo.transport.isRtcpMux();
        } else {
            video.connection = new Connection(Types.Net.IN, Types.Address.IP4, "0.0.0.0");
            video.port = 10000;
            video.rtcpMux = true;
        }

        if (ice != null) {
            video.ice = new Ice();
            video.ice.ufrag = ice.ufrag;
            video.ice.pwd = ice.pwd;
            video.candidates = bundleTransportCandidates;
        }

        if (dtls != null) {
            video.setup = Types.Setup.fromString(dtls.setup);
            video.fingerprint = new Fingerprint(dtls.type, dtls.hash);
        }

        video.type = MediaDescription.Type.VIDEO;
        video.label = "video" + mediaDescriptionIndex;
        video.mid = Integer.toString(mediaDescriptionIndex);
        video.rtcpMux = true;
        video.direction = Types.Direction.SEND_RECV;
        video.ssrcs.addAll(ssrcs);
        video.ssrcGroups.addAll(ssrcGroups);

        for (var smbPayloadType : smbVideo.payloadTypes) {
            video.payloadTypes.add(smbPayloadType.id);
            video.rtpMaps.put(smbPayloadType.id, new RtpMap(smbPayloadType.name, smbPayloadType.clockrate, null));

            final var rtcpFbs = new ArrayList<RtcpFb>();
            for (var smbRtcpFb : smbPayloadType.rtcpFeedbacks) {
                rtcpFbs.add(new RtcpFb(smbRtcpFb.type, smbRtcpFb.subtype));
            }
            video.rtcpFbs.put(smbPayloadType.id, rtcpFbs);

            final var fmtpsStringBuilder = new StringBuilder();
            for (var parameter : smbPayloadType.parameters.entrySet()) {
                fmtpsStringBuilder.append(parameter.getKey());
                fmtpsStringBuilder.append("=");
                fmtpsStringBuilder.append(parameter.getValue());
                fmtpsStringBuilder.append(";");
            }
            video.fmtps.put(smbPayloadType.id, fmtpsStringBuilder.toString());
        }

        smbVideo.rtpHeaderExtensions.forEach(smbHeaderExtension -> video.headerExtensions.add(new ExtMap(smbHeaderExtension.id,
                smbHeaderExtension.uri)));
        return video;
    }

    public SmbEndpointDescription makeEndpointDescription(SessionDescription sdpAnswer) throws ParserFailedException {
        SmbEndpointDescription endpointDescription = new SmbEndpointDescription();

        //if (!sdpAnswer.group.semantics.equals("BUNDLE")) {
        //    throw new ParserFailedException();
        //}

        if (sdpAnswer.mediaDescriptions.isEmpty()) {
            throw new ParserFailedException();
        }

        final var firstMediaDescription = sdpAnswer.mediaDescriptions.get(0);

        boolean useBundleTransport = false;
        if (sdpAnswer.group != null && "BUNDLE".equals(sdpAnswer.group.semantics)) {
            useBundleTransport = true;
            final var bundleTransport = new SmbTransport();
            bundleTransport.rtcpMux = true;
            bundleTransport.ice = new SmbIce();
            bundleTransport.ice.ufrag = firstMediaDescription.ice.ufrag;
            bundleTransport.ice.pwd = firstMediaDescription.ice.pwd;
            bundleTransport.dtls = new SmbDtls();
            bundleTransport.dtls.setup = firstMediaDescription.setup.toString();
            bundleTransport.dtls.type = firstMediaDescription.fingerprint.type;
            bundleTransport.dtls.hash = firstMediaDescription.fingerprint.hash;

            bundleTransport.ice.candidates = new ArrayList<>();
            firstMediaDescription.candidates.forEach(candidate -> {
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
        }

        for (final var mediaDescription : sdpAnswer.mediaDescriptions) {
            if (mediaDescription.type != MediaDescription.Type.APPLICATION &&
                    mediaDescription.direction != Types.Direction.SEND_RECV)
            {
                continue;
            }

            if (mediaDescription.type == MediaDescription.Type.AUDIO) {
                final var audio = new SmbAudio();

                audio.ssrcs = new ArrayList<>();
                mediaDescription.ssrcs.forEach(ssrc -> audio.ssrcs.add(Long.parseLong(ssrc.ssrc)));

                audio.payloadType = new SmbPayloadType();
                final var firstPayloadType = mediaDescription.payloadTypes.get(0);
                audio.payloadType.id = firstPayloadType;
                audio.payloadType.name = mediaDescription.rtpMaps.get(firstPayloadType).codec;
                audio.payloadType.clockrate = mediaDescription.rtpMaps.get(firstPayloadType).clockRate;
                audio.payloadType.channels = mediaDescription.rtpMaps.get(firstPayloadType).parameter;

                if (!useBundleTransport) {
                    audio.transport = new SmbTransport();
                    if (mediaDescription.ice != null && StringUtils.hasText(mediaDescription.ice.ufrag)) {
                        audio.transport.ice = new SmbIce();
                        audio.transport.ice.ufrag = mediaDescription.ice.ufrag;
                        audio.transport.ice.pwd = mediaDescription.ice.pwd;
                    } else {
                        audio.transport.connection = new SmbConnection();
                        String address = null;
                        if (mediaDescription.connection != null && StringUtils.hasText(mediaDescription.connection.address)) {
                            if (!"0.0.0.0".equals(mediaDescription.connection.address)) {
                                address = mediaDescription.connection.address;
                            }
                        }

                        if (address == null && sdpAnswer.connection != null) {
                            if (StringUtils.hasText(sdpAnswer.connection.address) && !"0.0.0.0".equals(sdpAnswer.connection.address)) {
                                address = sdpAnswer.connection.address;
                            }
                        }

                        if (address == null) {
                            throw new ParserFailedException();
                        }

                        audio.transport.connection.ip = address;
                        audio.transport.connection.port = mediaDescription.port;
                    }

                    if (mediaDescription.setup != null && mediaDescription.fingerprint != null && StringUtils.hasText(mediaDescription.fingerprint.hash)) {
                        audio.transport.dtls = new SmbDtls();
                        audio.transport.dtls.setup = mediaDescription.setup.toString();
                        audio.transport.dtls.type = mediaDescription.fingerprint.type;
                        audio.transport.dtls.hash = mediaDescription.fingerprint.hash;
                    }
                }

                final var parameters = mediaDescription.fmtps.get(firstPayloadType);
                if (parameters != null) {
                    final var parametersSplit = mediaDescription.fmtps.get(firstPayloadType).split(";");
                    for (final var parameter : parametersSplit) {
                        final var split = parameter.split("=");
                        audio.payloadType.addParameter(split[0], split[1]);
                    }
                }

                audio.rtpHeaderExtensions = new ArrayList<>();
                mediaDescription.headerExtensions.forEach(element -> {
                    final var smbRtpHeaderExtension = new SmbRtpHeaderExtension();
                    smbRtpHeaderExtension.id = element.id;
                    smbRtpHeaderExtension.uri = element.value;
                    audio.rtpHeaderExtensions.add(smbRtpHeaderExtension);
                });

                endpointDescription.audio = audio;

            } else if (mediaDescription.type == MediaDescription.Type.VIDEO) {
                final var video = new SmbVideo();
                final var streamsMap = new HashMap<String, SmbVideoStream>();

                if (!useBundleTransport) {
                    video.transport = new SmbTransport();
                    if (mediaDescription.ice != null && StringUtils.hasText(mediaDescription.ice.ufrag)) {
                        video.transport.ice = new SmbIce();
                        video.transport.ice.ufrag = mediaDescription.ice.ufrag;
                        video.transport.ice.pwd = mediaDescription.ice.pwd;
                    } else {
                        video.transport.connection = new SmbConnection();
                        String address = null;
                        if (mediaDescription.connection != null && StringUtils.hasText(mediaDescription.connection.address)) {
                            if (!"0.0.0.0".equals(mediaDescription.connection.address)) {
                                address = mediaDescription.connection.address;
                            }
                        }

                        if (address == null && sdpAnswer.connection != null) {
                            if (StringUtils.hasText(sdpAnswer.connection.address) && !"0.0.0.0".equals(sdpAnswer.connection.address)) {
                                address = sdpAnswer.connection.address;
                            }
                        }

                        if (address == null) {
                            throw new ParserFailedException();
                        }

                        video.transport.connection.ip = address;
                        video.transport.connection.port = mediaDescription.port;
                    }

                    if (mediaDescription.setup != null && mediaDescription.fingerprint != null && StringUtils.hasText(mediaDescription.fingerprint.hash)) {
                        video.transport.dtls = new SmbDtls();
                        video.transport.dtls.setup = mediaDescription.setup.toString();
                        video.transport.dtls.type = mediaDescription.fingerprint.type;
                        video.transport.dtls.hash = mediaDescription.fingerprint.hash;
                    }
                }

                mediaDescription.ssrcs.forEach(ssrc -> {
                    final var smbVideoStream = streamsMap.computeIfAbsent(
                            ssrc.mslabel,
                            key -> {
                                final var value = new SmbVideoStream();
                                value.id = ssrc.mslabel;
                                value.content = "slides".equals(mediaDescription.content)?  "slides" : "video";
                                value.sources = new ArrayList<>();
                                return value;
                            });

                    final var feedbackGroup = mediaDescription.ssrcGroups.stream()
                            .filter(SsrcGroup::isFeedback)
                            .filter(element -> element.ssrcs.contains(ssrc.ssrc))
                            .findFirst();

                    if (feedbackGroup.isPresent() && feedbackGroup.get().isMainSsrc(ssrc)) {
                        final var smbVideoSource = new SmbVideoStream.SmbVideoSource();
                        smbVideoSource.main = Long.parseLong(feedbackGroup.get().ssrcs.get(0));
                        smbVideoSource.feedback = Long.parseLong(feedbackGroup.get().ssrcs.get(1));
                        smbVideoStream.sources.add(smbVideoSource);

                    } else if (feedbackGroup.isEmpty()) {
                        final var smbVideoSource = new SmbVideoStream.SmbVideoSource();
                        smbVideoSource.main = Long.parseLong(ssrc.ssrc);
                        smbVideoStream.sources.add(smbVideoSource);
                    }
                });

                video.streams = new ArrayList<>(streamsMap.values());

                video.payloadTypes = new ArrayList<>();
                for (var payloadType : mediaDescription.payloadTypes) {
                    final var smbPayloadType = new SmbPayloadType();
                    smbPayloadType.id = payloadType;
                    smbPayloadType.name = mediaDescription.rtpMaps.get(payloadType).codec;
                    smbPayloadType.clockrate = mediaDescription.rtpMaps.get(payloadType).clockRate;
                    smbPayloadType.channels = null;

                    final var parameters = mediaDescription.fmtps.get(payloadType);
                    if (parameters != null) {
                        final var parametersSplit = mediaDescription.fmtps.get(payloadType).split(";");
                        for (final var parameter : parametersSplit) {
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

            } else if (mediaDescription.type == MediaDescription.Type.APPLICATION) {
                final var data = new SmbData();
                data.port = mediaDescription.sctpMap.number;
                endpointDescription.data = data;
            }
        }

        return endpointDescription;

    }
}
