package com.symphony.simpleserver.smb.api;

import com.fasterxml.jackson.annotation.JsonAnySetter;
import com.fasterxml.jackson.annotation.JsonSetter;
import java.util.HashMap;
import java.util.Map;
import java.util.List;

public class SmbPayloadType {
    public static class RtcpFeedback {
        public String type;
        public String subtype;
    }

    public Integer id;
    public String name;
    public Integer clockrate;
    public Integer channels;
    public Map<String, Object> parameters = new HashMap<String, Object>();

    @JsonAnySetter
    public void addParameter(String key, Object value){
        parameters.put(key, value);
    }

        
    @JsonSetter("rtcp-fbs")
    public List<RtcpFeedback> rtcpFeedbacks;
}
