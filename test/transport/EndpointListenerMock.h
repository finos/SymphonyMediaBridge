#pragma once

#include "transport/Endpoint.h"
#include <gmock/gmock.h>

using namespace transport;

namespace fakenet
{

struct EndpointListenerMock : public Endpoint::IEvents
{
    MOCK_METHOD(void,
        onRtpReceived,
        (Endpoint & endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::Packet* packet,
            memory::PacketPoolAllocator& allocator),
        (override));

    MOCK_METHOD(void,
        onDtlsReceived,
        (Endpoint & endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::Packet* packet,
            memory::PacketPoolAllocator& allocator),
        (override));

    MOCK_METHOD(void,
        onRtcpReceived,
        (Endpoint & endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::Packet* packet,
            memory::PacketPoolAllocator& allocator),
        (override));

    MOCK_METHOD(void,
        onIceReceived,
        (Endpoint & endpoint,
            const SocketAddress& source,
            const SocketAddress& target,
            memory::Packet* packet,
            memory::PacketPoolAllocator& allocator),
        (override));

    MOCK_METHOD(void, onPortClosed, (Endpoint & endpoint), (override));
    MOCK_METHOD(void, onUnregistered, (Endpoint & endpoint), (override));
};

} // namespace fakenet
