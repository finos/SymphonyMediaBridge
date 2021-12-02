package com.symphony.simpleserver;

import com.fasterxml.jackson.annotation.JsonTypeName;

@JsonTypeName("Message")
public class Message {
    @SuppressWarnings("unused")
    public Message() {
        this.type = null;
        this.payload = null;
    }

    public Message(String type, String payload) {
        this.type = type;
        this.payload = payload;
    }

    public String type;
    public String payload;
}
