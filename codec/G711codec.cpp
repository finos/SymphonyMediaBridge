#include "G711codec.h"
#include <cassert>
#include <cstddef>
#include <cstring>
#include <stdio.h>

namespace codec
{
int16_t PcmaCodec::_table[256] = {0};
int8_t PcmaCodec::_encodeTable[2048] = {0};

int16_t PcmuCodec::_table[256] = {0};
uint8_t PcmuCodec::_encodeTable[128] = {0};

void PcmaCodec::encode(const int16_t* pcm, uint8_t* target, size_t samples)
{
    for (size_t n = 0; n < samples; n++)
    {
        if (pcm[n] < 0)
        {
            target[n] = _encodeTable[(~pcm[n]) >> 4] ^ 0x0055;
        }
        else
        {
            target[n] = (_encodeTable[pcm[n] >> 4] | 0x80) ^ 0x0055;
        }
    }
}

void PcmaCodec::decode(const uint8_t* pcma, int16_t* target, size_t samples)
{
    assert(_table[0] != 0);
    for (size_t i = 1; i <= samples; ++i)
    {
        target[samples - i] = _table[pcma[samples - i]];
    }
}

void PcmaCodec::initialize()
{
    for (int16_t d = 0; d < 256; ++d)
    {
        // unmask 0x55 and remove sign bit
        short ix = (d ^ 0x0055) & 0x007F;
        short exponent = ix >> 4;
        short mant = ix & 0x000F;
        if (exponent > 0)
        {
            mant = mant + 16; /* add leading '1', if exponent > 0 */
        }

        mant = (mant << 4) + 8; /* now mantissa left justified and */
        /* 1/2 quantization step added */
        if (exponent > 1)
        {
            mant = mant << (exponent - 1);
        }

        _table[d] = d > 127 ? mant : -mant;
    }

    for (int16_t d = 0; d < 2048; ++d)
    {
        int16_t x = d;
        if (x > 32) // exponent=0 for x <= 32
        {
            short exponent = 1;
            while (x > 16 + 15) // find mantissa and exponent
            {
                x >>= 1;
                exponent++;
            }
            x -= 16; // second step: remove leading '1'
            x += exponent << 4; // now compute encoded value
        }

        _encodeTable[d] = x;
    }
}

void PcmuCodec::encode(const int16_t* pcm, uint8_t* target, size_t samples)
{
    // Change from 14 bit left justified to 14 bit right justified
    // Compute absolute value; adjust for easy processing
    for (size_t n = 0; n < samples; n++)
    {
        // compute 1's complement in case of  neg values. NB: 33 is the difference value
        short absno = pcm[n] < 0 ? ((~pcm[n]) >> 2) + 33 : (pcm[n] >> 2) + 33;

        if (absno > 0x1FFF) // limit to "absno" < 8192
        {
            absno = 0x1FFF;
        }

        short segno = _encodeTable[absno >> 6]; // Determination of sample's segment

        // Mounting the high-nibble of the log-PCM sample
        const short high_nibble = 0x0008 - segno;

        // Mounting the low-nibble of the log PCM sample
        // right shift of mantissa and masking away leading '1'
        short low_nibble = (absno >> segno) & 0x000F;
        low_nibble = 0x000F - low_nibble;

        // Joining the high-nibble and the low-nibble of the log PCM sample
        target[n] = (high_nibble << 4) | low_nibble;

        // Add sign bit
        if (pcm[n] >= 0)
        {
            target[n] = target[n] | 0x0080;
        }
    }
}

void PcmuCodec::decode(const uint8_t* pcmu, int16_t* target, size_t samples)
{
    assert(_table[0] != 0);
    for (size_t i = 1; i <= samples; ++i)
    {
        target[samples - i] = _table[pcmu[samples - i]];
    }
}

void PcmuCodec::initialize()
{
    for (int16_t d = 0; d < 256; ++d)
    {
        // sign-bit = 1 for positiv values
        short sign = d < 0x0080 ? -1 : 1;
        short mantissa = ~d; // 1's complement of input value
        short exponent = (mantissa >> 4) & 0x0007; // extract exponent
        short segment = exponent + 1; // compute segment number
        mantissa = mantissa & 0x000F; // extract mantissa

        // Compute Quantized Sample (14 bit left justified)
        short step = 4 << segment; // position of the LSB = 1 quantization step)
        _table[d] = sign *
            ((0x0080 << exponent) // '1', preceding the mantissa
                + step * mantissa // left shift of mantissa
                + step / 2 // 1/2 quantization step
                - 4 * 33);
    }

    for (int16_t d = 0; d < 128; ++d)
    {
        short i = d;
        short segno = 1;
        while (i != 0)
        {
            segno++;
            i >>= 1;
        }
        _encodeTable[d] = segno;
    }
}

} // namespace codec
