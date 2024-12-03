package com.symphony.simpleserver.httpClient;

import com.fasterxml.jackson.databind.JsonNode;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import org.apache.hc.client5.http.classic.methods.HttpDelete;
import org.apache.hc.client5.http.classic.methods.HttpPost;
import org.apache.hc.client5.http.impl.classic.CloseableHttpClient;
import org.apache.hc.core5.http.ParseException;
import org.apache.hc.core5.http.io.entity.EntityUtils;
import org.apache.hc.core5.http.io.entity.StringEntity;

public class HttpClient
{
    public static class ResponsePair
    {
        public final int statusCode;
        public final String body;

        public ResponsePair(int statusCode, String body)
        {
            this.statusCode = statusCode;
            this.body = body;
        }
    }

    private final CloseableHttpClient httpClient;

    HttpClient(CloseableHttpClient httpClient) { this.httpClient = httpClient; }

    public ResponsePair post(String url, JsonNode data) throws IOException, ParseException
    {
        final var request = new HttpPost(url);
        request.addHeader("Content-Type", "application/json");

        final var stringEntity = new StringEntity(data.toString(), StandardCharsets.UTF_8);
        request.setEntity(stringEntity);

        try
        {
            try (var httpResponse = httpClient.execute(request))
            {
                final var statusCode = httpResponse.getCode();
                if (statusCode == 204)
                {
                    return new ResponsePair(statusCode, null);
                }

                final var httpEntity = httpResponse.getEntity();
                final var responseBody = EntityUtils.toString(httpEntity);
                EntityUtils.consumeQuietly(httpEntity);
                return new ResponsePair(statusCode, responseBody);
            }
        }
        finally
        {
            EntityUtils.consumeQuietly(stringEntity);
        }
    }

    public ResponsePair delete(String url)throws IOException, ParseException
    {
        final var request = new HttpDelete(url);

        try (var httpResponse = httpClient.execute(request))
        {
            final var statusCode = httpResponse.getCode();
            if (statusCode == 204)
            {
                return new ResponsePair(statusCode, null);
            }

            final var httpEntity = httpResponse.getEntity();
            EntityUtils.consumeQuietly(httpEntity);
            return new ResponsePair(statusCode, null);
        }
        finally
        {
        }
    }
}
