#pragma once
#include <atomic>
namespace bridge
{

class Mixer;
class MixerManager;
struct StreamDescription;
class RequestLogger;
} // namespace bridge

namespace transport
{
class SslDtls;
} // namespace transport

namespace bridge
{
struct ActionContext
{
    ActionContext(bridge::MixerManager& mixerManager, transport::SslDtls& sslDtls)
        : _mixerManager(mixerManager),
          _sslDtls(sslDtls),
          _lastAutoRequestId(0)
    {
    }
    bridge::MixerManager& _mixerManager;
    transport::SslDtls& _sslDtls;
    std::atomic<uint32_t> _lastAutoRequestId;
};
} // namespace bridge