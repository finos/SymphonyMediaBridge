package com.symphony.simpleserver.sdp;

import com.symphony.simpleserver.sdp.objects.*;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Represents an SDP offer or answer with methods to convert to and from strings.
 */
public class SessionDescription {
    private static final Logger LOGGER = LoggerFactory.getLogger(SessionDescription.class);

    private static class ParserState {
        boolean globalMode;
        int currentMediaContentIndex;

        ParserState() {
            this.globalMode = true;
            this.currentMediaContentIndex = -1;
        }
    }

    public int version;
    public Origin origin;
    public String sessionName;
    public Connection connection;
    public Time time;
    public Ice ice;
    public Fingerprint fingerprint;
    public Group group;
    public MsidSemantic msidSemantic;
    public List<Candidate> candidates;
    public List<MediaDescription> mediaDescriptions;
    public Bandwidth bandwidth;

    public SessionDescription() {
        this.version = 0;
        this.origin = new Origin("-", "0", 0, Types.Net.IN, Types.Address.IP4, "0.0.0.0");
        this.sessionName = "-";
        this.connection = null;
        this.time = new Time(0, 0);
        this.ice = null;
        this.fingerprint = null;
        this.group = null;
        this.msidSemantic = null;
        this.bandwidth = null;
        this.candidates = new ArrayList<>();
        this.mediaDescriptions = new ArrayList<>();
    }

    /**
     * Create a SessionDescription from a string
     */
    public SessionDescription(String sdpData) throws ParserFailedException {
        this();

        String lastLine = null;

        try {
            final ParserState parserState = new ParserState();

            for (String line : sdpData.split("\r\n")) {
                lastLine = line;

                final String[] tokens = line.split("=", 2);
                switch (tokens[0]) {
                    case "a":
                        handleAttribute(tokens[1], parserState);
                        break;

                    case "m":
                        parserState.globalMode = false;
                        parserState.currentMediaContentIndex += 1;
                        handleMediaLine(tokens[1]);
                        break;

                    case "c":
                        handleConnectionInfo(tokens[1], parserState);
                        break;

                    case "o":
                        handleOrigin(tokens[1], parserState);
                        break;

                    case "t":
                        handleTime(tokens[1], parserState);
                        break;

                    case "b":
                        handleBandwidth(tokens[1], parserState);
                        break;

                    case "s":
                        validateParserState(parserState, true, tokens[1]);
                        sessionName = tokens[1];
                        break;
                    case "v":
                        break; // we are fine with version line
                    default:
                        LOGGER.debug("Unhandled sdp line: {}", line);
                        break;
                }
            }

        } catch (Exception e) {
            throw new ParserFailedException(lastLine, e);
        }
    }

    @Override
    public String toString() {
        final StringBuilder stringBuilder = new StringBuilder(16384);

        stringBuilder.append("v=");
        stringBuilder.append(version);
        stringBuilder.append("\r\n");

        stringBuilder.append(origin.toString());
        stringBuilder.append("s=");
        stringBuilder.append(sessionName);
        stringBuilder.append("\r\n");

        if (time != null) {
            stringBuilder.append(time.toString());
        }

        if (group != null) {
            stringBuilder.append(group.toString());
        }

        if (msidSemantic != null) {
            stringBuilder.append(msidSemantic.toString());
        }

        if (connection != null) {
            stringBuilder.append(connection.toString());
        }

        if (ice != null) {
            stringBuilder.append(ice.toString());
        }

        if (fingerprint != null) {
            stringBuilder.append(fingerprint.toString());
        }

        if (bandwidth != null) {
            stringBuilder.append(bandwidth.toString());
        }

        for (MediaDescription mediaDescription : mediaDescriptions) {
            stringBuilder.append(mediaDescription.toString());
        }

        return stringBuilder.toString();
    }

    private void handleMediaLine(String line) throws ParserFailedException {
        final MediaDescription mediaDescription = new MediaDescription();

        final String[] tokens = line.split(" ");
        mediaDescription.type = MediaDescription.Type.fromString(tokens[0]);
        mediaDescription.port = Integer.parseInt(tokens[1]);
        mediaDescription.protocol = tokens[2];

        switch (mediaDescription.type) {
            case AUDIO:
            case VIDEO:
                // Tokens 3 and onward are payload types
                for (int tokenIndex = 3; tokenIndex < tokens.length; tokenIndex++) {
                    final int id = Integer.parseInt(tokens[tokenIndex]);
                    mediaDescription.payloadTypes.add(id);
                }
                break;

            case APPLICATION:
                mediaDescription.applicationParameter = tokens[3];
                break;
            default:
                throw new ParserFailedException(line);
        }

        mediaDescriptions.add(mediaDescription);
    }

    private void handleConnectionInfo(String line, ParserState parserState) throws ParserFailedException {
        final String[] tokens = line.split(" ");
        if (tokens.length != 3) {
            throw new ParserFailedException(line);
        }

        final Connection newConnection = new Connection(Types.Net.fromString(tokens[0]),
                Types.Address.fromString(tokens[1]),
                tokens[2]);

        if (parserState.globalMode) {
            connection = newConnection;

        } else {
            final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
            mediaDescription.connection = newConnection;
        }
    }

    private void handleOrigin(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, true, line);

        final String[] tokens = line.split(" ");
        if (tokens.length != 6) {
            throw new ParserFailedException(line);
        }

        origin = new Origin(tokens[0],
                tokens[1],
                Long.parseUnsignedLong(tokens[2]),
                Types.Net.fromString(tokens[3]),
                Types.Address.fromString(tokens[4]),
                tokens[5]);
    }

    private void handleTime(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, true, line);

        final String[] tokens = line.split(" ");
        if (tokens.length != 2) {
            throw new ParserFailedException(line);
        }

        time = new Time(Integer.parseInt(tokens[0]), Integer.parseInt(tokens[1]));
    }

    private void handleBandwidth(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, parserState.currentMediaContentIndex < 0, line);

        final String[] tokens = line.split(":");
        if (tokens.length != 2) {
            throw new ParserFailedException(line);
        }

        if (parserState.currentMediaContentIndex < 0) {
            this.bandwidth = new Bandwidth(tokens[0], tokens[1]);
        } else {
            final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
            mediaDescription.bandwidth = new Bandwidth(tokens[0], tokens[1]);
        }
    }

    private void handleAttribute(String line, ParserState parserState) throws ParserFailedException {
        final String[] tokens = line.split(":", 2);
        final String attributeType = tokens[0];

        final String tail = tokens.length > 1 ? tokens[1] : "";

        switch (attributeType) {
            case "group":
                handleGroup(tail, parserState);
                break;

            case "msid-semantic":
                handleMsidSemantic(tail, parserState);
                break;

            case "rtpmap":
                handleRtpmap(tail, parserState);
                break;

            case "fmtp":
                handleFmtpParameter(tail, parserState);
                break;

            case "rtcp-fb":
                handleRtcpFbParameter(tail, parserState);
                break;

            case "content":
                handleContent(tail, parserState);
                break;

            case "extmap":
                handleExtmap(tail, parserState);
                break;

            case "setup":
                handleSetup(tail, parserState);
                break;

            case "fingerprint":
                handleFingerprint(tail, parserState);
                break;

            case "sendrecv":
            case "sendonly":
            case "recvonly":
            case "inactive":
                handleDirection(attributeType, parserState);
                break;

            case "ice-ufrag":
                handleUfrag(tail, parserState);
                break;

            case "ice-pwd":
                handlePwd(tail, parserState);
                break;

            case "ice-options":
                handleIceOptions(tail, parserState);
                break;

            case "rtcp-mux":
                handleRtcpMux(parserState);
                break;

            case "ssrc":
                handleSsrc(tail, parserState);
                break;

            case "ssrc-group":
                handleSsrcGroupAttribute(tail, parserState);
                break;

            case "candidate":
                handleCandidate(line, parserState);
                break;

            case "mid":
                handleMid(tail, parserState);
                break;

            case "rtcp":
                handleRtcp(tail, parserState);
                break;

            case "sctpmap":
                handleSctpMap(tail, parserState);
                break;

            case "ptime":
                handlePtime(tail, parserState);
                break;

            case "maxptime":
                handleMaxPtime(tail, parserState);
                break;

            case "label":
                handleLabel(tail, parserState);
                break;

            default:
                LOGGER.debug("Unhandled attribute: " + line);
                break;
        }
    }

    private void handleDirection(String direction, ParserState parserState) throws ParserFailedException {
        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);

        switch (direction) {
            case "sendonly":
                mediaDescription.direction = Types.Direction.SEND_ONLY;
                break;
            case "recvonly":
                mediaDescription.direction = Types.Direction.RECV_ONLY;
                break;
            case "sendrecv":
                mediaDescription.direction = Types.Direction.SEND_RECV;
                break;
            case "inactive":
                mediaDescription.direction = Types.Direction.INACTIVE;
                break;
            default:
                throw new ParserFailedException(direction);

        }
    }

    private void handleGroup(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, true, line);

        final String[] tokens = line.split(" ");
        if (tokens.length < 2) {
            throw new ParserFailedException(line);
        }

        group = new Group(tokens[0]);
        group.mids.addAll(Arrays.asList(tokens).subList(1, tokens.length));
    }

    private void handleMsidSemantic(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, true, line);

        if (line.startsWith(" ")) {
            line = line.substring(1);
        }

        final String[] tokens = line.split(" ");
        if (tokens.length == 0) {
            throw new ParserFailedException(line);
        }

        final MsidSemantic newMsidSemantic = new MsidSemantic(tokens[0]);
        newMsidSemantic.ids.addAll(Arrays.asList(tokens).subList(1, tokens.length));

        msidSemantic = newMsidSemantic;
    }

    private void handleRtpmap(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);

        final String[] rtpMapTokens = line.split(" ");
        if (rtpMapTokens.length != 2) {
            return;
        }

        final int id = Integer.parseInt(rtpMapTokens[0]);
        final String[] values = rtpMapTokens[1].split("/");
        if (values.length < 2) {
            return;
        }
        final String codec = values[0];
        final int clockRate = Integer.parseInt(values[1]);
        final Integer parameter = values.length > 2 ? Integer.parseInt(values[2]) : null;

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
        if (!mediaDescription.payloadTypes.contains(id)) {
            throw new ParserFailedException(line);
        }

        mediaDescription.rtpMaps.put(id, new RtpMap(codec, clockRate, parameter));
    }

    private void handleRtcpFbParameter(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);

        final String[] tokens = line.split(" ", 2);
        final String id = tokens[0];

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);

        if (!id.equals("*") && !mediaDescription.payloadTypes.contains(Integer.parseInt(id))) {
            throw new ParserFailedException(line);
        }

        final String[] typeSubType = tokens[1].split(" ", 2);
        final String type = typeSubType[0];
        final String subType = typeSubType.length > 1 ? typeSubType[1] : null;
        final RtcpFb rtcpFb = new RtcpFb(type, subType);

        if (id.equals("*")) {
            mediaDescription.rtcpFbWildcard = rtcpFb;
        } else {
            final int idInt = Integer.parseInt(id);
            final List<RtcpFb> rtcpFbs = mediaDescription.rtcpFbs.computeIfAbsent(idInt, key -> new ArrayList<>());

            rtcpFbs.add(rtcpFb);
        }
    }

    private void handleFmtpParameter(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);

        final String[] tokens = line.split(" ", 2);
        if (tokens.length < 2) {
            throw new ParserFailedException(line);
        }

        final int id = Integer.parseInt(tokens[0]);

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
        if (mediaDescription.payloadTypes.contains(id)) {
            mediaDescription.fmtps.put(id, tokens[1]);
        }
    }

    private void handleExtmap(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);

        String[] tokens = line.split(" ");
        if (tokens.length != 2) {
            throw new ParserFailedException(line);
        }

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
        mediaDescription.headerExtensions.add(new ExtMap(Integer.parseInt(tokens[0]), tokens[1]));
    }

    private void handleContent(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
        mediaDescription.content = line;
    }

    private void handleSetup(String value, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, "setup " + value);

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
        switch (value) {
            case "active":
                mediaDescription.setup = Types.Setup.ACTIVE;
                break;
            case "passive":
                mediaDescription.setup = Types.Setup.PASSIVE;
                break;
            case "actpass":
                mediaDescription.setup = Types.Setup.ACTPASS;
                break;
            default:
                throw new ParserFailedException("setup " + value);
        }
    }

    private void handleFingerprint(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);

        final String[] tokens = line.split(" ");
        if (tokens.length != 2) {
            throw new ParserFailedException(line);
        }

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
        mediaDescription.fingerprint = new Fingerprint(tokens[0], tokens[1]);
    }

    private void handleUfrag(String line, ParserState parserState) {
        if (parserState.globalMode) {
            if (ice == null) {
                ice = new Ice();
            }
            ice.ufrag = line;

        } else {
            final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
            if (mediaDescription.ice == null) {
                mediaDescription.ice = new Ice();
            }
            mediaDescription.ice.ufrag = line;
        }
    }

    private void handlePwd(String line, ParserState parserState) {
        if (parserState.globalMode) {
            if (ice == null) {
                ice = new Ice();
            }
            ice.pwd = line;

        } else {
            final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
            if (mediaDescription.ice == null) {
                mediaDescription.ice = new Ice();
            }
            mediaDescription.ice.pwd = line;
        }
    }

    private void handleIceOptions(String line, ParserState parserState) {
        if (parserState.globalMode) {
            if (ice == null) {
                ice = new Ice();
            }
            ice.options.add(line);

        } else {
            final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
            if (mediaDescription.ice == null) {
                mediaDescription.ice = new Ice();
            }
            mediaDescription.ice.options.add(line);
        }
    }

    private void handleRtcpMux(ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, "rtcp-mux");

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
        mediaDescription.rtcpMux = true;
    }

    private void handleSsrc(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);

        final String[] tokens = line.split(" ");
        final String[] paramNameAndParam0 = tokens[1].split(":");
        final String paramName = paramNameAndParam0[0];
        final String param0 = paramNameAndParam0[1];
        String param1 = "";
        if (tokens.length > 2) {
            param1 = tokens[2];
        }

        Ssrc ssrc = null;
        for (Ssrc element : mediaDescription.ssrcs) {
            if (element.ssrc.equals(tokens[0])) {
                ssrc = element;
                break;
            }
        }

        if (ssrc == null) {
            ssrc = new Ssrc(tokens[0]);
            mediaDescription.ssrcs.add(ssrc);
        }

        switch (paramName) {
            case "cname":
                ssrc.cname = param0;
                break;
            case "msid":
                ssrc.mslabel = param0;
                ssrc.label = param1;
                break;
            case "label":
                ssrc.label = param0;
                break;
            case "mslabel":
                ssrc.mslabel = param0;
                break;
            case "content":
                ssrc.content = param0;
                break;
            default:
                break;
        }
    }

    private void handleSsrcGroupAttribute(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);
        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);

        final String[] tokens = line.split(" ");
        if (tokens.length < 2) {
            throw new ParserFailedException(line);
        }

        final SsrcGroup ssrcGroup = new SsrcGroup(tokens[0]);
        ssrcGroup.ssrcs.addAll(Arrays.asList(tokens).subList(1, tokens.length));

        mediaDescription.ssrcGroups.add(ssrcGroup);
    }

    private void handleCandidate(String line, ParserState parserState) throws ParserFailedException {
        final Candidate candidate = new Candidate(line);

        if (parserState.globalMode) {
            candidates.add(candidate);
        } else {
            final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
            mediaDescription.candidates.add(candidate);
        }
    }

    private void handleMid(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
        mediaDescription.mid = line;
    }

    private void handleRtcp(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);

        final String[] tokens = line.split(" ");
        if (tokens.length != 1 && tokens.length != 4) {
            throw new ParserFailedException(line);
        }

        Connection connection = null;
        if (tokens.length == 4) {
            connection = new Connection(Types.Net.fromString(tokens[1]),
                    Types.Address.fromString(tokens[2]),
                    tokens[3]);
        }

        mediaDescription.rtcp = new Rtcp(Integer.parseInt(tokens[0]), connection);
    }

    private void handleSctpMap(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);

        final String[] tokens = line.split(" ");
        if (tokens.length < 2) {
            throw new ParserFailedException(line);
        }

        final SctpMap newSctpMap = new SctpMap(Integer.parseInt(tokens[0]), tokens[1]);
        if (tokens.length > 2) {
            newSctpMap.maxMessageSize = Integer.parseInt(tokens[2]);
            if (tokens.length > 3) {
                newSctpMap.streams = Integer.parseInt(tokens[3]);
            }
        }

        mediaDescription.sctpMap = newSctpMap;
    }

    private void handlePtime(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
        mediaDescription.ptime = Integer.parseInt(line);
    }

    private void handleMaxPtime(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
        mediaDescription.maxPtime = Integer.parseInt(line);
    }

    private void handleLabel(String line, ParserState parserState) throws ParserFailedException {
        validateParserState(parserState, false, line);

        final MediaDescription mediaDescription = mediaDescriptions.get(parserState.currentMediaContentIndex);
        mediaDescription.label = line;
    }

    private static void validateParserState(ParserState parserState, boolean isGlobalAttribute, String sdpLine) throws
            ParserFailedException
    {
        if (parserState.globalMode != isGlobalAttribute) {
            throw new ParserFailedException("Invalid parser state for sdp line " + sdpLine);
        }
    }
}
