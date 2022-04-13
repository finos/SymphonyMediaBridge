#include "memory/MemoryFile.h"
#include "transport/ice/IceCandidate.h"

memory::MemoryFile& operator<<(memory::MemoryFile& f, const transport::SocketAddress& address);
memory::MemoryFile& operator>>(memory::MemoryFile& f, transport::SocketAddress& address);
memory::MemoryFile& operator<<(memory::MemoryFile& f, const ice::IceCandidate& candidate);
memory::MemoryFile& operator>>(memory::MemoryFile& f, ice::IceCandidate& candidate);
