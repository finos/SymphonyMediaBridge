#pragma once

#include "rtp/RtpHeader.h"
#include "transport/recp/RecHeader.h"

namespace crypto
{
const uint16_t DEFAULT_AES_IV_SIZE = 12;
class AesGcmIvGenerator
{
public:
    AesGcmIvGenerator(const uint8_t* salt, uint16_t saltLength);

    void generateForRtp(uint32_t ssrc, uint32_t rolloverCount, uint16_t sequenceNumber, uint8_t* iv, uint16_t ivLength);
    void generateForRec(uint8_t event, uint16_t sequenceNumber, uint32_t timestamp, uint8_t* iv, uint16_t ivLength);

private:
    uint8_t _salt[DEFAULT_AES_IV_SIZE];
    const uint16_t _saltLength;

    void doGenerate(const uint8_t* proc, uint8_t* iv, uint16_t ivSize);
};
} // namespace crypto
