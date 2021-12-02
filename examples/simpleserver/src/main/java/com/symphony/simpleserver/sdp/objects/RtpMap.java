package com.symphony.simpleserver.sdp.objects;

import java.util.Objects;

public class RtpMap {
    public String codec;
    public int clockRate;
    public Integer parameter;

    public RtpMap(String codec, int clockRate, Integer parameter) {
        this.codec = codec;
        this.clockRate = clockRate;
        this.parameter = parameter;
    }

    public RtpMap(RtpMap other) {
        this.codec = other.codec;
        this.clockRate = other.clockRate;
        this.parameter = other.parameter;
    }

    public String toString(int payloadType) {
        String result = "a=rtpmap:" + payloadType +
                " " +
                codec +
                "/" +
                clockRate;
        if (parameter != null) {
            return result + "/" + parameter + "\r\n";
        }

        return result + "\r\n";
    }

    @Override
    public boolean equals(Object obj) {
        if (!(obj instanceof RtpMap)) {
            return false;
        }
        final RtpMap other = (RtpMap)obj;

        return codec.equals(other.codec) &&
                clockRate == other.clockRate &&
                Objects.equals(parameter, other.parameter);
    }

    @Override
    public int hashCode() {
        return Objects.hash(codec, clockRate, parameter);
    }
}
