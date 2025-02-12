package com.symphony.simpleserver;

import com.fasterxml.jackson.annotation.JsonInclude;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.JsonNodeFactory;
import java.util.ArrayList;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.*;

/**
 * Main entry point to the service. Spring URL mappings for the client API endpoints (join, request and conferences) as
 * well as the long polling endpoint (poll).
 */
@RestController public final class ConferenceRequestHandler
{
    private static final Logger LOGGER = LoggerFactory.getLogger(ConferenceRequestHandler.class);

    private final Conferences conferences;

    @Autowired public ConferenceRequestHandler(Conferences conferences) { this.conferences = conferences; }

    @RequestMapping(value = "/conferences/endpoints",
        method = RequestMethod.POST,
        produces = MediaType.APPLICATION_JSON_VALUE)
    @CrossOrigin
    public ResponseEntity<String>
    handleJoin(@RequestBody String message)
    {
        try
        {
            var objectMapper = new ObjectMapper().setSerializationInclusion(JsonInclude.Include.NON_NULL);
            var jsonBody = objectMapper.readTree(message);

            var codecList = new ArrayList<String>();
            if (jsonBody.has("video-codecs"))
            {
                var codecs = (ArrayNode)jsonBody.get("video-codecs");

                for (JsonNode node : codecs)
                {
                    codecList.add(node.asText());
                }
            }
            final var endpointId = conferences.join(codecList);

            final var responseBodyJson = JsonNodeFactory.instance.objectNode();
            responseBodyJson.put("endpointId", endpointId);
            return new ResponseEntity<>(responseBodyJson.toString(), HttpStatus.OK);
        }
        catch (Exception e)
        {
            LOGGER.error("Unhandled error", e);
            return new ResponseEntity<>(HttpStatus.INTERNAL_SERVER_ERROR);
        }
    }

    /**
     * Handle any other requests besides join. Request types and message formats defined
     * by CommandFactory.createCommand().
     */
    @RequestMapping(value = "/conferences/endpoints/{endpointId}/actions", method = RequestMethod.POST)
    @CrossOrigin
    public ResponseEntity<Void> handleRequest(@PathVariable("endpointId") String endpointId,
        @RequestBody String message)
    {
        try
        {
            if (!conferences.message(endpointId, message))
            {
                return new ResponseEntity<>(HttpStatus.BAD_REQUEST);
            }

            return new ResponseEntity<>(HttpStatus.OK);
        }
        catch (Exception e)
        {
            LOGGER.error("Unhandled error", e);
            return new ResponseEntity<>(HttpStatus.INTERNAL_SERVER_ERROR);
        }
    }

    @RequestMapping(value = "/conferences/endpoints/{endpointId}/poll",
        method = RequestMethod.GET,
        produces = MediaType.APPLICATION_JSON_VALUE)
    @CrossOrigin
    public ResponseEntity<String>
    handlePoll(@PathVariable("endpointId") String endpointId)
    {
        try
        {
            final var result = conferences.poll(endpointId);
            if (result == null)
            {
                return new ResponseEntity<>(HttpStatus.NO_CONTENT);
            }
            return new ResponseEntity<>(result, HttpStatus.OK);
        }
        catch (Exception e)
        {
            LOGGER.error("Unhandled error", e);
            return new ResponseEntity<>(HttpStatus.INTERNAL_SERVER_ERROR);
        }
    }
}
