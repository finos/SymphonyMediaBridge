#include "legacyapi/Parser.h"
#include "nlohmann/json.hpp"
#include <gtest/gtest.h>

namespace
{

const std::string requestData =
    "{\n"
    "  \"channel-bundles\" : [ {\n"
    "    \"id\" : \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
    "    \"transport\" : {\n"
    "      \"candidates\" : [ {\n"
    "        \"generation\" : 0,\n"
    "        \"component\" : 1,\n"
    "        \"protocol\" : \"udp\",\n"
    "        \"port\" : 10000,\n"
    "        \"ip\" : \"192.168.1.1\",\n"
    "        \"foundation\" : \"3\",\n"
    "        \"priority\" : 2130706431,\n"
    "        \"type\" : \"host\",\n"
    "        \"network\" : 0\n"
    "      }, {\n"
    "        \"generation\" : 0,\n"
    "        \"rel-port\" : 10000,\n"
    "        \"component\" : 1,\n"
    "        \"protocol\" : \"udp\",\n"
    "        \"port\" : 10000,\n"
    "        \"ip\" : \"18.216.242.52\",\n"
    "        \"foundation\" : \"4\",\n"
    "        \"rel-addr\" : \"192.168.1.1\",\n"
    "        \"priority\" : 1677724415,\n"
    "        \"type\" : \"srflx\",\n"
    "        \"network\" : 0\n"
    "      } ],\n"
    "      \"xmlns\" : \"urn:xmpp:jingle:transports:ice-udp:1\",\n"
    "      \"ufrag\" : \"alhik1c0gemu8k\",\n"
    "      \"rtcp-mux\" : true,\n"
    "      \"pwd\" : \"3c5lo26fcj00slrgmstld6lca3\",\n"
    "      \"fingerprints\" : [ {\n"
    "        \"fingerprint\" : \"5B:83:9C:D0:90:6A:BA:72:37:DD:94:45:22:6C:47:0F:73:AD:17:7C\",\n"
    "        \"setup\" : \"actpass\",\n"
    "        \"hash\" : \"sha-1\"\n"
    "      } ]\n"
    "    }\n"
    "  } ],\n"
    "  \"contents\" : [ {\n"
    "    \"channels\" : [ {\n"
    "      \"endpoint\" : \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
    "      \"channel-bundle-id\" : \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
    "      \"sources\" : [ 3437576905 ],\n"
    "      \"rtp-level-relay-type\" : \"translator\",\n"
    "      \"expire\" : 10,\n"
    "      \"initiator\" : true,\n"
    "      \"id\" : \"af5eb4dbefb9673a\",\n"
    "      \"direction\" : \"sendrecv\",\n"
    "      \"payload-types\": [\n"
    "        {\n"
    "          \"id\": 111,\n"
    "          \"parameters\": {\n"
    "            \"minptime\": \"10\",\n"
    "            \"useinbandfec\": \"1\"\n"
    "          },\n"
    "          \"rtcp-fbs\": [],\n"
    "          \"name\": \"opus\",\n"
    "          \"clockrate\": \"48000\",\n"
    "          \"channels\": \"2\"\n"
    "        },\n"
    "        {\n"
    "          \"id\": 0,\n"
    "          \"parameters\": {},\n"
    "          \"rtcp-fbs\": [],\n"
    "          \"name\": \"PCMU\",\n"
    "          \"clockrate\": \"8000\"\n"
    "        },\n"
    "        {\n"
    "          \"id\": 8,\n"
    "          \"parameters\": {},\n"
    "          \"rtcp-fbs\": [], \n"
    "          \"name\": \"PCMA\",\n"
    "          \"clockrate\": \"8000\"\n"
    "        },\n"
    "        {\n"
    "          \"id\": 126,\n"
    "          \"parameters\": {},\n"
    "          \"rtcp-fbs\": [],\n"
    "          \"name\": \"telephone-event\",\n"
    "          \"clockrate\": \"8000\"\n"
    "        }\n"
    "      ],\n"
    "      \"rtp-hdrexts\": [\n"
    "        {\n"
    "          \"id\": 1,\n"
    "          \"uri\": \"urn:ietf:params:rtp-hdrext:ssrc-audio-level\"\n"
    "        },\n"
    "        {\n"
    "          \"id\": 3,\n"
    "          \"uri\": \"http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\"\n"
    "        }\n"
    "      ]"
    "    } ],\n"
    "    \"name\" : \"audio\"\n"
    "  }, {\n"
    "    \"channels\" : [ {\n"
    "      \"endpoint\" : \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
    "      \"channel-bundle-id\" : \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
    "      \"rtp-level-relay-type\" : \"translator\",\n"
    "      \"expire\" : 10,\n"
    "      \"initiator\" : true,\n"
    "      \"id\" : \"8f7a4e13de94732\",\n"
    "      \"direction\" : \"sendonly\",\n"
    "      \"last-n\" : -1,\n"
    "      \"payload-types\": [\n"
    "        {\n"
    "          \"id\": 100,\n"
    "          \"parameters\": {},\n"
    "          \"rtcp-fbs\": [\n"
    "            {\n"
    "              \"type\": \"goog-remb\"\n"
    "            },\n"
    "            {\n"
    "              \"type\": \"ccm\",\n"
    "              \"subtype\": \"fir\"\n"
    "            },\n"
    "            {\n"
    "              \"type\": \"nack\"\n"
    "            },\n"
    "            {\n"
    "              \"type\": \"nack\",\n"
    "              \"subtype\": \"pli\"\n"
    "            }\n"
    "          ],\n"
    "          \"name\": \"VP8\",\n"
    "          \"clockrate\": \"90000\"\n"
    "        },\n"
    "        {\n"
    "          \"id\": 96,\n"
    "          \"parameters\": {\n"
    "            \"apt\": \"100\"\n"
    "          },\n"
    "          \"rtcp-fbs\": [],\n"
    "          \"name\": \"rtx\",\n"
    "          \"clockrate\": \"90000\"\n"
    "        }\n"
    "      ],\n"
    "      \"rtp-hdrexts\": [\n"
    "        {\n"
    "          \"id\": 3,\n"
    "          \"uri\": \"http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\"\n"
    "        }\n"
    "      ],"
    "      \"sources\" : [ 2567509219, 261784215, 2662387555, 4101452676, 2322531777, 2640664518,\n"
    "      3574920906, 3574920906 ],\n"
    "      \"ssrc-attributes\":[{\"content\" : \"slides\", \"sources\":[\"3574920905\"]}],\n"
    "      \"ssrc-groups\" : [ {\n"
    "        \"sources\" : [ \"2567509219\", \"261784215\" ],\n"
    "        \"semantics\" : \"FID\"\n"
    "      }, {\n"
    "        \"sources\" : [ \"2662387555\", \"4101452676\" ],\n"
    "        \"semantics\" : \"FID\"\n"
    "      }, {\n"
    "        \"sources\" : [ \"2322531777\", \"2640664518\" ],\n"
    "        \"semantics\" : \"FID\"\n"
    "      }, {\n"
    "        \"sources\" : [ \"3574920906\", \"3574920906\" ],\n"
    "        \"semantics\" : \"FID\"\n"
    "      }, {\n"
    "        \"sources\" : [ \"2567509219\", \"2662387555\", \"2322531777\" ],\n"
    "        \"semantics\" : \"SIM\"\n"
    "      } ],"
    "      \"ssrc-whitelist\": [\"2567509219\", \"261784215\"]\n"
    "    } ],\n"
    "    \"name\" : \"video\"\n"
    "  }, {\n"
    "    \"sctpconnections\" : [ {\n"
    "      \"endpoint\" : \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
    "      \"channel-bundle-id\" : \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
    "      \"port\" : \"5000\",\n"
    "      \"expire\" : 10,\n"
    "      \"initiator\" : true,\n"
    "      \"id\" : \"e76ca561340fed47\"\n"
    "    } ],\n"
    "    \"name\" : \"data\"\n"
    "  } ],\n"
    "  \"id\" : \"b554cdfd47c6133d\"\n"
    "}\n";

}

class ParserTest : public ::testing::Test
{
    void SetUp() override
    {
        const auto requestJson = nlohmann::json::parse(requestData);
        _conference = legacyapi::Parser::parse(requestJson);
    }

    void TearDown() override {}

protected:
    legacyapi::Conference _conference;
};

TEST_F(ParserTest, conferenceId)
{
    EXPECT_EQ("b554cdfd47c6133d", _conference._id);
}

TEST_F(ParserTest, channelBundle)
{
    EXPECT_EQ(1, _conference._channelBundles.size());
    const auto& channelBundle = _conference._channelBundles[0];

    EXPECT_EQ("dac7f369-bc78-4151-8d63-376ff265aaa4", channelBundle._id);
    EXPECT_EQ("3c5lo26fcj00slrgmstld6lca3", channelBundle._transport._pwd.get());
    EXPECT_EQ("alhik1c0gemu8k", channelBundle._transport._ufrag.get());
    EXPECT_TRUE(channelBundle._transport._rtcpMux);
    EXPECT_EQ("urn:xmpp:jingle:transports:ice-udp:1", channelBundle._transport._xmlns.get());

    EXPECT_EQ(1, channelBundle._transport._fingerprints.size());
    EXPECT_EQ("sha-1", channelBundle._transport._fingerprints[0]._hash);
    EXPECT_EQ("actpass", channelBundle._transport._fingerprints[0]._setup);
    EXPECT_EQ("5B:83:9C:D0:90:6A:BA:72:37:DD:94:45:22:6C:47:0F:73:AD:17:7C",
        channelBundle._transport._fingerprints[0]._fingerprint);

    EXPECT_EQ(2, channelBundle._transport._candidates.size());

    EXPECT_EQ(0, channelBundle._transport._candidates[0]._generation);
    EXPECT_EQ(1, channelBundle._transport._candidates[0]._component);
    EXPECT_EQ("udp", channelBundle._transport._candidates[0]._protocol);
    EXPECT_EQ(10000, channelBundle._transport._candidates[0]._port);
    EXPECT_EQ("192.168.1.1", channelBundle._transport._candidates[0]._ip);
    EXPECT_FALSE(channelBundle._transport._candidates[0]._relPort.isSet());
    EXPECT_FALSE(channelBundle._transport._candidates[0]._relAddr.isSet());
    EXPECT_EQ("3", channelBundle._transport._candidates[0]._foundation);
    EXPECT_EQ(2130706431, channelBundle._transport._candidates[0]._priority);
    EXPECT_EQ("host", channelBundle._transport._candidates[0]._type);
    EXPECT_EQ(0, channelBundle._transport._candidates[0]._network);

    EXPECT_EQ(0, channelBundle._transport._candidates[1]._generation);
    EXPECT_EQ(1, channelBundle._transport._candidates[1]._component);
    EXPECT_EQ("udp", channelBundle._transport._candidates[1]._protocol);
    EXPECT_EQ(10000, channelBundle._transport._candidates[1]._port);
    EXPECT_EQ("18.216.242.52", channelBundle._transport._candidates[1]._ip);
    EXPECT_EQ(10000, channelBundle._transport._candidates[1]._relPort.get());
    EXPECT_EQ("192.168.1.1", channelBundle._transport._candidates[1]._relAddr.get());
    EXPECT_EQ("4", channelBundle._transport._candidates[1]._foundation);
    EXPECT_EQ(1677724415, channelBundle._transport._candidates[1]._priority);
    EXPECT_EQ("srflx", channelBundle._transport._candidates[1]._type);
    EXPECT_EQ(0, channelBundle._transport._candidates[1]._network);
}

TEST_F(ParserTest, contents)
{
    EXPECT_EQ(3, _conference._contents.size());

    {
        const auto& audioContent = _conference._contents[0];
        EXPECT_TRUE(audioContent._sctpConnections.empty());
        EXPECT_EQ("audio", audioContent._name);
        EXPECT_EQ(1, audioContent._channels.size());
        const auto& audioChannel = audioContent._channels[0];
        EXPECT_EQ("dac7f369-bc78-4151-8d63-376ff265aaa4", audioChannel._endpoint.get());
        EXPECT_EQ("dac7f369-bc78-4151-8d63-376ff265aaa4", audioChannel._channelBundleId.get());
        EXPECT_EQ(1, audioChannel._sources.size());
        EXPECT_EQ(3437576905, audioChannel._sources[0]);
        EXPECT_EQ("translator", audioChannel._rtpLevelRelayType.get());
        EXPECT_EQ(10, audioChannel._expire.get());
        EXPECT_TRUE(audioChannel._initiator.get());
        EXPECT_EQ("af5eb4dbefb9673a", audioChannel._id.get());
        EXPECT_EQ("sendrecv", audioChannel._direction.get());
        EXPECT_FALSE(audioChannel._ssrcWhitelist.isSet());
    }

    {
        const auto& videoContent = _conference._contents[1];
        EXPECT_TRUE(videoContent._sctpConnections.empty());
        EXPECT_EQ("video", videoContent._name);
        EXPECT_EQ(1, videoContent._channels.size());
        const auto& videoChannel = videoContent._channels[0];
        EXPECT_EQ("dac7f369-bc78-4151-8d63-376ff265aaa4", videoChannel._endpoint.get());
        EXPECT_EQ("dac7f369-bc78-4151-8d63-376ff265aaa4", videoChannel._channelBundleId.get());
        EXPECT_EQ(8, videoChannel._sources.size());
        EXPECT_EQ(2567509219, videoChannel._sources[0]);
        EXPECT_EQ("translator", videoChannel._rtpLevelRelayType.get());
        EXPECT_EQ(10, videoChannel._expire.get());
        EXPECT_TRUE(videoChannel._initiator.get());
        EXPECT_EQ("8f7a4e13de94732", videoChannel._id.get());
        EXPECT_EQ("sendonly", videoChannel._direction.get());
        EXPECT_EQ(-1, videoChannel._lastN.get());
        EXPECT_TRUE(videoChannel._ssrcWhitelist.isSet());
        EXPECT_EQ(2, videoChannel._ssrcWhitelist.get().size());
        EXPECT_EQ(2567509219, videoChannel._ssrcWhitelist.get()[0]);
        EXPECT_EQ(261784215, videoChannel._ssrcWhitelist.get()[1]);

        EXPECT_EQ(1, videoChannel._ssrcAttributes.size());
        EXPECT_EQ("slides", videoChannel._ssrcAttributes[0]._content);
        EXPECT_EQ(1, videoChannel._ssrcAttributes[0]._sources.size());
        EXPECT_EQ(3574920905, videoChannel._ssrcAttributes[0]._sources[0]);
    }

    {
        const auto& dataContent = _conference._contents[2];
        EXPECT_EQ(1, dataContent._sctpConnections.size());
        EXPECT_EQ("data", dataContent._name);
        EXPECT_TRUE(dataContent._channels.empty());
        const auto& dataSctpConnection = dataContent._sctpConnections[0];
        EXPECT_EQ("dac7f369-bc78-4151-8d63-376ff265aaa4", dataSctpConnection._endpoint.get());
        EXPECT_EQ("dac7f369-bc78-4151-8d63-376ff265aaa4", dataSctpConnection._channelBundleId.get());
        EXPECT_EQ(10, dataSctpConnection._expire.get());
        EXPECT_TRUE(dataSctpConnection._initiator.get());
        EXPECT_EQ("e76ca561340fed47", dataSctpConnection._id.get());
        EXPECT_EQ("5000", dataSctpConnection._port.get());
    }
}

TEST_F(ParserTest, payloadTypes)
{
    {
        const auto& audioChannel = _conference._contents[0]._channels[0];
        EXPECT_EQ(4, audioChannel._payloadTypes.size());
        EXPECT_EQ(111, audioChannel._payloadTypes[0]._id);
        EXPECT_EQ("opus", audioChannel._payloadTypes[0]._name);
        EXPECT_EQ("48000", audioChannel._payloadTypes[0]._clockRate);
        EXPECT_EQ("2", audioChannel._payloadTypes[0]._channels.get());
        EXPECT_EQ("minptime", audioChannel._payloadTypes[0]._parameters[0].first);
        EXPECT_EQ("10", audioChannel._payloadTypes[0]._parameters[0].second);
        EXPECT_EQ("useinbandfec", audioChannel._payloadTypes[0]._parameters[1].first);
        EXPECT_EQ("1", audioChannel._payloadTypes[0]._parameters[1].second);
        EXPECT_EQ(2, audioChannel._rtpHeaderHdrExts.size());
        EXPECT_EQ(1, audioChannel._rtpHeaderHdrExts[0]._id);
        EXPECT_EQ("urn:ietf:params:rtp-hdrext:ssrc-audio-level", audioChannel._rtpHeaderHdrExts[0]._uri);
    }

    {
        const auto& videoChannel = _conference._contents[1]._channels[0];
        EXPECT_EQ(2, videoChannel._payloadTypes.size());
        EXPECT_EQ(4, videoChannel._payloadTypes[0]._rtcpFbs.size());
        EXPECT_EQ("goog-remb", videoChannel._payloadTypes[0]._rtcpFbs[0]._type);
        EXPECT_FALSE(videoChannel._payloadTypes[0]._rtcpFbs[0]._subtype.isSet());
        EXPECT_EQ("ccm", videoChannel._payloadTypes[0]._rtcpFbs[1]._type);
        EXPECT_EQ("fir", videoChannel._payloadTypes[0]._rtcpFbs[1]._subtype.get());
    }
}
