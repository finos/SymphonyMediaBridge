package com.symphony.simpleserver.httpClient;

import org.apache.http.impl.client.HttpClientBuilder;
import org.apache.http.impl.client.HttpClients;
import org.apache.http.impl.conn.PoolingHttpClientConnectionManager;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.stereotype.Component;

@Component
public class HttpClientFactory {
    private final PoolingHttpClientConnectionManager connectionManager;

    @Autowired
    public HttpClientFactory() {
        this.connectionManager = new PoolingHttpClientConnectionManager();
    }

    public HttpClient createClient() {
        final HttpClientBuilder httpClientBuilder = HttpClients.custom();
        httpClientBuilder.setConnectionManager(connectionManager);
        httpClientBuilder.setConnectionManagerShared(true);
        return new HttpClient(httpClientBuilder.build());
    }
}
