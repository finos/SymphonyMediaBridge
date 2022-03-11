#include "UdpEndpoint.h"
#include "dtls/SslDtls.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include <cstdint>

namespace transport
{

namespace
{
using namespace transport;
class UnRegisterListenerJob : public jobmanager::Job
{
public:
    UnRegisterListenerJob(UdpEndpoint& endpoint, Endpoint::IEvents* listener) : _endpoint(endpoint), _listener(listener)
    {
    }

    void run() override { _endpoint.internalUnregisterListener(_listener); }

private:
    UdpEndpoint& _endpoint;
    Endpoint::IEvents* _listener;
};
} // namespace

// When this endpoint is shared the number of registration jobs and packets in queue will be plenty
// and the data structures are therefore larger
UdpEndpoint::UdpEndpoint(jobmanager::JobManager& jobManager,
    size_t maxSessionCount,
    memory::PacketPoolAllocator& allocator,
    const SocketAddress& localPort,
    RtcePoll& epoll,
    bool isShared)
    : BaseUdpEndpoint("UdpEndpoint", jobManager, maxSessionCount, allocator, localPort, epoll, isShared),
      _iceListeners(maxSessionCount),
      _dtlsListeners(maxSessionCount),
      _iceResponseListeners(maxSessionCount * 4)
{
}

void UdpEndpoint::sendStunTo(const transport::SocketAddress& target,
    __uint128_t transactionId,
    const void* data,
    size_t len,
    uint64_t timestamp)
{
    auto* msg = ice::StunMessage::fromPtr(data);
    if (msg->header.isRequest() && _iceResponseListeners.find(transactionId) == _iceResponseListeners.cend())
    {
        auto names = msg->getAttribute<ice::StunUserName>(ice::StunAttribute::USERNAME);
        if (names)
        {
            auto localUser = names->getNames().second;
            auto it = _iceListeners.find(localUser);
            if (it != _iceListeners.cend())
            {
                assert(it->second);
                _iceResponseListeners.emplace(transactionId, it->second);
            }
        }
    }
    sendTo(target, memory::makePacket(_allocator, data, len), _allocator);
}

void UdpEndpoint::unregisterListener(IEvents* listener)
{
    if (!_receiveJobs.addJob<UnRegisterListenerJob>(*this, listener))
    {
        logger::error("failed to post unregister job", _name.c_str());
    }
}

void UdpEndpoint::internalUnregisterListener(IEvents* listener)
{
    // Hashmap allows erasing elements while iterating.
    logger::debug("unregister %p", _name.c_str(), listener);
    for (auto& item : _iceListeners)
    {
        if (item.second == listener)
        {
            _iceListeners.erase(item.first);
        }
    }

    for (auto& responseListener : _iceResponseListeners)
    {
        if (responseListener.second == listener)
        {
            _iceResponseListeners.erase(responseListener.first);
        }
    }

    for (auto& item : _dtlsListeners)
    {
        if (item.second == listener)
        {
            _dtlsListeners.erase(item.first);
        }
    }

    listener->onUnregistered(*this);
}

namespace
{
template <typename KeyType>
UdpEndpoint::IEvents* findListener(concurrency::MpmcHashmap32<KeyType, UdpEndpoint::IEvents*>& map, const KeyType& key)
{
    auto it = map.find(key);
    if (it != map.cend())
    {
        return it->second;
    }
    return nullptr;
}
} // namespace

void UdpEndpoint::dispatchReceivedPacket(const SocketAddress& srcAddress, memory::Packet* packet)
{
    UdpEndpoint::IEvents* listener = _defaultListener;

    if (ice::isStunMessage(packet->get(), packet->getLength()))
    {
        auto msg = ice::StunMessage::fromPtr(packet->get());

        if (msg->header.isRequest())
        {
            auto users = msg->getAttribute<ice::StunUserName>(ice::StunAttribute::USERNAME);
            if (users)
            {
                auto userName = users->getNames().first;
                listener = findListener(_iceListeners, userName);
            }
        }
        else if (msg->header.isResponse())
        {
            auto transactionId = msg->header.transactionId.get();
            listener = findListener(_iceResponseListeners, transactionId);
            if (listener)
            {
                // TODO there is still risk of depletion as not all transactions are responded to.
                // Only when channel is brought down will it clear its transaction listeners
                _iceResponseListeners.erase(transactionId);
            }
        }
        if (listener)
        {
            listener->onIceReceived(*this, srcAddress, _socket.getBoundPort(), packet, _allocator);
            return;
        }
    }
    else if (transport::isDtlsPacket(packet->get()))
    {
        listener = findListener(_dtlsListeners, srcAddress);
        listener = listener ? listener : _defaultListener.load();
        if (listener)
        {
            listener->onDtlsReceived(*this, srcAddress, _socket.getBoundPort(), packet, _allocator);
            return;
        }
    }
    else if (rtp::isRtcpPacket(packet->get(), packet->getLength()))
    {
        auto rtcpReport = rtp::RtcpReport::fromPtr(packet->get(), packet->getLength());
        if (rtcpReport)
        {
            listener = findListener(_dtlsListeners, srcAddress);

            if (listener)
            {
                listener->onRtcpReceived(*this, srcAddress, _socket.getBoundPort(), packet, _allocator);
                return;
            }
        }
    }
    else if (rtp::isRtpPacket(packet->get(), packet->getLength()))
    {
        auto rtpPacket = rtp::RtpHeader::fromPacket(*packet);
        if (rtpPacket)
        {
            listener = findListener(_dtlsListeners, srcAddress);

            if (listener)
            {
                listener->onRtpReceived(*this, srcAddress, _socket.getBoundPort(), packet, _allocator);
                return;
            }
        }
    }
    else
    {
        logger::info("Unexpected packet from %s", "Rafael", srcAddress.toString().c_str());
    }
    // unexpected packet that can come from anywhere. We do not log as it facilitates DoS
    _allocator.free(packet);
}

void UdpEndpoint::registerListener(const std::string& stunUserName, IEvents* listener)
{
    _iceListeners.emplace(stunUserName, listener);
}

// If using ICE, must be called from receive job queue to sync unregister
void UdpEndpoint::registerListener(const SocketAddress& srcAddress, IEvents* listener)
{
    auto dtlsIt = _dtlsListeners.find(srcAddress);
    if (dtlsIt != _dtlsListeners.end())
    {
        // src port is re-used. Unregister will look at listener pointer
        dtlsIt->second = listener;
    }
    else
    {
        _dtlsListeners.emplace(srcAddress, listener);
    }
}

} // namespace transport
