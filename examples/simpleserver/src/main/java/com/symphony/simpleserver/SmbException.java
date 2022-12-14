package com.symphony.simpleserver;

public class SmbException extends Exception {

    private final String smbErrorMessage;

    public SmbException(String message) {
        super(message);
        this.smbErrorMessage = null;
    }

    public SmbException(String message, String smbErrorMessage) {
        super(message);
        this.smbErrorMessage = smbErrorMessage;
    }

    public String getSmbErrorMessage() {
        return this.smbErrorMessage;
    }
}
