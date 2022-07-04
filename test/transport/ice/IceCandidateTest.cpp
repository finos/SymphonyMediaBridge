#include "transport/ice/IceCandidate.h"
#include <gtest/gtest.h>


TEST(IceCandidateTest, transportTypeToString)
{
    ASSERT_EQ(std::string("UDP"), ice::toString(ice::TransportType::UDP));
    ASSERT_EQ(std::string("TCP"), ice::toString(ice::TransportType::TCP));
    ASSERT_EQ(std::string("SSLTCP"), ice::toString(ice::TransportType::SSLTCP));
}

TEST(IceCandidateTest, candidateTypeToString)
{
    ASSERT_EQ(std::string("HOST"), ice::toString(ice::IceCandidate::Type::HOST));
    ASSERT_EQ(std::string("SRFLX"), ice::toString(ice::IceCandidate::Type::SRFLX));
    ASSERT_EQ(std::string("PRFLX"), ice::toString(ice::IceCandidate::Type::PRFLX));
    ASSERT_EQ(std::string("RELAY"), ice::toString(ice::IceCandidate::Type::RELAY));
}