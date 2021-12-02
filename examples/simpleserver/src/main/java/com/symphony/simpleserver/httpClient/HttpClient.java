package com.symphony.simpleserver.httpClient;

import com.fasterxml.jackson.databind.JsonNode;
import org.apache.http.client.methods.HttpPost;
import org.apache.http.entity.StringEntity;
import org.apache.http.impl.client.CloseableHttpClient;
import org.apache.http.util.EntityUtils;

import java.io.IOException;
import java.nio.charset.StandardCharsets;

public class HttpClient {
    public static class ResponsePair {
        public final int statusCode;
        public final String body;

        public ResponsePair(int statusCode, String body) {
            this.statusCode = statusCode;
            this.body = body;
        }
    }

    private final CloseableHttpClient httpClient;

    HttpClient(CloseableHttpClient httpClient) {
        this.httpClient = httpClient;
    }

    public ResponsePair post(String url, JsonNode data) throws IOException {
        final var request = new HttpPost(url);
        request.addHeader("Content-Type", "application/json");

        final var stringEntity = new StringEntity(data.toString(), StandardCharsets.UTF_8);
        request.setEntity(stringEntity);

        try {
            try (var httpResponse = httpClient.execute(request)) {
                final var statusCode = httpResponse.getStatusLine().getStatusCode();
                if (statusCode == 204) {
                    return new ResponsePair(statusCode, null);
                }

                final var httpEntity = httpResponse.getEntity();
                final var responseBody = EntityUtils.toString(httpEntity);
                EntityUtils.consumeQuietly(httpEntity);
                return new ResponsePair(statusCode, responseBody);
            }

        } finally {
            EntityUtils.consumeQuietly(stringEntity);
        }
    }
}
