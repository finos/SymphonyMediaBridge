package com.symphony.simpleserver.sdp.objects;

public class Fingerprint {
    public String type;
    public String hash;

    public Fingerprint(String type, String hash) {
        this.type = type;
        this.hash = hash;
    }

    public Fingerprint(Fingerprint other) {
        this.type = other.type;
        this.hash = other.hash;
    }

    @Override
    public String toString() {
        return "a=fingerprint:" + type + " " + hash + "\r\n";
    }
}
