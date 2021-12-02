package com.symphony.simpleserver.smb.api;

import com.fasterxml.jackson.annotation.JsonGetter;
import com.fasterxml.jackson.annotation.JsonSetter;

public class SmbTransport {
    public boolean rtcpMux;

    @JsonGetter("rtcp-mux")
    public boolean isRtcpMux() {
        return rtcpMux;
    }

    @JsonSetter("rtcp-mux")
    public void setRtcpMux(boolean rtcpMux) {
        this.rtcpMux = rtcpMux;
    }

    public SmbIce ice;
    public SmbDtls dtls;
}
