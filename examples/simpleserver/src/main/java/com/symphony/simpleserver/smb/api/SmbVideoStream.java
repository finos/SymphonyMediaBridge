package com.symphony.simpleserver.smb.api;

import java.util.List;

public class SmbVideoStream {
    public static class SmbVideoSource {
        public long main;
        public long feedback;
    }

    public List<SmbVideoSource> sources;
    public String id;
    public String content;
}
