package com.symphony.simpleserver.sdp.objects;

public class SctpMap {
    public int number;
    public String app;
    public Integer maxMessageSize;
    public Integer streams;

    public SctpMap(int number, String app) {
        this.number = number;
        this.app = app;
        this.maxMessageSize = null;
        this.streams = null;
    }

    public SctpMap(SctpMap other) {
        this.number = other.number;
        this.app = other.app;
        this.maxMessageSize = other.maxMessageSize;
        this.streams = other.streams;
    }

    @Override
    public String toString() {
        String result = "a=sctpmap:" + number +
                " " +
                app;

        if (maxMessageSize != null) {
            result += " " + maxMessageSize.toString();
            if (streams != null) {
                result += " " + streams.toString();
            }
        }

        return result + "\r\n";
    }
}
