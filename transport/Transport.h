#pragma once

#include "jobmanager/JobQueue.h"
#include "memory/PacketPoolAllocator.h"

namespace transport
{

class DataReceiver;

class Transport
{
public:
    virtual ~Transport() = default;

    virtual bool isInitialized() const = 0;
    virtual const logger::LoggableId& getLoggableId() const = 0;

    /** Numeric unique id. It's used instead of object's address to make
     * maps traversal order deterministic in UTs when map's key is TransportId. */
    virtual size_t getId() const = 0;

    virtual size_t getEndpointIdHash() const = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual bool hasPendingJobs() const = 0;
    virtual std::atomic_uint32_t& getJobCounter() = 0;
    virtual bool unprotect(memory::Packet& packet) = 0;
    virtual bool unprotectFirstRtp(memory::Packet& packet, uint32_t& rolloverCounter) = 0;
    virtual void setDataReceiver(DataReceiver* dataReceiver) = 0;
    virtual bool isConnected() = 0;
    virtual bool start() = 0;
    virtual void connect() = 0;
    virtual jobmanager::JobQueue& getJobQueue() = 0;
    virtual void protectAndSend(memory::UniquePacket packet) = 0;

    template <class Callable>
    bool postOnQueue(Callable&& callableFunction)
    {
        return getJobQueue().post(getJobCounter(), std::forward<Callable>(callableFunction));
    }
};

} // namespace transport
