#include "crypto/AesGcmIvGenerator.h"
#include <cassert>
#include <cstring>

namespace
{
void adjustProcSize(uint8_t* proc, uint16_t& index, uint16_t ivLength)
{
    if (ivLength > index)
    {
        std::memset(proc + index, 0x00, ivLength - index);
        index = ivLength;
    }
}

void addBytes(uint8_t* proc, uint16_t& index, uint32_t value, uint16_t base)
{
    for (int32_t i = (base - 1); i >= 0; --i)
    {
        proc[index++] = (value >> (8 * i)) & 0xff;
    }
}
} // namespace

namespace crypto
{
AesGcmIvGenerator::AesGcmIvGenerator(const uint8_t* salt, uint16_t saltLength) : _saltLength(saltLength)
{
    assert(saltLength <= DEFAULT_AES_IV_SIZE);
    if (salt != nullptr)
    {
        std::memcpy(_salt, salt, saltLength);
    }
}

void AesGcmIvGenerator::generateForRtp(uint32_t ssrc,
    uint32_t rolloverCount,
    uint16_t sequenceNumber,
    uint8_t* iv,
    uint16_t ivLength)
{
    assert(ivLength >= _saltLength);
    uint8_t proc[ivLength];
    uint16_t index = 2;

    // pad
    std::memset(proc, 0x00, 2);

    // ssrc
    addBytes(proc, index, ssrc, 4);

    // roc
    addBytes(proc, index, rolloverCount, 4);

    // sequence number
    addBytes(proc, index, sequenceNumber, 2);

    adjustProcSize(proc, index, ivLength);

    doGenerate(proc, iv, ivLength);
}

void AesGcmIvGenerator::generateForRec(uint8_t event,
    uint16_t sequenceNumber,
    uint32_t timestamp,
    uint8_t* iv,
    uint16_t ivLength)
{
    assert(ivLength >= _saltLength);
    uint8_t proc[ivLength];
    uint16_t index = 2;

    // pad - use 0x01 to avoid collisions with RTP packets
    std::memset(proc, 0x01, 2);

    // event type
    proc[index++] = event;

    // timestamp
    addBytes(proc, index, timestamp, 4);

    // sequence number
    addBytes(proc, index, sequenceNumber, 2);

    adjustProcSize(proc, index, ivLength);

    doGenerate(proc, iv, ivLength);
}

void AesGcmIvGenerator::doGenerate(const uint8_t* proc, uint8_t* iv, uint16_t ivSize)
{
    // xor salt and processing vector
    for (auto i = 0; i < ivSize; ++i)
    {
        if (i < _saltLength)
        {
            iv[i] = proc[i] ^ _salt[i];
        }
        else
        {
            iv[i] = 0x00;
        }
    }
}
} // namespace crypto
