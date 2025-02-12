package com.symphony.simpleserver.smb;

import com.fasterxml.jackson.annotation.JsonInclude;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.JsonNodeFactory;
import com.fasterxml.jackson.databind.node.ObjectNode;
import com.symphony.simpleserver.httpClient.HttpClient;
import com.symphony.simpleserver.httpClient.HttpClientFactory;
import com.symphony.simpleserver.smb.api.SmbEndpointDescription;
import java.io.IOException;
import java.util.List;
import org.apache.hc.core5.http.ParseException;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.stereotype.Component;

@Component public class SymphonyMediaBridge
{
    private static final Logger LOGGER = LoggerFactory.getLogger(SymphonyMediaBridge.class);
    private static final String BASE_URL = "http://127.0.0.1:8080/conferences/";
    private final HttpClient httpClient;
    private final ObjectMapper objectMapper;

    @Autowired public SymphonyMediaBridge(HttpClientFactory httpClientFactory)
    {
        this.httpClient = httpClientFactory.createClient();
        this.objectMapper = new ObjectMapper().setSerializationInclusion(JsonInclude.Include.NON_NULL);
    }

    public String allocateConference(List<String> videoCodecs) throws IOException, ParseException
    {
        final var requestBodyJson = JsonNodeFactory.instance.objectNode();
        ArrayNode arrayNode = JsonNodeFactory.instance.arrayNode();
        for (String item : videoCodecs)
        {
            arrayNode.add(item);
        }

        requestBodyJson.set("video-codecs", arrayNode);
        final var response = httpClient.post(BASE_URL, requestBodyJson);

        final var responseBodyJson = objectMapper.readTree(response.body);
        return responseBodyJson.get("id").asText();
    }

    public JsonNode allocateEndpoint(String conferenceId, String endpointId) throws IOException, ParseException
    {
        final var requestBodyJson = JsonNodeFactory.instance.objectNode();
        requestBodyJson.put("action", "allocate");

        final var bundleTransportJson = requestBodyJson.putObject("bundle-transport");
        bundleTransportJson.put("ice-controlling", true);
        bundleTransportJson.put("ice", true);
        bundleTransportJson.put("dtls", true);

        final var audioJson = requestBodyJson.putObject("audio");
        audioJson.put("relay-type", "ssrc-rewrite");

        final var videoJson = requestBodyJson.putObject("video");
        videoJson.put("relay-type", "ssrc-rewrite");

        requestBodyJson.putObject("data");

        LOGGER.info("Request\n{}", objectMapper.writerWithDefaultPrettyPrinter().writeValueAsString(requestBodyJson));

        final var url = BASE_URL + conferenceId + "/" + endpointId;
        final var response = httpClient.post(url, requestBodyJson);
        final var responseBodyJson = objectMapper.readTree(response.body);
        LOGGER.info("Response\n{}", objectMapper.writerWithDefaultPrettyPrinter().writeValueAsString(responseBodyJson));
        return responseBodyJson;
    }

    public void configureEndpoint(String conferenceId, String endpointId, SmbEndpointDescription endpointDescription)
        throws IOException, ParseException
    {

        final var requestBodyJson = (ObjectNode)objectMapper.valueToTree(endpointDescription);
        requestBodyJson.put("action", "configure");

        LOGGER.info("Request\n{}", objectMapper.writerWithDefaultPrettyPrinter().writeValueAsString(requestBodyJson));

        final var url = BASE_URL + conferenceId + "/" + endpointId;
        final var response = httpClient.post(url, requestBodyJson);
        LOGGER.info("Response {}", response.statusCode);
    }

    public void deleteEndpoint(String conferenceId, String endpointId) throws IOException, ParseException
    {
        final var url = BASE_URL + conferenceId + "/" + endpointId;
        final var response = httpClient.delete(url);
        LOGGER.info("Response {}", response.statusCode);
    }
}
