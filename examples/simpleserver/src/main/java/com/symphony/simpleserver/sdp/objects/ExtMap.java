package com.symphony.simpleserver.sdp.objects;

public class ExtMap {
    public int id;
    public String value;

    public ExtMap(int id, String value) {
        this.id = id;
        this.value = value;
    }

    public ExtMap(ExtMap other) {
        this.id = other.id;
        this.value = other.value;
    }

    @Override
    public String toString() {
        return "a=extmap:" + id +
                " " +
                value +
                "\r\n";
    }
}
