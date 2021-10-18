#pragma once
#include <cinttypes>

struct Simple
{
    Simple() : ssrc(0), seqNo(0) {}
    Simple(uint32_t ssrc_, int seqNo_) : ssrc(ssrc_), seqNo(seqNo_) {}

    uint32_t ssrc;
    int seqNo;
    int data[90];
};

struct SimpleSmall
{
    SimpleSmall() : ssrc(0), seqNo(0) {}
    SimpleSmall(uint32_t ssrc_, int seqNo_) : ssrc(ssrc_), seqNo(seqNo_) {}
    uint32_t ssrc;
    int seqNo;
};
