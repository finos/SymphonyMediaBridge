package com.symphony.simpleserver.sdp.objects;

public class Time {
    public int startTime;
    public int stopTime;

    public Time(int startTime, int stopTime) {
        this.startTime = startTime;
        this.stopTime = stopTime;
    }

    public Time(Time other) {
        this.startTime = other.startTime;
        this.stopTime = other.stopTime;
    }

    @Override
    public String toString() {
        return "t=" + startTime + " " + stopTime + "\r\n";
    }
}
