#pragma once
#include "transport/Endpoint.h"
#include "transport/RtcePoll.h"

namespace transport
{

class TcpEndpoint : public Endpoint, public RtcePoll::IEventListener
{
public:
    virtual void internalStopped() = 0;

    ice::TransportType getTransportType() const override { return ice::TransportType::TCP; }
};

} // namespace transport
