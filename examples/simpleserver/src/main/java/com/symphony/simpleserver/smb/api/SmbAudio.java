package com.symphony.simpleserver.smb.api;

import com.fasterxml.jackson.annotation.JsonGetter;
import com.fasterxml.jackson.annotation.JsonSetter;
import java.util.List;

public class SmbAudio
{
    public List<Long> ssrcs;

    public List<SmbPayloadType> payloadTypes;

    @JsonGetter("payload-types") public List<SmbPayloadType> getPayloadTypes() { return payloadTypes; }
    @JsonSetter("payload-types") public void setPayloadTypes(List<SmbPayloadType> payloadTypes)
    {
        this.payloadTypes = payloadTypes;
    }

    public List<SmbRtpHeaderExtension> rtpHeaderExtensions;

    @JsonGetter("rtp-hdrexts") public List<SmbRtpHeaderExtension> getRtpHeaderExtensions()
    {
        return rtpHeaderExtensions;
    }

    @JsonSetter("rtp-hdrexts") public void setRtpHeaderExtensions(List<SmbRtpHeaderExtension> rtpHeaderExtensions)
    {
        this.rtpHeaderExtensions = rtpHeaderExtensions;
    }
}
