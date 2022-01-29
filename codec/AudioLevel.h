#pragma once

namespace memory
{

class AudioPacket;
class Packet;

} // namespace memory

namespace codec
{

int computeAudioLevel(const memory::AudioPacket& packet);
void addAudioLevelRtpExtension(int extensionId, int audioLeveldBO, memory::Packet& packet);

} // namespace codec
