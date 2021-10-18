#include "crypto/AesGcmIvGenerator.h"
#include "crypto/SslHelper.h"
#include "utils/Base64.h"
#include <gtest/gtest.h>

TEST(AesGcmIvGenerator, GenerateForRTP)
{
    std::string saltEnc = "UXVpZCBwcm8gcXVv";
    size_t saltLen = utils::Base64::decodeLength(saltEnc);
    uint8_t saltDec[saltLen];
    utils::Base64::decode(saltEnc, saltDec, saltLen);

    uint32_t ssrc = 1426170034;
    uint16_t sequenceNumber = 61819;
    uint32_t roc = 0;

    crypto::AesGcmIvGenerator ivGenerator(saltDec, saltLen);
    uint16_t ivLen = 12;
    uint8_t iv[ivLen];
    ivGenerator.generateForRtp(ssrc, roc, sequenceNumber, iv, ivLen);

    EXPECT_STREQ(crypto::toHexString(iv, ivLen).c_str(), "51753c6580c2726f20718414");
}

TEST(AesGcmIvGenerator, GenerateForRec)
{
    std::string saltEnc = "UXVpZCBwcm8gcXVv";
    size_t saltLen = utils::Base64::decodeLength(saltEnc);
    uint8_t saltDec[saltLen];
    utils::Base64::decode(saltEnc, saltDec, saltLen);

    auto event = static_cast<uint8_t>(recp::RecEventType::StreamRemoved);
    uint16_t seq = 61819;
    uint32_t timestamp = 1426170034;

    crypto::AesGcmIvGenerator ivGenerator(saltDec, saltLen);
    size_t ivLen = 12;
    uint8_t iv[ivLen];
    ivGenerator.generateForRec(event, seq, timestamp, iv, ivLen);

    EXPECT_STREQ(crypto::toHexString(iv, ivLen).c_str(), "50746a3121d0c09e5b71756f");
}
