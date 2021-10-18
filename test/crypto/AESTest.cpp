#include "crypto/AesGcmIvGenerator.h"
#include "crypto/SslHelper.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/recp/RecStartStopEventBuilder.h"
#include "utils/Base64.h"
#include <gtest/gtest.h>
#include <openssl/rand.h>

constexpr void generateKey(uint8_t* key, const uint16_t keyLength)
{
    for (auto i = 0; i < keyLength; ++i)
    {
        key[i] = i;
    }
}

TEST(AES, EncryptAndDecrypt)
{
    std::string base64Key = "gOTrkn6EId/xfBql/G+lvT/vbgBuFzSZtFWwU+xwfkk=";
    uint8_t key[32];
    utils::Base64::decode(base64Key, key, 32);
    size_t ivLen = 12;
    auto iv = new unsigned char[ivLen];
    RAND_bytes(iv, sizeof(iv));

    crypto::AES aes(key, 32);

    auto plaintext = "Hello AES world!";
    auto plaintextLength = strlen(plaintext);
    auto ciphertextLength = 4096; // buffer large enough
    unsigned char ciphertext[ciphertextLength];
    auto re = aes.encrypt(reinterpret_cast<const unsigned char*>(plaintext),
        plaintextLength,
        ciphertext,
        reinterpret_cast<uint16_t&>(ciphertextLength),
        iv,
        ivLen);
    EXPECT_TRUE(re);

    unsigned char plaintextDecrypted[plaintextLength];
    auto rd = aes.decrypt(ciphertext,
        ciphertextLength,
        plaintextDecrypted,
        reinterpret_cast<uint16_t&>(plaintextLength),
        iv,
        ivLen);
    EXPECT_TRUE(rd);

    std::string decrypted;
    for (const auto& value : plaintextDecrypted)
    {
        decrypted += (char)value;
    }
    EXPECT_EQ(decrypted.length(), plaintextLength);
    EXPECT_STREQ(decrypted.c_str(), plaintext);
}

// This test uses the values from the rfc7714 to validate the encryption algorithm
TEST(AES, EncryptRtpPayload)
{
    std::string headerEnc = "gEDxe4BB+NNVAaCy";
    std::string payloadEnc = "R2FsbGlhIGVzdCBvbW5pcyBkaXZpc2EgaW4gcGFydGVzIHRyZXM=";
    auto headerLen = utils::Base64::decodeLength(headerEnc);
    auto payloadLen = utils::Base64::decodeLength(payloadEnc);
    uint8_t headerDec[headerLen];
    uint8_t payloadDec[payloadLen];
    utils::Base64::decode(headerEnc, headerDec, headerLen);
    utils::Base64::decode(payloadEnc, payloadDec, payloadLen);

    std::string saltEnc = "UXVpZCBwcm8gcXVv";
    const size_t saltLength = 12;
    EXPECT_EQ(saltLength, utils::Base64::decodeLength(saltEnc));

    uint8_t saltDec[saltLength];
    utils::Base64::decode(saltEnc, saltDec, saltLength);

    uint32_t ssrc = 1426170034;
    uint16_t sequenceNumber = 61819;
    uint32_t roc = 0;

    size_t ivLen = 12;
    uint8_t iv[ivLen];
    crypto::AesGcmIvGenerator ivGenerator(saltDec, saltLength);
    ivGenerator.generateForRtp(ssrc, roc, sequenceNumber, iv, ivLen);

    const uint16_t keyLength = 32; // 32 bytes -> 256 bits
   uint8_t key[keyLength];
    generateKey(key, keyLength);

    crypto::AES aes(key, keyLength);

    uint16_t bufLength = 4096;
    unsigned char encrypted[bufLength];

    aes.gcmEncrypt(payloadDec, payloadLen, encrypted, bufLength, iv, ivLen, headerDec, headerLen);

    auto hexEnc = crypto::toHexString(encrypted, bufLength);

    EXPECT_STREQ(hexEnc.c_str(),
        "32b1de78a822fe12ef9f78fa332e33aab18012389a58e2f3b50b2a0276ffae0f1ba63799b87b7aa3db36dfffd6b0f9bb7878d7a76c13");
}

TEST(AES, EncryptRecPayload)
{
    memory::PacketPoolAllocator allocator(4096 * 32, "testAES");
    auto recPacket = recp::RecStartStopEventBuilder(allocator)
                         .setSequenceNumber(6950)
                         .setTimestamp(1620658227)
                         .setUserId("userId0")
                         .setRecordingId("recordingId0")
                         .setAudioEnabled(true)
                         .setVideoEnabled(false)
                         .build();

    std::string saltEnc = "UXVpZCBwcm8gcXVv";
    const size_t saltLength = 12;
    EXPECT_EQ(saltLength, utils::Base64::decodeLength(saltEnc));

    uint8_t saltDec[saltLength];
    utils::Base64::decode(saltEnc, saltDec, saltLength);

    size_t ivLen = 12;
    uint8_t iv[ivLen];
    crypto::AesGcmIvGenerator ivGenerator(saltDec, saltLength);
    ivGenerator.generateForRec((uint8_t)0x01, 6950, 1620658227, iv, ivLen);

    const uint16_t keyLength = 32; // 32 bytes -> 256 bits
    uint8_t key[keyLength];
    generateKey(key, keyLength);

    crypto::AES aes(key, keyLength);

    auto header = recp::RecHeader::fromPacket(*recPacket);

    auto payload = header->getPayload();
    auto payloadLength = recPacket->getLength() - recp::REC_HEADER_SIZE;
    uint16_t encryptedLength = 4096;

    aes.gcmEncrypt(payload,
        payloadLength,
        reinterpret_cast<unsigned char*>(payload),
        encryptedLength,
        iv,
        crypto::DEFAULT_AES_IV_SIZE,
        reinterpret_cast<unsigned char*>(header),
        recp::REC_HEADER_SIZE);

    recPacket->setLength(recp::REC_HEADER_SIZE + encryptedLength);

    auto hexEnc = crypto::toHexString(recPacket->get(), recPacket->getLength());

    allocator.free(recPacket);

    EXPECT_STREQ(hexEnc.c_str(),
        "00011b26609948336dcd5dbbd33513c2374cd0b1bae531e3a0b48a7567b33ab3e17757ebb3be3869b78e64a1ae501f");
}
