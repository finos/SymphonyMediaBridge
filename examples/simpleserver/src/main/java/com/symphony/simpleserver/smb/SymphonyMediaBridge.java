package com.symphony.simpleserver.smb;

import com.fasterxml.jackson.annotation.JsonInclude;
import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.JsonNodeFactory;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.symphony.simpleserver.SmbException;
import com.symphony.simpleserver.httpClient.HttpClient;
import com.symphony.simpleserver.httpClient.HttpClientFactory;
import com.symphony.simpleserver.httpClient.HttpClient.ResponsePair;
import com.symphony.simpleserver.smb.api.SmbEndpointDescription;
import org.apache.hc.core5.http.ParseException;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.stereotype.Component;
import org.springframework.util.StringUtils;

import java.io.IOException;

@Component
public class SymphonyMediaBridge {
    private static final Logger LOGGER = LoggerFactory.getLogger(SymphonyMediaBridge.class);
    //private static final String BASE_URL = "http://172.31.114.70:8080/conferences/";
    private static final String BASE_URL = "http://127.0.0.1:8080/conferences/";
    private final HttpClient httpClient;
    private final ObjectMapper objectMapper;

    @Autowired
    public SymphonyMediaBridge(HttpClientFactory httpClientFactory) {
        this.httpClient = httpClientFactory.createClient();
        this.objectMapper = new ObjectMapper().setSerializationInclusion(JsonInclude.Include.NON_NULL);
    }

    public String allocateConference() throws IOException, ParseException, SmbException {
        final var requestBodyJson = JsonNodeFactory.instance.objectNode();
        final var response = httpClient.post(BASE_URL, requestBodyJson);

        checkResponse(response);

        final var responseBodyJson = objectMapper.readTree(response.body);
        return responseBodyJson.get("id").asText();
    }

    public JsonNode allocateEndpoint(String conferenceId, String endpointId, boolean bundleTransport, boolean enableDtls, boolean enableIce) throws IOException, ParseException, SmbException {
        final var requestBodyJson = JsonNodeFactory.instance.objectNode();
        requestBodyJson.put("action", "allocate");

        if (bundleTransport) {
            final var bundleTransportJson = requestBodyJson.putObject("bundle-transport");
            bundleTransportJson.put("ice-controlling", true);
            bundleTransportJson.put("ice", true);
            bundleTransportJson.put("dtls", true);
        }

        final var audioJson = requestBodyJson.putObject("audio");
        audioJson.put("relay-type", "forwarder");
        if (!bundleTransport) {
            final var transport = audioJson.putObject("transport");
            transport.put("ice", enableIce);
            transport.put("dtls", enableDtls);
        }

        final var videoJson = requestBodyJson.putObject("video");
        videoJson.put("relay-type", "forwarder");
        if (!bundleTransport) {
            final var transport = videoJson.putObject("transport");
            transport.put("ice", enableIce);
            transport.put("dtls", enableDtls);
        }

        if (bundleTransport) {
            requestBodyJson.putObject("data");
        }

        LOGGER.info("Request\n{}", objectMapper.writerWithDefaultPrettyPrinter().writeValueAsString(requestBodyJson));

        final var url = BASE_URL + conferenceId + "/" + endpointId;
        final var response = httpClient.post(url, requestBodyJson);

        checkResponse(response);

        final var responseBodyJson = objectMapper.readTree(response.body);
        LOGGER.info("Response\n{}", objectMapper.writerWithDefaultPrettyPrinter().writeValueAsString(responseBodyJson));
        return responseBodyJson;
    }

    public void configureEndpoint(
            String conferenceId, String endpointId, SmbEndpointDescription endpointDescription) throws IOException, ParseException, SmbException {

        final var requestBodyJson = (ObjectNode) objectMapper.valueToTree(endpointDescription);
        requestBodyJson.put("action", "configure");

        LOGGER.info("Request\n{}", objectMapper.writerWithDefaultPrettyPrinter().writeValueAsString(requestBodyJson));

        final var url = BASE_URL + conferenceId + "/" + endpointId;
        final var response = httpClient.post(url, requestBodyJson);
        checkResponse(response);
        LOGGER.info("Response {}", response.statusCode);
    }

    public void deleteEndpoint(String conferenceId, String endpointId) throws IOException, ParseException {
        final var requestBodyJson = JsonNodeFactory.instance.objectNode();
        requestBodyJson.put("action", "expire");

        LOGGER.info("Request\n{}", objectMapper.writerWithDefaultPrettyPrinter().writeValueAsString(requestBodyJson));

        final var url = BASE_URL + conferenceId + "/" + endpointId;
        final var response = httpClient.post(url, requestBodyJson);
        LOGGER.info("Response {}", response.statusCode);
    }

    private void checkResponse(ResponsePair response) throws SmbException {
        final var statusFirstDigit = response.statusCode / 100;
        if (statusFirstDigit > 2) {
            try {
                if (!StringUtils.hasText(response.body)) {
                    throw new SmbException("smb request has failed");
                }

                final var jsonNode = objectMapper.readTree(response.body);
                final var messageNode = jsonNode.path("message");
                if (messageNode.isTextual()) {
                    throw new SmbException("smb request has failed", messageNode.asText());
                }

            } catch (JsonProcessingException e) {
                // Do nothing
            }

            throw new SmbException("smb request has failed");
        }
    }
}
