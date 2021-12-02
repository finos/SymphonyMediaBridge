package com.symphony.simpleserver.sdp.objects;

import java.util.ArrayList;

public class Group {
    public String semantics;
    public ArrayList<String> mids;

    public Group(String semantics) {
        this.semantics = semantics;
        this.mids = new ArrayList<>();
    }

    public Group(Group other) {
        this.semantics = other.semantics;
        this.mids = new ArrayList<>();
        this.mids.addAll(other.mids);
    }

    @Override
    public String toString() {
        final StringBuilder stringBuilder = new StringBuilder(4096);

        stringBuilder.append("a=group:");
        stringBuilder.append(semantics);

        for (String mid : mids) {
            stringBuilder.append(" ");
            stringBuilder.append(mid);
        }
        stringBuilder.append("\r\n");

        return stringBuilder.toString();
    }
}
