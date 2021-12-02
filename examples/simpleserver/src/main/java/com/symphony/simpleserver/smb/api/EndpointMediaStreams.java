package com.symphony.simpleserver.smb.api;

import java.util.ArrayList;
import java.util.List;

public class EndpointMediaStreams {
    public String endpointId;
    public List<MediaStream> mediaStreams;
    public boolean active;

    public EndpointMediaStreams(String endpointId, List<MediaStream> mediaStreams) {
        this.endpointId = endpointId;
        this.mediaStreams = new ArrayList<>(mediaStreams);
        this.active = true;
    }
}
