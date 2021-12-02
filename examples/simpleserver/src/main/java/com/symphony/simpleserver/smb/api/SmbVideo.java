package com.symphony.simpleserver.smb.api;

import com.fasterxml.jackson.annotation.JsonGetter;
import com.fasterxml.jackson.annotation.JsonSetter;

import java.util.List;

public class SmbVideo {
    public List<String> ssrcs;

    public List<SmbSsrcGroup> ssrcGroups;

    @JsonGetter("ssrc-groups")
    public List<SmbSsrcGroup> getSsrcGroups() {
        return ssrcGroups;
    }

    @JsonSetter("ssrc-groups")
    public void setSsrcGroups(List<SmbSsrcGroup> ssrcGroups) {
        this.ssrcGroups = ssrcGroups;
    }

    public List<SmbPayloadType> payloadTypes;

    @JsonGetter("payload-types")
    public List<SmbPayloadType> getPayloadTypes() {
        return payloadTypes;
    }

    @JsonSetter("payload-types")
    public void setPayloadTypes(List<SmbPayloadType> payloadTypes) {
        this.payloadTypes = payloadTypes;
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

    public List<SmbSsrcAttribute> ssrcAttributes;

    @JsonGetter("ssrc-attributes")
    public List<SmbSsrcAttribute> getSsrcAttributes() {
        return ssrcAttributes;
    }

    @JsonSetter("ssrc-attributes")
    public void setSsrcAttributes(List<SmbSsrcAttribute> ssrcAttributes) {
        this.ssrcAttributes = ssrcAttributes;
    }
}
