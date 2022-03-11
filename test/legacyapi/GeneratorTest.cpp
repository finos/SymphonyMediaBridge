#include "legacyapi/Generator.h"
#include "legacyapi/Conference.h"
#include "nlohmann/json.hpp"
#include <gtest/gtest.h>

class GeneratorTest : public ::testing::Test
{
    void SetUp() override {}

    void TearDown() override {}
};

TEST_F(GeneratorTest, conferenceId)
{
    legacyapi::Conference conference;
    conference._id = "b554cdfd47c6133d";

    const auto conferenceJson = legacyapi::Generator::generate(conference);
    EXPECT_EQ(conference._id, conferenceJson["id"].get<std::string>());
}

TEST_F(GeneratorTest, channelBundles)
{
    legacyapi::Conference conference;

    legacyapi::ChannelBundle channelBundle;
    channelBundle._id = "dac7f369-bc78-4151-8d63-376ff265aaa4";
    channelBundle._transport._xmlns.set("urn:xmpp:jingle:transports:ice-udp:1");
    channelBundle._transport._ufrag.set("alhik1c0gemu8k");
    channelBundle._transport._pwd.set("3c5lo26fcj00slrgmstld6lca3");
    channelBundle._transport._rtcpMux = true;
    channelBundle._transport._fingerprints.emplace_back(
        legacyapi::Fingerprint{"5B:83:9C:D0:90:6A:BA:72:37:DD:94:45:22:6C:47:0F:73:AD:17:7C", "actpass", "sha-1"});

    {
        legacyapi::Candidate candidate;
        candidate._generation = 0;
        candidate._component = 0;
        candidate._protocol = "udp";
        candidate._port = 10000;
        candidate._ip = "192.168.1.1";
        candidate._foundation = "3";
        candidate._priority = 2130706431;
        candidate._type = "host";
        candidate._network = 0;
        channelBundle._transport._candidates.push_back(candidate);
    }
    {
        legacyapi::Candidate candidate;
        candidate._generation = 0;
        candidate._component = 1;
        candidate._protocol = "udp";
        candidate._port = 10000;
        candidate._ip = "18.216.242.52";
        candidate._relPort.set(10000);
        candidate._relAddr.set("192.168.1.1");
        candidate._foundation = "4";
        candidate._priority = 1677724415;
        candidate._type = "srflx";
        candidate._network = 0;
        channelBundle._transport._candidates.push_back(candidate);
    }

    conference._channelBundles.push_back(channelBundle);

    const auto conferenceJson = legacyapi::Generator::generate(conference);

    const std::string expectedResult =
        "{\n"
        "    \"channel-bundles\": [\n"
        "        {\n"
        "            \"id\": \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
        "            \"transport\": {\n"
        "                \"candidates\": [\n"
        "                    {\n"
        "                        \"component\": 0,\n"
        "                        \"foundation\": \"3\",\n"
        "                        \"generation\": 0,\n"
        "                        \"ip\": \"192.168.1.1\",\n"
        "                        \"network\": 0,\n"
        "                        \"port\": 10000,\n"
        "                        \"priority\": 2130706431,\n"
        "                        \"protocol\": \"udp\",\n"
        "                        \"type\": \"host\"\n"
        "                    },\n"
        "                    {\n"
        "                        \"component\": 1,\n"
        "                        \"foundation\": \"4\",\n"
        "                        \"generation\": 0,\n"
        "                        \"ip\": \"18.216.242.52\",\n"
        "                        \"network\": 0,\n"
        "                        \"port\": 10000,\n"
        "                        \"priority\": 1677724415,\n"
        "                        \"protocol\": \"udp\",\n"
        "                        \"rel-addr\": \"192.168.1.1\",\n"
        "                        \"rel-port\": 10000,\n"
        "                        \"type\": \"srflx\"\n"
        "                    }\n"
        "                ],\n"
        "                \"fingerprints\": [\n"
        "                    {\n"
        "                        \"fingerprint\": \"5B:83:9C:D0:90:6A:BA:72:37:DD:94:45:22:6C:47:0F:73:AD:17:7C\",\n"
        "                        \"hash\": \"sha-1\",\n"
        "                        \"setup\": \"actpass\"\n"
        "                    }\n"
        "                ],\n"
        "                \"pwd\": \"3c5lo26fcj00slrgmstld6lca3\",\n"
        "                \"rtcp-mux\": true,\n"
        "                \"ufrag\": \"alhik1c0gemu8k\",\n"
        "                \"xmlns\": \"urn:xmpp:jingle:transports:ice-udp:1\"\n"
        "            }\n"
        "        }\n"
        "    ],\n"
        "    \"id\": \"\"\n"
        "}";

    EXPECT_EQ(expectedResult, conferenceJson.dump(4));
}

TEST_F(GeneratorTest, contents)
{
    legacyapi::Conference conference;

    {
        legacyapi::Channel channel;
        channel._endpoint.set("dac7f369-bc78-4151-8d63-376ff265aaa4");
        channel._channelBundleId.set("dac7f369-bc78-4151-8d63-376ff265aaa4");
        channel._sources.push_back(3437576905);
        channel._rtpLevelRelayType.set("translator");
        channel._expire.set(10);
        channel._initiator.set(true);
        channel._id.set("af5eb4dbefb9673a");
        channel._direction.set("sendrecv");

        legacyapi::Content content;
        content._channels.push_back(channel);
        content._name = "audio";
        conference._contents.push_back(content);
    }

    {
        legacyapi::Channel channel;
        channel._endpoint.set("dac7f369-bc78-4151-8d63-376ff265aaa4");
        channel._channelBundleId.set("dac7f369-bc78-4151-8d63-376ff265aaa4");
        channel._sources.push_back(176003640);
        channel._rtpLevelRelayType.set("translator");
        channel._expire.set(10);
        channel._initiator.set(true);
        channel._id.set("8f7a4e13de94732");
        channel._direction.set("sendonly");
        channel._lastN.set(-1);

        legacyapi::Content content;
        content._channels.push_back(channel);
        content._name = "video";
        conference._contents.push_back(content);
    }

    {
        legacyapi::SctpConnection sctpConnection;
        sctpConnection._endpoint.set("dac7f369-bc78-4151-8d63-376ff265aaa4");
        sctpConnection._channelBundleId.set("dac7f369-bc78-4151-8d63-376ff265aaa4");
        sctpConnection._expire.set(10);
        sctpConnection._initiator.set(true);
        sctpConnection._id.set("e76ca561340fed47");
        sctpConnection._port.set("5000");

        legacyapi::Content content;
        content._sctpConnections.push_back(sctpConnection);
        content._name = "data";
        conference._contents.push_back(content);
    }

    const auto conferenceJson = legacyapi::Generator::generate(conference);

    const std::string expectedResult =
        "{\n"
        "    \"contents\": [\n"
        "        {\n"
        "            \"channels\": [\n"
        "                {\n"
        "                    \"channel-bundle-id\": \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
        "                    \"direction\": \"sendrecv\",\n"
        "                    \"endpoint\": \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
        "                    \"expire\": 10,\n"
        "                    \"id\": \"af5eb4dbefb9673a\",\n"
        "                    \"initiator\": true,\n"
        "                    \"rtp-level-relay-type\": \"translator\",\n"
        "                    \"sources\": [\n"
        "                        3437576905\n"
        "                    ]\n"
        "                }\n"
        "            ],\n"
        "            \"name\": \"audio\"\n"
        "        },\n"
        "        {\n"
        "            \"channels\": [\n"
        "                {\n"
        "                    \"channel-bundle-id\": \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
        "                    \"direction\": \"sendonly\",\n"
        "                    \"endpoint\": \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
        "                    \"expire\": 10,\n"
        "                    \"id\": \"8f7a4e13de94732\",\n"
        "                    \"initiator\": true,\n"
        "                    \"last-n\": -1,\n"
        "                    \"rtp-level-relay-type\": \"translator\",\n"
        "                    \"sources\": [\n"
        "                        176003640\n"
        "                    ]\n"
        "                }\n"
        "            ],\n"
        "            \"name\": \"video\"\n"
        "        },\n"
        "        {\n"
        "            \"name\": \"data\",\n"
        "            \"sctpconnections\": [\n"
        "                {\n"
        "                    \"channel-bundle-id\": \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
        "                    \"endpoint\": \"dac7f369-bc78-4151-8d63-376ff265aaa4\",\n"
        "                    \"expire\": 10,\n"
        "                    \"id\": \"e76ca561340fed47\",\n"
        "                    \"initiator\": true,\n"
        "                    \"port\": \"5000\"\n"
        "                }\n"
        "            ]\n"
        "        }\n"
        "    ],\n"
        "    \"id\": \"\"\n"
        "}";

    printf("%s\n", conferenceJson.dump(4).c_str());
    EXPECT_EQ(expectedResult, conferenceJson.dump(4));
}

TEST_F(GeneratorTest, payloadTypes)
{
    legacyapi::Conference conference;

    {
        legacyapi::Channel channel;
        legacyapi::PayloadType payloadType;
        payloadType._id = 111;
        payloadType._name = "opus";
        payloadType._clockRate = "48000";
        payloadType._channels.set("2");
        payloadType._parameters.push_back(std::make_pair("minptime", "10"));
        channel._payloadTypes.push_back(payloadType);

        legacyapi::Content content;
        content._channels.push_back(channel);
        content._name = "audio";
        conference._contents.push_back(content);
    }

    {
        legacyapi::Channel channel;
        legacyapi::PayloadType payloadType;
        payloadType._id = 100;
        payloadType._name = "vp8";
        payloadType._clockRate = "90000";
        legacyapi::PayloadType::RtcpFb rtcpFb;
        rtcpFb._type = "ccm";
        rtcpFb._subtype.set("fir");
        payloadType._rtcpFbs.push_back(rtcpFb);
        channel._payloadTypes.push_back(payloadType);

        legacyapi::Content content;
        content._channels.push_back(channel);
        content._name = "video";
        conference._contents.push_back(content);
    }

    const auto conferenceJson = legacyapi::Generator::generate(conference);

    const std::string expectedResult = "{\n"
                                       "    \"contents\": [\n"
                                       "        {\n"
                                       "            \"channels\": [\n"
                                       "                {\n"
                                       "                    \"payload-types\": [\n"
                                       "                        {\n"
                                       "                            \"channels\": \"2\",\n"
                                       "                            \"clockrate\": \"48000\",\n"
                                       "                            \"id\": 111,\n"
                                       "                            \"name\": \"opus\",\n"
                                       "                            \"parameters\": {\n"
                                       "                                \"minptime\": \"10\"\n"
                                       "                            },\n"
                                       "                            \"rtcp-fbs\": []\n"
                                       "                        }\n"
                                       "                    ]\n"
                                       "                }\n"
                                       "            ],\n"
                                       "            \"name\": \"audio\"\n"
                                       "        },\n"
                                       "        {\n"
                                       "            \"channels\": [\n"
                                       "                {\n"
                                       "                    \"payload-types\": [\n"
                                       "                        {\n"
                                       "                            \"clockrate\": \"90000\",\n"
                                       "                            \"id\": 100,\n"
                                       "                            \"name\": \"vp8\",\n"
                                       "                            \"parameters\": {},\n"
                                       "                            \"rtcp-fbs\": [\n"
                                       "                                {\n"
                                       "                                    \"subtype\": \"fir\",\n"
                                       "                                    \"type\": \"ccm\"\n"
                                       "                                }\n"
                                       "                            ]\n"
                                       "                        }\n"
                                       "                    ]\n"
                                       "                }\n"
                                       "            ],\n"
                                       "            \"name\": \"video\"\n"
                                       "        }\n"
                                       "    ],\n"
                                       "    \"id\": \"\"\n"
                                       "}";

    EXPECT_EQ(expectedResult, conferenceJson.dump(4));
}

TEST_F(GeneratorTest, headerExtensions)
{
    legacyapi::Conference conference;

    {
        legacyapi::Channel channel;
        legacyapi::Channel::RtpHdrExt rtpHdrExt;
        rtpHdrExt._id = 1;
        rtpHdrExt._uri = "urn:ietf:params:rtp-hdrext:ssrc-audio-level";
        channel._rtpHeaderHdrExts.push_back(rtpHdrExt);

        legacyapi::Content content;
        content._channels.push_back(channel);
        content._name = "audio";
        conference._contents.push_back(content);
    }

    const auto conferenceJson = legacyapi::Generator::generate(conference);

    const std::string expectedResult =
        "{\n"
        "    \"contents\": [\n"
        "        {\n"
        "            \"channels\": [\n"
        "                {\n"
        "                    \"rtp-hdrexts\": [\n"
        "                        {\n"
        "                            \"id\": 1,\n"
        "                            \"uri\": \"urn:ietf:params:rtp-hdrext:ssrc-audio-level\"\n"
        "                        }\n"
        "                    ]\n"
        "                }\n"
        "            ],\n"
        "            \"name\": \"audio\"\n"
        "        }\n"
        "    ],\n"
        "    \"id\": \"\"\n"
        "}";

    EXPECT_EQ(expectedResult, conferenceJson.dump(4));
}
