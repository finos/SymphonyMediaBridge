#pragma once
#include "config/Config.h"
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
    ActionContext(bridge::MixerManager& mixerManager,
        transport::SslDtls& sslDtls,
        transport::ProbeServer& probeServer,
        const config::Config& config)
        : mixerManager(mixerManager),
          sslDtls(sslDtls),
          probeServer(probeServer),
          config(config)
    {
    }

    bridge::MixerManager& mixerManager;
    transport::SslDtls& sslDtls;
    transport::ProbeServer& probeServer;
    const config::Config& config;
};
} // namespace bridge
