package com.symphony.simpleserver.sdp.objects;

import com.symphony.simpleserver.sdp.ParserFailedException;

public class Types {
    public enum Net {
        IN;

        public static Net fromString(String value) throws ParserFailedException {
            switch (value) {
                case "IN":
                    return IN;
                default:
                    throw new ParserFailedException(value);
            }
        }
    }

    public enum Address {
        IP4, IP6;

        public static Address fromString(String value) throws ParserFailedException {
            switch (value) {
                case "IP4":
                    return IP4;
                case "IP6":
                    return IP6;
                default:
                    throw new ParserFailedException(value);
            }
        }
    }

    public enum Direction {
        SEND_RECV, SEND_ONLY, RECV_ONLY, INACTIVE;

        public static Direction fromString(String value) throws ParserFailedException {
            switch (value) {
                case "sendrecv":
                    return SEND_RECV;
                case "sendonly":
                    return SEND_ONLY;
                case "recvonly":
                    return RECV_ONLY;
                case "inactive":
                    return INACTIVE;
                default:
                    throw new ParserFailedException(value);
            }
        }

        @Override
        public String toString() {
            switch (this) {
                case SEND_RECV:
                    return "sendrecv";
                case SEND_ONLY:
                    return "sendonly";
                case RECV_ONLY:
                    return "recvonly";
                case INACTIVE:
                    return "inactive";
                default:
                    return null;
            }
        }

        public Direction getInverted() {
            switch (this) {
                case SEND_ONLY:
                    return RECV_ONLY;
                case RECV_ONLY:
                    return SEND_ONLY;
                default:
                    return this;
            }
        }

        public boolean canRecv() {
            return this == SEND_RECV || this == RECV_ONLY;
        }

        public boolean canSend() {
            return this == SEND_RECV || this == SEND_ONLY;
        }
    }

    public enum Setup {
        ACTIVE, PASSIVE, ACTPASS;

        public static Setup fromString(String value) throws ParserFailedException {
            switch (value) {
                case "active":
                    return ACTIVE;
                case "passive":
                    return PASSIVE;
                case "actpass":
                    return ACTPASS;
                default:
                    throw new ParserFailedException();
            }
        }

        @Override
        public String toString() {
            switch (this) {
                case ACTIVE:
                    return "active";
                case PASSIVE:
                    return "passive";
                case ACTPASS:
                    return "actpass";
                default:
                    return null;
            }
        }
    }

}
