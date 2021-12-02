package com.symphony.simpleserver.sdp;

public class ParserFailedException extends Exception {
    public ParserFailedException(String message, Throwable throwable) {
        super(message, throwable);
    }

    public ParserFailedException(String message) {
        super(message);
    }

    public ParserFailedException() {

    }
}
