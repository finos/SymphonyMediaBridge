#include "transport/RecordingEndpoint.h"
#include "transport/recp/RecControlHeader.h"

namespace transport
{
namespace
{
using namespace transport;
class UnRegisterRecordingListenerJob : public jobmanager::Job
{
public:
    UnRegisterRecordingListenerJob(RecordingEndpoint& endpoint, RecordingEndpoint::IRecordingEvents* listener)
        : _endpoint(endpoint),
          _listener(listener)
    {
    }

    void run() override { _endpoint.internalUnregisterListener(_listener); }

private:
    RecordingEndpoint& _endpoint;
    RecordingEndpoint::IRecordingEvents* _listener;
};
} // namespace

RecordingEndpoint::RecordingEndpoint(jobmanager::JobManager& jobManager,
    size_t maxSessionCount,
    memory::PacketPoolAllocator& allocator,
    const SocketAddress& localPort,
    RtcePoll& epoll,
    bool isShared)
    : BaseUdpEndpoint("RecordingEndpoint", jobManager, maxSessionCount, allocator, localPort, epoll, isShared),
      _listeners(maxSessionCount)
{
}

RecordingEndpoint::~RecordingEndpoint()
{
    logger::debug("removed", _name.c_str());
}

void RecordingEndpoint::internalUnregisterListener(IRecordingEvents* listener)
{
    // Hashmap allows erasing elements while iterating.
    logger::debug("unregister %p", _name.c_str(), listener);
    for (auto& item : _listeners)
    {
        if (item.second == listener)
        {
            _listeners.erase(item.first);
        }
    }

    listener->onUnregistered(*this);
}

namespace
{
template <typename KeyType>
RecordingEndpoint::IRecordingEvents* findListener(
    concurrency::MpmcHashmap32<KeyType, RecordingEndpoint::IRecordingEvents*>& map,
    const KeyType& key)
{
    auto it = map.find(key);
    if (it != map.cend())
    {
        return it->second;
    }
    return nullptr;
}
} // namespace

void RecordingEndpoint::dispatchReceivedPacket(const SocketAddress& srcAddress, memory::UniquePacket packet)
{
    if (recp::isRecControlPacket(packet->get(), packet->getLength()))
    {
        auto recControlPacket = recp::RecControlHeader::fromPacket(*packet);
        if (recControlPacket)
        {
            auto listener = findListener(_listeners, srcAddress);
            if (listener)
            {
                listener->onRecControlReceived(*this, srcAddress, _socket.getBoundPort(), std::move(packet));
                return;
            }
        }
    }

    logger::info("Unexpected packet from %s", _name.c_str(), srcAddress.toString().c_str());
    // unexpected packet that can come from anywhere. We do not log as it facilitates DoS
}

void RecordingEndpoint::registerRecordingListener(const SocketAddress& srcAddress, IRecordingEvents* listener)
{
    auto listenerIt = _listeners.find(srcAddress);
    if (listenerIt != _listeners.end())
    {
        // src port is re-used. Unregister will look at listener pointer
        listenerIt->second = listener;
    }
    else
    {
        _listeners.emplace(srcAddress, listener);
    }
}

void RecordingEndpoint::unregisterRecordingListener(IRecordingEvents* listener)
{
    if (!_receiveJobs.addJob<UnRegisterRecordingListenerJob>(*this, listener))
    {
        logger::error("failed to post unregister job", _name.c_str());
    }
}
} // namespace transport
