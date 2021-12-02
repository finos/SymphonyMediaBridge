package com.symphony.simpleserver.smb.api;

import com.symphony.simpleserver.sdp.objects.MediaDescription;
import com.symphony.simpleserver.sdp.objects.Ssrc;
import com.symphony.simpleserver.sdp.objects.SsrcGroup;

import java.util.ArrayList;
import java.util.List;

public class MediaStream {
    public MediaDescription.Type type;
    public List<Ssrc> ssrcs;
    public List<SsrcGroup> ssrcGroups;

    public MediaStream(MediaDescription.Type type) {
        this.type = type;
        this.ssrcs = new ArrayList<>();
        this.ssrcGroups = new ArrayList<>();
    }

    @Override
    public String toString() {
        return "Mid{" + "type=" + type + ", ssrcs=" + ssrcs + ", ssrcGroups=" + ssrcGroups + "}";
    }
}
