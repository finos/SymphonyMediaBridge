package com.symphony.simpleserver.sdp.objects;

public class Rtcp {
    public int port;
    public Connection connection;

    public Rtcp(int port, Connection connection) {
        this.port = port;
        this.connection = connection;
    }

    public Rtcp(Rtcp other) {
        this.port = other.port;
        this.connection = other.connection == null ? null : new Connection(other.connection);
    }

    @Override
    public String toString() {
        String result = "a=rtcp:" + port;

        if (connection != null) {
            return result +
                    " " +
                    connection.netType.toString() +
                    " " +
                    connection.addressType.toString() +
                    " " +
                    connection.address +
                    "\r\n";
        }

        return result;
    }
}
