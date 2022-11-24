#include "UdpEndpointImpl.h"
#include "dtls/SslDtls.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include <cstdint>

#include "crypto/SslHelper.h"

namespace transport
{

namespace
{
using namespace transport;
class UnRegisterListenerJob : public jobmanager::Job
{
public:
    UnRegisterListenerJob(UdpEndpointImpl& endpoint, Endpoint::IEvents* listener)
        : _endpoint(endpoint),
          _listener(listener)
    {
    }

    void run() override { _endpoint.internalUnregisterListener(_listener); }

private:
    UdpEndpointImpl& _endpoint;
    Endpoint::IEvents* _listener;
};

class UnRegisterNotifyListenerJob : public jobmanager::Job
{
public:
    UnRegisterNotifyListenerJob(UdpEndpointImpl& endpoint, Endpoint::IEvents& listener)
        : _endpoint(endpoint),
          _listener(listener)
    {
    }

    void run() override { _listener.onUnregistered(_endpoint); }

private:
    UdpEndpointImpl& _endpoint;
    Endpoint::IEvents& _listener;
};

} // namespace

// When this endpoint is shared the number of registration jobs and packets in queue will be plenty
// and the data structures are therefore larger
UdpEndpointImpl::UdpEndpointImpl(jobmanager::JobManager& jobManager,
    size_t maxSessionCount,
    memory::PacketPoolAllocator& allocator,
    const SocketAddress& localPort,
    RtcePoll& epoll,
    bool isShared)
    : BaseUdpEndpoint("UdpEndpoint", jobManager, maxSessionCount, allocator, localPort, epoll, isShared),
      _iceListeners(maxSessionCount * 2),
      _dtlsListeners(maxSessionCount * 2),
      _iceResponseListeners(maxSessionCount * 4)
{
}

UdpEndpointImpl::~UdpEndpointImpl()
{
    logger::debug("removed", _name.c_str());
}

void UdpEndpointImpl::sendStunTo(const transport::SocketAddress& target,
    __uint128_t transactionId,
    const void* data,
    size_t len,
    uint64_t timestamp)
{
    auto* msg = ice::StunMessage::fromPtr(data);
    if (msg->header.isRequest() && !_dtlsListeners.contains(target) && !_iceResponseListeners.contains(transactionId))
    {
        auto names = msg->getAttribute<ice::StunUserName>(ice::StunAttribute::USERNAME);
        if (names)
        {
            auto localUser = names->getNames().second;
            auto it = _iceListeners.find(localUser);
            if (it != _iceListeners.cend())
            {
                assert(it->second);
                auto pair = _iceResponseListeners.emplace(transactionId, it->second);
                if (!pair.second)
                {
                    logger::warn("Pending ICE request lookup table is full", _name.c_str());
                }
                else
                {
                    const IndexableInteger<__uint128_t, uint32_t> id(transactionId);
                    logger::debug("register ICE listener for %04x%04x%04x", _name.c_str(), id[1], id[2], id[3]);
                }
            }
        }
    }
    sendTo(target, memory::makeUniquePacket(_allocator, data, len));
}

void UdpEndpointImpl::unregisterListener(IEvents* listener)
{
    if (!_receiveJobs.addJob<UnRegisterListenerJob>(*this, listener))
    {
        logger::error("failed to post unregister job", _name.c_str());
    }
}

void UdpEndpointImpl::cancelStunTransaction(__uint128_t transactionId)
{
    // Hashmap allows erasing elements while iterating.
    auto itPair = _iceResponseListeners.find(transactionId);
    if (itPair != _iceResponseListeners.cend())
    {
        _iceResponseListeners.erase(transactionId);
    }
}

void UdpEndpointImpl::internalUnregisterListener(IEvents* listener)
{
    // Hashmap allows erasing elements while iterating.
    logger::debug("unregister %p", _name.c_str(), listener);
    for (auto& item : _iceListeners)
    {
        if (item.second == listener)
        {
            _iceListeners.erase(item.first);
            _receiveJobs.addJob<UnRegisterNotifyListenerJob>(*this, *listener);
        }
    }

    for (auto& responseListener : _iceResponseListeners)
    {
        if (responseListener.second == listener)
        {
            _iceResponseListeners.erase(responseListener.first);
            // must be iceListener to be iceResponseListener so no extra unreg notification
        }
    }

    for (auto& item : _dtlsListeners)
    {
        if (item.second == listener)
        {
            _dtlsListeners.erase(item.first);
            _receiveJobs.addJob<UnRegisterNotifyListenerJob>(*this, *listener);
        }
    }
}

void UdpEndpointImpl::dispatchReceivedPacket(const SocketAddress& srcAddress,
    memory::UniquePacket packet,
    const uint64_t timestamp)
{
    UdpEndpointImpl::IEvents* listener = _defaultListener;

    if (ice::isStunMessage(packet->get(), packet->getLength()))
    {
        auto msg = ice::StunMessage::fromPtr(packet->get());

        if (msg->header.isRequest())
        {
            auto users = msg->getAttribute<ice::StunUserName>(ice::StunAttribute::USERNAME);
            if (users)
            {
                auto userName = users->getNames().first;
                listener = _iceListeners.getItem(userName);
            }
            logger::debug("ICE request to %s src %s",
                _name.c_str(),
                users->getNames().first.c_str(),
                srcAddress.toString().c_str());
        }
        else if (msg->header.isResponse())
        {
            auto transactionId = msg->header.transactionId.get();
            listener = _iceResponseListeners.getItem(transactionId);
            if (listener)
            {
                _iceResponseListeners.erase(transactionId);
            }
            else
            {
                listener = _dtlsListeners.getItem(srcAddress);
            }
        }
        if (listener)
        {
            listener->onIceReceived(*this, srcAddress, _socket.getBoundPort(), std::move(packet), timestamp);
            return;
        }
    }
    else if (transport::isDtlsPacket(packet->get()))
    {
        listener = _dtlsListeners.getItem(srcAddress);
        listener = listener ? listener : _defaultListener.load();
        if (listener)
        {
            listener->onDtlsReceived(*this, srcAddress, _socket.getBoundPort(), std::move(packet), timestamp);
            return;
        }
    }
    else if (rtp::isRtcpPacket(packet->get(), packet->getLength()))
    {
        auto rtcpReport = rtp::RtcpReport::fromPtr(packet->get(), packet->getLength());
        if (rtcpReport)
        {
            listener = _dtlsListeners.getItem(srcAddress);

            if (listener)
            {
                listener->onRtcpReceived(*this, srcAddress, _socket.getBoundPort(), std::move(packet), timestamp);
                return;
            }
        }
    }
    else if (rtp::isRtpPacket(packet->get(), packet->getLength()))
    {
        auto rtpPacket = rtp::RtpHeader::fromPacket(*packet);
        if (rtpPacket)
        {
            listener = _dtlsListeners.getItem(srcAddress);

            if (listener)
            {
                listener->onRtpReceived(*this, srcAddress, _socket.getBoundPort(), std::move(packet), timestamp);
                return;
            }
        }
    }
    else
    {
        logger::info("Unexpected packet from %s", _name.c_str(), srcAddress.toString().c_str());
    }
    // unexpected packet that can come from anywhere. We do not log as it facilitates DoS
}

void UdpEndpointImpl::registerListener(const std::string& stunUserName, IEvents* listener)
{
    if (_iceListeners.contains(stunUserName))
    {
        return;
    }

    _iceListeners.emplace(stunUserName, listener);
    listener->onRegistered(*this);
}

/** If using ICE, must be called from receive job queue to sync unregister */
void UdpEndpointImpl::registerListener(const SocketAddress& srcAddress, IEvents* listener)
{
    auto dtlsIt = _dtlsListeners.find(srcAddress);
    if (dtlsIt != _dtlsListeners.end())
    {
        // src port is re-used. Unregister will look at listener pointer
        if (dtlsIt->second == listener)
        {
            return;
        }

        if (dtlsIt->second)
        {
            _receiveJobs.addJob<UnRegisterNotifyListenerJob>(*this, *dtlsIt->second);
        }
        dtlsIt->second = listener;
        listener->onRegistered(*this);
    }
    else
    {
        _dtlsListeners.emplace(srcAddress, listener);
        listener->onRegistered(*this);
    }
}

void UdpEndpointImpl::focusListener(const SocketAddress& remotePort, IEvents* listener)
{
    _receiveJobs.post([this, remotePort, listener]() {
        for (auto& item : _dtlsListeners)
        {
            if (item.second == listener && item.first != remotePort)
            {
                _dtlsListeners.erase(item.first);
                listener->onUnregistered(*this);
            }
        }
    });
}

} // namespace transport
