package com.symphony.simpleserver.sdp.objects;

public class Bandwidth {
    public String modifier;
    public String value;

    public Bandwidth(String modifier, String value) {
        this.modifier = modifier;
        this.value = value;
    }

    public Bandwidth(Bandwidth other) {
        this.modifier = other.modifier;
        this.value = other.value;
    }

    @Override
    public String toString() {
        return "b=" +
                modifier +
                ":" +
                value +
                "\r\n";
    }
}
