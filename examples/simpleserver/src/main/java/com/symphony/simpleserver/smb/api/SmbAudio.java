package com.symphony.simpleserver.smb.api;

import com.fasterxml.jackson.annotation.JsonGetter;
import com.fasterxml.jackson.annotation.JsonSetter;

import java.util.List;

public class SmbAudio {
    public List<String> ssrcs;

    public SmbPayloadType payloadType;

    @JsonGetter("payload-type")
    public SmbPayloadType getPayloadType() {
        return payloadType;
    }
    @JsonGetter("payload-type")
    public void setPayloadType(SmbPayloadType payloadType) {
        this.payloadType = payloadType;
    }

    public List<SmbRtpHeaderExtension> rtpHeaderExtensions;

    @JsonGetter("rtp-hdrexts")
    public List<SmbRtpHeaderExtension> getRtpHeaderExtensions() {
        return rtpHeaderExtensions;
    }

    @JsonSetter("rtp-hdrexts")
    public void setRtpHeaderExtensions(List<SmbRtpHeaderExtension> rtpHeaderExtensions) {
        this.rtpHeaderExtensions = rtpHeaderExtensions;
    }
}
