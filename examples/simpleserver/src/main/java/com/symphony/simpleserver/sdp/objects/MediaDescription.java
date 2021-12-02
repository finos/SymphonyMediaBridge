package com.symphony.simpleserver.sdp.objects;

import com.symphony.simpleserver.sdp.Candidate;
import com.symphony.simpleserver.sdp.ParserFailedException;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;

/**
 * Represents an m-line in a SessionDescription.
 */
public class MediaDescription {
    public enum Type {
        AUDIO, VIDEO, APPLICATION;

        public static Type fromString(String value) throws ParserFailedException {
            switch (value) {
                case "audio":
                    return AUDIO;
                case "video":
                    return VIDEO;
                case "application":
                    return APPLICATION;
                default:
                    throw new ParserFailedException(value);
            }
        }

        @Override
        public String toString() {
            switch (this) {
                case AUDIO:
                    return "audio";
                case VIDEO:
                    return "video";
                case APPLICATION:
                    return "application";
                default:
                    throw new AssertionError();
            }
        }
    }

    public Type type;
    public int port;
    public String protocol;
    public List<Integer> payloadTypes;
    public String applicationParameter;

    public Connection connection;
    public Rtcp rtcp;
    public boolean rtcpMux;
    public String mid;
    public String label;

    public Bandwidth bandwidth;

    public Ice ice;
    public Fingerprint fingerprint;

    public Integer ptime;
    public Integer maxPtime;

    public Types.Direction direction;
    public Types.Setup setup;

    public HashMap<Integer, RtpMap> rtpMaps;
    public HashMap<Integer, String> fmtps;

    public RtcpFb rtcpFbWildcard;
    public HashMap<Integer, List<RtcpFb>> rtcpFbs;
    public List<ExtMap> headerExtensions;
    public String content; // https://tools.ietf.org/html/rfc4796

    public List<Ssrc> ssrcs;
    public List<SsrcGroup> ssrcGroups;

    public List<Candidate> candidates;

    public SctpMap sctpMap;

    public MediaDescription() {
        this.type = null;
        this.port = 0;
        this.protocol = null;
        this.payloadTypes = new ArrayList<>();
        this.applicationParameter = null;
        this.connection = null;
        this.rtcp = null;
        this.rtcpMux = false;
        this.mid = null;
        this.label = null;
        this.bandwidth = null;
        this.ice = null;
        this.fingerprint = null;
        this.ptime = null;
        this.maxPtime = null;
        this.direction = null;
        this.setup = null;
        this.rtpMaps = new HashMap<>();
        this.fmtps = new HashMap<>();
        this.rtcpFbWildcard = null;
        this.rtcpFbs = new HashMap<>();
        this.headerExtensions = new ArrayList<>();
        this.content = null;
        this.ssrcs = new ArrayList<>();
        this.ssrcGroups = new ArrayList<>();
        this.candidates = new ArrayList<>();
        this.sctpMap = null;
    }

    public MediaDescription(MediaDescription other) {
        this.type = other.type;
        this.port = other.port;
        this.protocol = other.protocol;
        this.payloadTypes = new ArrayList<>();
        this.payloadTypes.addAll(other.payloadTypes);

        this.applicationParameter = other.applicationParameter;
        this.connection = other.connection == null ? null : new Connection(other.connection);
        this.rtcp = other.rtcp == null ? null : new Rtcp(other.rtcp);
        this.rtcpMux = other.rtcpMux;
        this.mid = other.mid;
        this.label = other.label;
        this.bandwidth = other.bandwidth == null ? null : new Bandwidth(other.bandwidth);
        this.ice = other.ice == null ? null : new Ice(other.ice);
        this.fingerprint = other.fingerprint == null ? null : new Fingerprint(other.fingerprint);
        this.ptime = other.ptime;
        this.maxPtime = other.maxPtime;
        this.direction = other.direction;
        this.setup = other.setup;
        this.rtpMaps = new HashMap<>();
        other.rtpMaps.forEach((key, value) -> this.rtpMaps.put(key, new RtpMap(value)));

        this.fmtps = new HashMap<>();
        other.fmtps.forEach((key, value) -> this.fmtps.put(key, value));

        this.rtcpFbWildcard = other.rtcpFbWildcard == null ? null : new RtcpFb(other.rtcpFbWildcard);
        this.rtcpFbs = new HashMap<>();
        for (Integer otherRtcpFbsKey : other.rtcpFbs.keySet()) {
            final List<RtcpFb> otherRtcpFbs = other.rtcpFbs.get(otherRtcpFbsKey);
            final ArrayList<RtcpFb> thisRtcpFbs = new ArrayList<>();
            otherRtcpFbs.forEach(element -> thisRtcpFbs.add(new RtcpFb(element)));
            this.rtcpFbs.put(otherRtcpFbsKey, thisRtcpFbs);
        }

        this.headerExtensions = new ArrayList<>();
        other.headerExtensions.forEach(element -> this.headerExtensions.add(new ExtMap(element)));
        this.content = other.content;

        this.ssrcs = new ArrayList<>();
        other.ssrcs.forEach(element -> this.ssrcs.add(new Ssrc(element)));

        this.ssrcGroups = new ArrayList<>();
        other.ssrcGroups.forEach(element -> this.ssrcGroups.add(new SsrcGroup(element)));

        this.candidates = new ArrayList<>();
        other.candidates.forEach(element -> this.candidates.add(new Candidate(element)));

        this.sctpMap = other.sctpMap == null ? null : new SctpMap(other.sctpMap);
    }

    @Override
    public String toString() {
        final StringBuilder stringBuilder = new StringBuilder(4096);

        appendMLine(stringBuilder);

        if (connection != null) {
            stringBuilder.append(connection.toString());
        }

        if (rtcp != null) {
            stringBuilder.append(rtcp.toString());
        }

        if (rtcpMux) {
            stringBuilder.append("a=rtcp-mux\r\n");
        }

        if (mid != null) {
            stringBuilder.append("a=mid:");
            stringBuilder.append(mid);
            stringBuilder.append("\r\n");
        }

        if (label != null) {
            stringBuilder.append("a=label:");
            stringBuilder.append(label);
            stringBuilder.append("\r\n");
        }

        if (bandwidth != null) {
            stringBuilder.append(bandwidth.toString());
        }

        if (ice != null) {
            stringBuilder.append(ice.toString());
        }

        if (fingerprint != null) {
            stringBuilder.append(fingerprint.toString());
        }

        if (ptime != null) {
            stringBuilder.append("a=ptime:");
            stringBuilder.append(ptime.toString());
            stringBuilder.append("\r\n");
        }

        if (maxPtime != null) {
            stringBuilder.append("a=maxptime:");
            stringBuilder.append(maxPtime.toString());
            stringBuilder.append("\r\n");
        }

        if (direction != null) {
            stringBuilder.append("a=");
            stringBuilder.append(direction.toString());
            stringBuilder.append("\r\n");
        }

        if (setup != null) {
            stringBuilder.append("a=setup:");
            stringBuilder.append(setup.toString());
            stringBuilder.append("\r\n");
        }

        for (Integer payloadType : payloadTypes) {
            final RtpMap rtpMap = rtpMaps.get(payloadType);
            if (rtpMap != null) {
                stringBuilder.append(rtpMap.toString(payloadType));
            }

            final String fmtp = fmtps.get(payloadType);
            if (fmtp != null) {
                stringBuilder.append("a=fmtp:");
                stringBuilder.append(payloadType.toString());
                stringBuilder.append(" ");
                stringBuilder.append(fmtp);
                stringBuilder.append("\r\n");
            }

            final List<RtcpFb> payloadTypeRtcpfbs = rtcpFbs.get(payloadType);
            if (payloadTypeRtcpfbs != null) {
                for (RtcpFb rtcpFb : payloadTypeRtcpfbs) {
                    stringBuilder.append(rtcpFb.toString(payloadType.toString()));
                }
            }
        }

        if (rtcpFbWildcard != null) {
            stringBuilder.append(rtcpFbWildcard.toString("*"));
        }

        for (ExtMap extMap : headerExtensions) {
            stringBuilder.append(extMap.toString());
        }

        if (content != null) {
            stringBuilder.append("a=content:" + content);
            stringBuilder.append("\r\n");
        }

        for (SsrcGroup ssrcGroup : ssrcGroups) {
            stringBuilder.append(ssrcGroup.toString());
        }

        for (Ssrc ssrc : ssrcs) {
            stringBuilder.append(ssrc.toString());
        }

        for (Candidate candidate : candidates) {
            stringBuilder.append(candidate.toString());
        }

        if (sctpMap != null) {
            stringBuilder.append(sctpMap.toString());
        }

        return stringBuilder.toString();
    }

    private void appendMLine(StringBuilder stringBuilder) {
        stringBuilder.append("m=");
        stringBuilder.append(type.toString());
        stringBuilder.append(" ");
        stringBuilder.append(port);
        stringBuilder.append(" ");
        stringBuilder.append(protocol);

        for (Integer payloadType : payloadTypes) {
            stringBuilder.append(" ");
            stringBuilder.append(payloadType.toString());
        }

        if (type == Type.APPLICATION && applicationParameter != null) {
            stringBuilder.append(" ");
            stringBuilder.append(applicationParameter);
        }

        stringBuilder.append("\r\n");
    }
}
