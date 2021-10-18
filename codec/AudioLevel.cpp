#include "AudioLevel.h"
#include "memory/Packet.h"
#include "rtp/RtpHeader.h"
#include <cmath>
#include <inttypes.h>

namespace codec
{

int computeAudioLevel(const int16_t* payload, int count)
{
    const double overload = 0x8000;
    double rms = 0;
    for (int i = 0; i < count; ++i)
    {
        double sample = double(payload[i]);
        rms += sample * sample;
    }
    rms /= (overload * overload);
    rms = count ? std::sqrt(rms / count) : 0;
    rms = std::max(rms, 1e-9);

    return -std::max(-127, static_cast<int>(20 * std::log10(rms)));
}

void addAudioLevelRtpExtension(int extensionId, memory::Packet& packet)
{
    const auto rtpHeader = rtp::RtpHeader::fromPacket(packet);
    if (!rtpHeader)
    {
        return;
    }

    const auto payload = reinterpret_cast<const int16_t*>(rtpHeader->getPayload());
    const int count = (packet.getLength() - rtpHeader->headerLength()) / sizeof(int16_t);
    const int dBO = computeAudioLevel(payload, count);

    uint16_t tmp[count];
    std::memcpy(tmp, payload, count * sizeof(int16_t));
    rtp::RtpHeaderExtension extensionHead(rtpHeader->getExtensionHeader());

    rtp::GeneralExtension1Byteheader extAudioLevel;
    extAudioLevel.id = extensionId;
    extAudioLevel.len = 0;
    extAudioLevel.data[0] = dBO;
    auto cursor = extensionHead.extensions().begin();
    extensionHead.addExtension(cursor, extAudioLevel);
    rtpHeader->setExtensions(extensionHead);

    std::memcpy(rtpHeader->getPayload(), tmp, count * sizeof(int16_t));
    packet.setLength(rtpHeader->headerLength() + count * sizeof(int16_t));
}

} // namespace codec
