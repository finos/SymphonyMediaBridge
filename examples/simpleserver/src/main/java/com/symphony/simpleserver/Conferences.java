package com.symphony.simpleserver;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.SerializationFeature;
import com.symphony.simpleserver.sdp.ParserFailedException;
import com.symphony.simpleserver.sdp.SessionDescription;
import com.symphony.simpleserver.sdp.objects.MediaDescription;
import com.symphony.simpleserver.sdp.objects.Types;
import com.symphony.simpleserver.smb.SymphonyMediaBridge;
import com.symphony.simpleserver.smb.api.EndpointMediaStreams;
import com.symphony.simpleserver.smb.api.MediaStream;
import com.symphony.simpleserver.smb.api.Parser;
import com.symphony.simpleserver.smb.api.SmbEndpointDescription;
import java.io.IOException;
import java.util.*;
import java.util.concurrent.*;
import javax.annotation.PreDestroy;
import org.apache.hc.core5.http.ParseException;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.stereotype.Component;

@Component public class Conferences
{
    private static final Logger LOGGER = LoggerFactory.getLogger(Conferences.class);
    private static final long TIMEOUT_NS = 3L * 1000000000L;

    private static class Endpoint
    {
        public Endpoint(SmbEndpointDescription lastEndpointDescription)
        {
            this.lastEndpointDescription = lastEndpointDescription;
            this.isConfigured = false;
            this.lastSeenTimeStamp = System.nanoTime();
        }
        SmbEndpointDescription lastEndpointDescription;
        boolean isConfigured;
        long lastSeenTimeStamp;
    }

    private final ScheduledFuture<?> timerCheckerSchedule;
    private final ObjectMapper objectMapper;
    private final SymphonyMediaBridge symphonyMediaBridge;
    private final Parser parser;
    private final Map<String, Endpoint> endpoints;
    private final ConcurrentHashMap<String, LinkedBlockingQueue<String>> messageQueues;
    private final List<EndpointMediaStreams> endpointMediaStreams;
    private String conferenceId;

    @Autowired public Conferences(SymphonyMediaBridge symphonyMediaBridge, Parser parser)
    {
        final var timerCheckerExecutor = Executors.newScheduledThreadPool(1);
        this.timerCheckerSchedule = timerCheckerExecutor.scheduleAtFixedRate(this::checkTimers, 0, 3, TimeUnit.SECONDS);

        this.objectMapper = new ObjectMapper()
                                .configure(DeserializationFeature.FAIL_ON_UNKNOWN_PROPERTIES, false)
                                .configure(SerializationFeature.FAIL_ON_EMPTY_BEANS, false);
        this.parser = parser;
        this.symphonyMediaBridge = symphonyMediaBridge;
        this.endpoints = new HashMap<>();
        this.messageQueues = new ConcurrentHashMap<>();
        this.endpointMediaStreams = new ArrayList<>();
        this.conferenceId = null;
    }

    @SuppressWarnings("unused") @PreDestroy public void preDestroy() { timerCheckerSchedule.cancel(true); }

    public synchronized String join(List<String> videoCodecs)
        throws IOException, ParserFailedException, InterruptedException, ParseException
    {
        final var endpointId = UUID.randomUUID().toString();

        messageQueues.put(endpointId, new LinkedBlockingQueue<>());

        if (conferenceId == null)
        {
            conferenceId = symphonyMediaBridge.allocateConference(videoCodecs);
        }

        final var allocateEndpointResponse = symphonyMediaBridge.allocateEndpoint(conferenceId, endpointId);
        final var endpointDescription =
            objectMapper.treeToValue(allocateEndpointResponse, SmbEndpointDescription.class);
        final var offer = parser.makeSdpOffer(endpointDescription, endpointId, endpointMediaStreams);
        endpoints.put(endpointId, new Endpoint(endpointDescription));

        LOGGER.info("Join, endpointId {}, conferenceId {}, SDP {}", endpointId, conferenceId, offer.toString());
        sendMessage(endpointId, "offer", offer.toString());
        return endpointId;
    }

    public synchronized boolean message(String endpointId, String messageString)
        throws IOException, ParserFailedException, InterruptedException, ParseException
    {
        final var message = objectMapper.readValue(messageString, Message.class);
        LOGGER.info("Message type {} from {}", message.type, endpointId);

        if (message.type.equals("answer"))
        {
            return onAnswer(endpointId, message);
        }
        else if (message.type.equals("hangup"))
        {
            return onHangup(endpointId);
        }

        return false;
    }

    private boolean onAnswer(String endpointId, Message message)
        throws ParserFailedException, IOException, InterruptedException, ParseException
    {
        final var endpoint = endpoints.get(endpointId);
        if (endpoint.isConfigured)
        {
            return true;
        }

        final var answer = new SessionDescription(message.payload);
        final var endpointDescription = parser.makeEndpointDescription(answer);
        final var mediaStreams = new ArrayList<MediaStream>();

        for (var mediaDescription : answer.mediaDescriptions)
        {
            if (mediaDescription.direction != Types.Direction.SEND_RECV || mediaDescription.ssrcs.isEmpty())
            {
                continue;
            }

            final var mid = new MediaStream(mediaDescription.type);

            if (mediaDescription.type == MediaDescription.Type.AUDIO)
            {
                mid.ssrcs = new ArrayList<>(mediaDescription.ssrcs);
                mid.ssrcGroups = List.of();
            }
            else if (mediaDescription.type == MediaDescription.Type.VIDEO)
            {
                mid.type = MediaDescription.Type.VIDEO;
                mid.ssrcs = new ArrayList<>(mediaDescription.ssrcs);
                mid.ssrcGroups = new ArrayList<>(mediaDescription.ssrcGroups);
            }

            mediaStreams.add(mid);
        }

        final var stringBuilder = new StringBuilder();
        mediaStreams.forEach(mediaStream -> {
            stringBuilder.append(mediaStream.toString());
            stringBuilder.append(" ");
        });
        LOGGER.info("Media streams in answer {}", stringBuilder);

        endpointMediaStreams.add(new EndpointMediaStreams(endpointId, mediaStreams));
        LOGGER.info("Configure endpoint {} {}", endpointId, endpointDescription.toString());
        symphonyMediaBridge.configureEndpoint(conferenceId, endpointId, endpointDescription);
        endpoint.isConfigured = true;
        endpoints.put(endpointId, endpoint);

        return true;
    }

    private boolean onHangup(String endpointId)
        throws ParserFailedException, IOException, InterruptedException, ParseException
    {
        final var endpoint = endpoints.get(endpointId);
        if (endpoint == null)
        {
            return false;
        }
        endpoints.remove(endpointId);
        messageQueues.remove(endpointId);
        try
        {
            symphonyMediaBridge.deleteEndpoint(conferenceId, endpointId);
            return true;
        }
        catch (IOException | ParseException e)
        {
            LOGGER.error("Error deleting endpoint ", e);
            return false;
        }
    }

    public String poll(String endpointId) throws InterruptedException
    {
        final var queue = messageQueues.get(endpointId);
        final var endpoint = endpoints.get(endpointId);
        if (queue == null || endpoint == null)
        {
            return null;
        }

        endpoint.lastSeenTimeStamp = System.nanoTime();
        return queue.poll(1, TimeUnit.SECONDS);
    }

    private void sendMessage(String endpointId, String type, String payload)
        throws JsonProcessingException, InterruptedException
    {
        final var queue = messageQueues.get(endpointId);
        queue.put(objectMapper.writeValueAsString(new Message(type, payload)));
        LOGGER.info("Message type {} to {}", type, endpointId);
    }

    private synchronized void checkTimers()
    {
        final var endpointIdsToRemove = new HashSet<String>();

        for (var endpointsEntry : endpoints.entrySet())
        {
            final var endpoint = endpointsEntry.getValue();
            if (System.nanoTime() - endpoint.lastSeenTimeStamp > TIMEOUT_NS)
            {
                endpointIdsToRemove.add(endpointsEntry.getKey());
                LOGGER.info("Timeout {}", endpointsEntry.getKey());
            }
        }

        for (var endpointId : endpointIdsToRemove)
        {
            endpoints.remove(endpointId);
            messageQueues.remove(endpointId);
            try
            {
                symphonyMediaBridge.deleteEndpoint(conferenceId, endpointId);
            }
            catch (IOException | ParseException e)
            {
                LOGGER.error("Error deleting endpoint ", e);
            }
        }

        for (var endpointMediaStreamsEntry : endpointMediaStreams)
        {
            if (endpointIdsToRemove.contains(endpointMediaStreamsEntry.endpointId))
            {
                endpointMediaStreamsEntry.active = false;
            }
        }

        if (endpoints.isEmpty())
        {
            conferenceId = null;
            endpointMediaStreams.clear();
        }
    }
}
