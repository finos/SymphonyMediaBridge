#include "api/Parser.h"
#include "test/ResourceLoader.h"
#include <gtest/gtest.h>

TEST(ParserTest, patchNoIceCandidates)
{
    auto patchBody = ResourceLoader::loadAsJson("api-patch-no-ice-candidates.json");
    api::EndpointDescription endpointDescription = api::Parser::parsePatchEndpoint(patchBody, "endpointId-0");

    ASSERT_EQ("endpointId-0", endpointDescription.endpointId);

    ASSERT_EQ(true, endpointDescription.bundleTransport.isSet());
    auto& bundleTransport = endpointDescription.bundleTransport.get();
    ASSERT_EQ(true, bundleTransport.rtcpMux);

    ASSERT_EQ(true, bundleTransport.ice.isSet());
    auto& ice = bundleTransport.ice.get();
    ASSERT_EQ("Rl3b", ice.ufrag);
    ASSERT_EQ("gb4ISfk9Ppy6M5zYcZdtqldd", ice.pwd);
    ASSERT_EQ(0, ice.candidates.size());
}

TEST(ParserTest, patchEmptyIceCandidates)
{
    ASSERT_TRUE(false);
    auto patchBody = ResourceLoader::loadAsJson("api-patch-empty-ice-candidates.json");
    api::EndpointDescription endpointDescription = api::Parser::parsePatchEndpoint(patchBody, "endpointId-0");

    ASSERT_EQ("endpointId-0", endpointDescription.endpointId);

    ASSERT_EQ(true, endpointDescription.bundleTransport.isSet());
    auto& bundleTransport = endpointDescription.bundleTransport.get();
    ASSERT_EQ(true, bundleTransport.rtcpMux);

    ASSERT_EQ(true, bundleTransport.ice.isSet());
    auto& ice = bundleTransport.ice.get();
    ASSERT_EQ("Rl3b", ice.ufrag);
    ASSERT_EQ("gb4ISfk9Ppy6M5zYcZdtqldd", ice.pwd);
    ASSERT_EQ(0, ice.candidates.size());
}
