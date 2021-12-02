package com.symphony.simpleserver.smb.api;

import com.fasterxml.jackson.annotation.JsonSetter;

import java.util.List;

public class SmbPayloadType {
    public static class RtcpFeedback {
        public String type;
        public String subtype;
    }

    public static class Parameter {
        public String name;
        public String value;
    }

    public Integer id;
    public String name;
    public Integer clockrate;
    public Integer channels;
    public List<Parameter> parameters;

    @JsonSetter("rtcp-fbs")
    public List<RtcpFeedback> rtcpFeedbacks;
}
