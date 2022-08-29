#include "api/Parser.h"
#include "test/ResourceLoader.h"
#include <gtest/gtest.h>

TEST(ParserTest, patchNoIceCandidates)
{
    auto patchBody = ResourceLoader::loadAsJson("api-patch-no-ice-candidates.json");
    api::EndpointDescription endpointDescription = api::Parser::parsePatchEndpoint(patchBody, "endpointId-0");

    ASSERT_EQ("endpointId-0", endpointDescription._endpointId);

    ASSERT_EQ(true, endpointDescription._bundleTransport.isSet());
    auto& bundleTransport = endpointDescription._bundleTransport.get();
    ASSERT_EQ(true, bundleTransport._rtcpMux);

    ASSERT_EQ(true, bundleTransport._ice.isSet());
    auto& ice = bundleTransport._ice.get();
    ASSERT_EQ("Rl3b", ice._ufrag);
    ASSERT_EQ("gb4ISfk9Ppy6M5zYcZdtqldd", ice._pwd);
    ASSERT_EQ(0, ice._candidates.size());
}

TEST(ParserTest, patchEmptyIceCandidates)
{
    auto patchBody = ResourceLoader::loadAsJson("api-patch-empty-ice-candidates.json");
    api::EndpointDescription endpointDescription = api::Parser::parsePatchEndpoint(patchBody, "endpointId-0");

    ASSERT_EQ("endpointId-0", endpointDescription._endpointId);

    ASSERT_EQ(true, endpointDescription._bundleTransport.isSet());
    auto& bundleTransport = endpointDescription._bundleTransport.get();
    ASSERT_EQ(true, bundleTransport._rtcpMux);

    ASSERT_EQ(true, bundleTransport._ice.isSet());
    auto& ice = bundleTransport._ice.get();
    ASSERT_EQ("Rl3b", ice._ufrag);
    ASSERT_EQ("gb4ISfk9Ppy6M5zYcZdtqldd", ice._pwd);
    ASSERT_EQ(0, ice._candidates.size());
}