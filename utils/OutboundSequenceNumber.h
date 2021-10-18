#pragma once

#include <cstdint>

namespace utils
{

namespace OutboundSequenceNumber
{

constexpr uint32_t maxSequenceNumberJump = 16384;

/**
 * Calculate the next outbound sequence number.
 *
 * @param extendedSequenceNumber [in] the extended sequence number of the next packet to send.
 * @param highestSeenExtendedSequenceNumber [in,out] The highest seen inbound extended sequence number, before rewriting
 * it for this outbound context.
 * @param highestSentExtendedSequenceNumber [in,out] The highest sent outbound extended sequence number after rewriting
 * it for this outbound context.
 * @param outNextSequenceNumber [out] The sequence number (non-extended) to set in the outbound packet header.
 * @return true if the packet should be sent, false if the packet should be dropped.
 */
inline bool process(const uint32_t extendedSequenceNumber,
    uint32_t& highestSeenExtendedSequenceNumber,
    uint32_t& highestSentExtendedSequenceNumber,
    uint16_t& outNextSequenceNumber)
{
    // lastProtectedExtendedSequenceNumber == 0xFFFFFFFF means this is the first packet for this outbound context
    if (__builtin_expect((highestSeenExtendedSequenceNumber == 0xFFFFFFFF), 0))
    {
        highestSentExtendedSequenceNumber = extendedSequenceNumber & 0xFFFF;
        highestSeenExtendedSequenceNumber = extendedSequenceNumber;
        outNextSequenceNumber = highestSentExtendedSequenceNumber & 0xFFFF;
        return true;
    }

    const auto expectedSequenceNumber = highestSeenExtendedSequenceNumber + 1;
    if (__builtin_expect((extendedSequenceNumber == expectedSequenceNumber), 1))
    {
        ++highestSentExtendedSequenceNumber;
        highestSeenExtendedSequenceNumber = extendedSequenceNumber;
        outNextSequenceNumber = highestSentExtendedSequenceNumber & 0xFFFF;
        return true;
    }

    if (extendedSequenceNumber > highestSeenExtendedSequenceNumber + maxSequenceNumberJump)
    {
        ++highestSentExtendedSequenceNumber;
        highestSeenExtendedSequenceNumber = extendedSequenceNumber;
        outNextSequenceNumber = highestSentExtendedSequenceNumber & 0xFFFF;
        return true;
    }

    if (extendedSequenceNumber > expectedSequenceNumber)
    {
        highestSentExtendedSequenceNumber += (extendedSequenceNumber - expectedSequenceNumber) + 1;
        highestSeenExtendedSequenceNumber = extendedSequenceNumber;
        outNextSequenceNumber = highestSentExtendedSequenceNumber & 0xFFFF;
        return true;
    }

    if (extendedSequenceNumber < highestSeenExtendedSequenceNumber)
    {
        const auto offset = highestSeenExtendedSequenceNumber - extendedSequenceNumber;
        if (offset > maxSequenceNumberJump)
        {
            return false;
        }
        outNextSequenceNumber = (highestSentExtendedSequenceNumber - offset) & 0xFFFF;
        return true;
    }

    return false;
}

} // namespace OutboundSequenceNumber

} // namespace utils
