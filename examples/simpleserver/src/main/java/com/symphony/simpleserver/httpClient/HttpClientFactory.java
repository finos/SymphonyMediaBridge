package com.symphony.simpleserver.httpClient;

import org.apache.hc.client5.http.impl.classic.HttpClients;
import org.apache.hc.client5.http.impl.io.PoolingHttpClientConnectionManager;
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
        final var httpClientBuilder = HttpClients.custom();
        httpClientBuilder.setConnectionManager(connectionManager);
        httpClientBuilder.setConnectionManagerShared(true);
        return new HttpClient(httpClientBuilder.build());
    }
}
