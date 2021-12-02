package com.symphony.simpleserver.smb.api;

import com.fasterxml.jackson.annotation.JsonGetter;
import com.fasterxml.jackson.annotation.JsonSetter;

public class SmbEndpointDescription {
    public SmbTransport bundleTransport;

    @JsonGetter("bundle-transport")
    public SmbTransport getBundleTransport() {
        return bundleTransport;
    }

    @JsonSetter("bundle-transport")
    public void setBundleTransport(SmbTransport bundleTransport) {
        this.bundleTransport = bundleTransport;
    }

    public SmbAudio audio;
    public SmbVideo video;
    public SmbData data;
}
