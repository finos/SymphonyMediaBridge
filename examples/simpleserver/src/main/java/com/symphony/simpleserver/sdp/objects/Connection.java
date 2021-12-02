package com.symphony.simpleserver.sdp.objects;

public class Connection {
    public Types.Net netType;
    public Types.Address addressType;
    public String address;

    public Connection(Types.Net netType, Types.Address addressType, String address) {
        this.netType = netType;
        this.addressType = addressType;
        this.address = address;
    }

    public Connection(Connection other) {
        this.netType = other.netType;
        this.addressType = other.addressType;
        this.address = other.address;
    }

    @Override
    public String toString() {
        return "c=" +
                netType.toString() +
                " " +
                addressType.toString() +
                " " +
                address +
                "\r\n";
    }
}
