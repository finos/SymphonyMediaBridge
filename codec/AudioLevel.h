#pragma once
#include <cstdint>

namespace memory
{

class AudioPacket;
class Packet;

} // namespace memory

namespace codec
{

int computeAudioLevel(const memory::AudioPacket& packet);
int computeAudioLevel(const int16_t* payload, int count);
void addAudioLevelRtpExtension(int extensionId, int audioLeveldBO, memory::Packet& packet);

} // namespace codec
