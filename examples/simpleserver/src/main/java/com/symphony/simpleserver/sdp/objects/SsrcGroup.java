package com.symphony.simpleserver.sdp.objects;

import java.util.ArrayList;

public class SsrcGroup {
    public String semantics;
    public ArrayList<String> ssrcs;

    public SsrcGroup(String semantics) {
        this.semantics = semantics;
        this.ssrcs = new ArrayList<>();
    }

    public SsrcGroup(SsrcGroup other) {
        this.semantics = other.semantics;
        this.ssrcs = new ArrayList<>();
        this.ssrcs.addAll(other.ssrcs);
    }

    public SsrcGroup() {
        this.semantics = null;
        this.ssrcs = new ArrayList<>();
    }

    @Override
    public String toString() {
        final StringBuilder stringBuilder = new StringBuilder();

        stringBuilder.append("a=ssrc-group:");
        stringBuilder.append(semantics);
        for (String ssrc : ssrcs) {
            stringBuilder.append(" ");
            stringBuilder.append(ssrc);
        }
        stringBuilder.append("\r\n");

        return stringBuilder.toString();
    }

    public boolean isFeedback() {
        return semantics.equals("FID");
    }

    public boolean isSimulcast() {
        return semantics.equals("SIM");
    }

    public boolean isMainSsrc(Ssrc ssrc) {
        if (!isFeedback()) {
            throw new AssertionError();
        }
        return ssrcs.size() == 2 && ssrc.ssrc.equals(ssrcs.get(0));
    }

    public boolean isFeedbackSsrc(Ssrc ssrc) {
        if (!isFeedback()) {
            throw new AssertionError();
        }
        return ssrcs.size() == 2 && ssrc.ssrc.equals(ssrcs.get(0));
    }
}
