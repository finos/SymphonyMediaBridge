#pragma once
#include <atomic>
namespace bridge
{

class MixerManager;
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
        : mixerManager(mixerManager),
          sslDtls(sslDtls)
    {
    }
    bridge::MixerManager& mixerManager;
    transport::SslDtls& sslDtls;
};
} // namespace bridge
