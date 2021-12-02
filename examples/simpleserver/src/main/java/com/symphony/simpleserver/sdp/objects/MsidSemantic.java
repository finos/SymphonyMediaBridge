package com.symphony.simpleserver.sdp.objects;

import java.util.ArrayList;

public class MsidSemantic {
    public String semantic;
    public ArrayList<String> ids;

    public MsidSemantic(String semantic) {
        this.semantic = semantic;
        this.ids = new ArrayList<>();
    }

    public MsidSemantic(MsidSemantic other) {
        this.semantic = other.semantic;
        this.ids = other.ids;
    }

    @Override
    public String toString() {
        final StringBuilder stringBuilder = new StringBuilder(4096);
        stringBuilder.append("a=msid-semantic: ");
        stringBuilder.append(semantic);
        for (String id : ids) {
            stringBuilder.append(" ");
            stringBuilder.append(id);
        }
        stringBuilder.append("\r\n");

        return stringBuilder.toString();
    }
}
