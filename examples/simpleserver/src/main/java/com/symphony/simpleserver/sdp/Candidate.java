package com.symphony.simpleserver.sdp;

public class Candidate {
    public enum Component {
        RTP, RTCP;

        public static Component fromString(String value) throws ParserFailedException {
            switch (value) {
                case "1":
                    return RTP;
                case "2":
                    return RTCP;
                default:
                    throw new ParserFailedException(value);
            }
        }

        @Override
        public String toString() {
            switch (this) {
                case RTP:
                    return "1";
                case RTCP:
                    return "2";
                default:
                    throw new AssertionError();
            }
        }
    }

    public enum TransportType {
        UDP, TCP, SSLTCP;

        public static TransportType fromString(String value) throws ParserFailedException {
            switch (value.toLowerCase()) {
                case "udp":
                    return UDP;
                case "tcp":
                    return TCP;
                case "ssltcp":
                    return SSLTCP;
                default:
                    throw new ParserFailedException(value);
            }
        }

        @Override
        public String toString() {
            switch (this) {
                case UDP:
                    return "udp";
                case TCP:
                    return "tcp";
                case SSLTCP:
                    return "ssltcp";
                default:
                    return null;
            }
        }
    }

    public enum Type {
        HOST, SRFLX, PRFLX, RELAY;

        public static Type fromString(String value) throws ParserFailedException {
            switch (value) {
                case "host":
                    return HOST;
                case "srflx":
                    return SRFLX;
                case "prflx":
                    return PRFLX;
                case "relay":
                    return RELAY;
                default:
                    throw new ParserFailedException(value);
            }
        }

        @Override
        public String toString() {
            switch (this) {
                case HOST:
                    return "host";
                case SRFLX:
                    return "srflx";
                case PRFLX:
                    return "prflx";
                case RELAY:
                    return "relay";
               default:
                   return null;
            }
        }
    }

    public enum TcpType {
        ACTIVE, PASSIVE, SO;

        public static TcpType fromString(String value) throws ParserFailedException {
            switch (value) {
                case "active":
                    return ACTIVE;
                case "passive":
                    return PASSIVE;
                case "so":
                    return SO;
                default:
                    throw new ParserFailedException(value);
            }
        }
    }

    public String foundation;
    public Component component;
    public TransportType transportType;
    public long priority;
    public String address;
    public int port;
    public Type type;
    public String remoteAddress;
    public Integer remotePort;
    public TcpType tcpType;
    public String ufrag;
    public Integer generation;
    public Integer networkId;

    public Candidate(
            String foundation,
            Component component,
            TransportType transportType,
            long priority,
            String address,
            int port,
            Type type)
    {
        this.foundation = foundation;
        this.component = component;
        this.transportType = transportType;
        this.priority = priority;
        this.address = address;
        this.port = port;
        this.type = type;
        this.remoteAddress = null;
        this.remotePort = null;
        this.tcpType = null;
        this.ufrag = null;
        this.generation = null;
        this.networkId = null;
    }

    public Candidate(Candidate other) {
        this.foundation = other.foundation;
        this.component = other.component;
        this.transportType = other.transportType;
        this.priority = other.priority;
        this.address = other.address;
        this.port = other.port;
        this.type = other.type;
        this.remoteAddress = other.remoteAddress;
        this.remotePort = other.remotePort;
        this.tcpType = other.tcpType;
        this.ufrag = other.ufrag;
        this.generation = other.generation;
        this.networkId = other.networkId;
    }

    public Candidate(String line) throws ParserFailedException {
        this.remoteAddress = null;
        this.remotePort = null;
        this.tcpType = null;
        this.ufrag = null;
        this.generation = null;
        this.networkId = null;

        final String[] typeValue = line.split(":", 2);
        if (typeValue.length != 2 || !typeValue[0].equals("candidate")) {
            throw new ParserFailedException(line);
        }

        final String[] tokens = typeValue[1].split(" ");
        if (tokens.length < 8) {
            throw new ParserFailedException(line);
        }

        this.foundation = tokens[0];
        this.component = Candidate.Component.fromString(tokens[1]);
        this.transportType = Candidate.TransportType.fromString(tokens[2]);
        this.priority = Long.parseLong(tokens[3]);
        this.address = tokens[4];
        this.port = Integer.parseInt(tokens[5]);
        this.type = Candidate.Type.fromString(tokens[7]);

        int nextToken = 8;
        while (nextToken < tokens.length) {
            switch (tokens[nextToken]) {
                case "raddr":
                    this.remoteAddress = tokens[nextToken + 1];
                    break;
                case "rport":
                    this.remotePort = Integer.parseInt(tokens[nextToken + 1]);
                    break;
                case "generation":
                    this.generation = Integer.parseInt(tokens[nextToken + 1]);
                    break;
                case "network-id":
                    this.networkId = Integer.parseInt(tokens[nextToken + 1]);
                    break;
                case "tcptype":
                    this.tcpType = Candidate.TcpType.fromString(tokens[nextToken + 1]);
                    break;
                case "ufrag":
                    this.ufrag = tokens[nextToken + 1];
                    break;
                default:
                    break;
            }
            nextToken += 2;
        }
    }

    @Override
    public String toString() {
        final StringBuilder stringBuilder = new StringBuilder(512);
        stringBuilder.append("a=candidate:");
        stringBuilder.append(foundation);
        stringBuilder.append(" ");
        stringBuilder.append(component.toString());
        stringBuilder.append(" ");
        stringBuilder.append(transportType.toString());
        stringBuilder.append(" ");
        stringBuilder.append(priority);
        stringBuilder.append(" ");
        stringBuilder.append(address);
        stringBuilder.append(" ");
        stringBuilder.append(port);
        stringBuilder.append(" typ ");
        stringBuilder.append(type.toString());

        if (remoteAddress != null && remotePort != null) {
            stringBuilder.append(" raddr ");
            stringBuilder.append(remoteAddress);
            stringBuilder.append(" rport ");
            stringBuilder.append(remotePort.toString());
        }

        if (tcpType != null) {
            stringBuilder.append(" tcptype ");
            stringBuilder.append(tcpType.toString().toLowerCase());
        }

        if (generation != null) {
            stringBuilder.append(" generation ");
            stringBuilder.append(generation.toString());
        }

        if (ufrag != null) {
            stringBuilder.append(" ufrag ");
            stringBuilder.append(ufrag);
        }

        if (networkId != null) {
            stringBuilder.append(" network-id ");
            stringBuilder.append(networkId.toString());
        }

        stringBuilder.append("\r\n");

        return stringBuilder.toString();
    }
}
