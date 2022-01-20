#pragma once

namespace memory
{
class AudioPacket;
}

namespace codec
{
void addAudioLevelRtpExtension(int extensionId, memory::AudioPacket& packet);
}
