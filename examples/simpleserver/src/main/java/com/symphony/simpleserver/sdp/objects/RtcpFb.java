package com.symphony.simpleserver.sdp.objects;

public class RtcpFb {
    public String type;
    public String subtype;

    public RtcpFb(String type, String subtype) {
        this.type = type;
        this.subtype = subtype;
    }

    public RtcpFb(RtcpFb other) {
        this.type = other.type;
        this.subtype = other.subtype;
    }

    public String toString(String payloadType) {
        String result = "a=rtcp-fb:" +
                payloadType +
                " " +
                type;

        if (subtype != null) {
            return result + " " + subtype + "\r\n";
        }

        return result + "\r\n";
    }
}
