package com.symphony.simpleserver.sdp.objects;

public class Origin {
    public String userName;
    public String sessionId;
    public long sessionVersion;
    public Types.Net netType;
    public Types.Address addressType;
    public String address;

    public Origin(
            String userName,
            String sessionId,
            long sessionVersion,
            Types.Net netType,
            Types.Address addressType,
            String address)
    {
        this.userName = userName;
        this.sessionId = sessionId;
        this.sessionVersion = sessionVersion;
        this.netType = netType;
        this.addressType = addressType;
        this.address = address;
    }

    @Override
    public String toString() {
        return "o=" +
                userName +
                " " +
                sessionId +
                " " + Long.toUnsignedString(sessionVersion) +
                " " +
                netType.toString() +
                " " +
                addressType.toString() +
                " " +
                address +
                "\r\n";
    }
}
