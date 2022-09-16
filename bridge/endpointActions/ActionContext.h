#pragma once
#include <atomic>
namespace bridge
{

class MixerManager;
} // namespace bridge

namespace transport
{
class ProbeServer;
class SslDtls;
} // namespace transport

namespace bridge
{
struct ActionContext
{
    ActionContext(bridge::MixerManager& mixerManager, transport::SslDtls& sslDtls, transport::ProbeServer& probeServer)
        : mixerManager(mixerManager),
          sslDtls(sslDtls),
          probeServer(probeServer)
    {
    }
    bridge::MixerManager& mixerManager;
    transport::SslDtls& sslDtls;
    transport::ProbeServer& probeServer;
};
} // namespace bridge
