package com.symphony.simpleserver.sdp.objects;

import java.util.ArrayList;

public class Ice {
    public String ufrag;
    public String pwd;
    public ArrayList<String> options;

    public Ice() {
        this.ufrag = null;
        this.pwd = null;
        this.options = new ArrayList<>();
    }

    public Ice(Ice other) {
        this.ufrag = other.ufrag;
        this.pwd = other.pwd;
        this.options = new ArrayList<>();
        this.options.addAll(other.options);
    }

    @Override
    public String toString() {
        String result = "";

        if (ufrag != null) {
            result += "a=ice-ufrag:" + ufrag + "\r\n";
        }

        if (pwd != null) {
            result += "a=ice-pwd:" + pwd + "\r\n";
        }

        return result;
    }
}
