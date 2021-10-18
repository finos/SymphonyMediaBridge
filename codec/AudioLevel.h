#pragma once

namespace memory
{
class Packet;
}

namespace codec
{
void addAudioLevelRtpExtension(int extensionId, memory::Packet& packet);
}