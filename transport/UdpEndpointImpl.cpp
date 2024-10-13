#include "UdpEndpointImpl.h"
#include "dtls/SslDtls.h"
#include "memory/Packet.h"
#include "memory/PacketPoolAllocator.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "utils/Function.h"
#include <cstdint>

#include "crypto/SslHelper.h"

#define DEBUG_ENDPOINT 0

#if DEBUG_ENDPOINT
#define LOG(fmt, ...) logger::debug(fmt, ##__VA_ARGS__)
#else
#define LOG(fmt, ...)
#endif
namespace transport
{

// When this endpoint is shared the number of registration jobs and packets in queue will be plenty
// and the data structures are therefore larger
UdpEndpointImpl::UdpEndpointImpl(jobmanager::JobManager& jobManager,
    size_t maxSessionCount,
    memory::PacketPoolAllocator& allocator,
    const SocketAddress& localPort,
    RtcePoll& epoll,
    bool isShared)
    : _name("UdpEndpointImpl"),
      _baseUdpEndpoint(_name,
          jobManager,
          maxSessionCount,
          allocator,
          localPort,
          epoll,
          std::bind(&UdpEndpointImpl::dispatchReceivedPacket,
              this,
              std::placeholders::_1,
              std::placeholders::_2,
              std::placeholders::_3),
          this),
      _iceListeners(maxSessionCount * 2),
      _dtlsListeners(maxSessionCount * 5),
      _iceResponseListeners(maxSessionCount * 16),
      _defaultListener(nullptr)
{
}

UdpEndpointImpl::~UdpEndpointImpl()
{
    if (_baseUdpEndpoint._receiveJobs.getCount() > 0)
    {
        logger::error("receive job queue not empty", _name.c_str());
    }
    if (_baseUdpEndpoint._sendJobs.getCount() > 0)
    {
        logger::error("send job queue not empty", _name.c_str());
    }
    logger::debug("removed", _name.c_str());
}

void UdpEndpointImpl::sendStunTo(const transport::SocketAddress& target,
    ice::Int96 transactionId,
    const void* data,
    size_t len,
    uint64_t timestamp)
{
    auto* msg = ice::StunMessage::fromPtr(data);
    if (msg->header.isRequest() && !_iceResponseListeners.contains(transactionId))
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
                    LOG("register ICE listener for %04x%04x%04x, count %" PRIu64,
                        _name.c_str(),
                        transactionId.w2,
                        transactionId.w1,
                        transactionId.w0,
                        _iceResponseListeners.size());
                }
            }
        }
    }

    sendTo(target, memory::makeUniquePacket(_baseUdpEndpoint._allocator, data, len));
}

void UdpEndpointImpl::unregisterListener(IEvents* listener)
{
    if (!_baseUdpEndpoint._receiveJobs.post(utils::bind(&UdpEndpointImpl::internalUnregisterListener, this, listener)))
    {
        logger::error("failed to post unregister job", _name.c_str());
    }
}

void UdpEndpointImpl::cancelStunTransaction(ice::Int96 transactionId)
{
    const bool posted = _baseUdpEndpoint._receiveJobs.post([this, transactionId]() {
        _iceResponseListeners.erase(transactionId);
        LOG("remove ICE listener for %04x%04x%04x, count %" PRIu64,
            _name.c_str(),
            transactionId.w2,
            transactionId.w1,
            transactionId.w0,
            _iceResponseListeners.size());
    });
    if (!posted)
    {
        logger::warn("failed to post unregister STUN transaction job", _name.c_str());
    }
}

void UdpEndpointImpl::internalUnregisterListener(IEvents* listener)
{
    // Hashmap allows erasing elements while iterating.
    LOG("unregister %p", _name.c_str(), listener);
    for (auto& item : _iceListeners)
    {
        if (item.second == listener)
        {
            _iceListeners.erase(item.first);
            listener->onUnregistered(*this);
            break;
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
            listener->onUnregistered(*this);
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
                LOG("ICE request for %s src %s, %s",
                    _name.c_str(),
                    users->getNames().first.c_str(),
                    srcAddress.toString().c_str(),
                    listener ? "" : "unknown user");
            }
            if (!users || !listener)
            {
                return;
            }
        }
        else if (msg->header.isResponse())
        {
            auto transactionId = msg->header.transactionId.get();
            listener = _iceResponseListeners.getItem(transactionId);
            if (listener)
            {
                _iceResponseListeners.erase(transactionId);
                LOG("STUN response received for transaction %04x%04x%04x, count %" PRIu64,
                    _name.c_str(),
                    transactionId.w2,
                    transactionId.w1,
                    transactionId.w0,
                    _iceResponseListeners.size());
            }
        }

        if (listener)
        {
            listener->onIceReceived(*this,
                srcAddress,
                _baseUdpEndpoint._socket.getBoundPort(),
                std::move(packet),
                timestamp);
            return;
        }
        else
        {
            LOG("cannot find listener for STUN", _name.c_str());
        }
    }
    else if (transport::isDtlsPacket(packet->get(), packet->getLength()))
    {
        listener = _dtlsListeners.getItem(srcAddress);
        listener = listener ? listener : _defaultListener.load();
        if (listener)
        {
            listener->onDtlsReceived(*this,
                srcAddress,
                _baseUdpEndpoint._socket.getBoundPort(),
                std::move(packet),
                timestamp);
            return;
        }
        else
        {
            LOG("cannot find listener for DTLS source %s", _name.c_str(), srcAddress.toString().c_str());
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
                listener->onRtcpReceived(*this,
                    srcAddress,
                    _baseUdpEndpoint._socket.getBoundPort(),
                    std::move(packet),
                    timestamp);
                return;
            }
            else
            {
                LOG("cannot find listener for RTCP", _name.c_str());
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
                listener->onRtpReceived(*this,
                    srcAddress,
                    _baseUdpEndpoint._socket.getBoundPort(),
                    std::move(packet),
                    timestamp);
                return;
            }
            else
            {
                LOG("cannot find listener for RTP", _name.c_str());
            }
        }
    }
    else
    {
        LOG("Unexpected packet from %s", _name.c_str(), srcAddress.toString().c_str());
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
    auto it = _dtlsListeners.emplace(srcAddress, listener);
    if (it.second)
    {
        logger::debug("register listener for %s", _name.c_str(), srcAddress.toString().c_str());
        listener->onRegistered(*this);
    }
    else if (it.first != _dtlsListeners.cend() && it.first->second == listener)
    {
        // already registered
    }
    else
    {
        _baseUdpEndpoint._receiveJobs.post(utils::bind(&UdpEndpointImpl::swapListener, this, srcAddress, listener));
    }
}

void UdpEndpointImpl::swapListener(const SocketAddress& srcAddress, IEvents* newListener)
{
    auto it = _dtlsListeners.find(srcAddress);
    if (it != _dtlsListeners.cend())
    {
        // src port is re-used. Unregister will look at listener pointer
        if (it->second == newListener)
        {
            return;
        }

        if (it->second)
        {
            it->second->onUnregistered(*this);
        }
        it->second = newListener;
        newListener->onRegistered(*this);
        return;
    }

    logger::warn("dtls listener swap on %s skipped. Already removed", _name.c_str(), srcAddress.toString().c_str());
}

void UdpEndpointImpl::unregisterListener(const SocketAddress& remotePort, IEvents* listener)
{
    _baseUdpEndpoint._receiveJobs.post([this, remotePort, listener]() {
        auto it = _dtlsListeners.find(remotePort);
        if (it == _dtlsListeners.end())
        {
            return;
        }

        if (it->second == listener)
        {
            LOG("remove listener on %s", _name.c_str(), remotePort.toString().c_str());
            _dtlsListeners.erase(it->first);
            listener->onUnregistered(*this);
        }
    });
}
} // namespace transport
