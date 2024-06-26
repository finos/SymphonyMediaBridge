#pragma once

#include "transport/ice/IceSession.h"
#include <gmock/gmock.h>
#include <memory>

namespace test
{

struct IceSessionEventListenerMock : public ::ice::IceSession::IEvents
{
    MOCK_METHOD(void, onIceStateChanged, (::ice::IceSession * session, ::ice::IceSession::State state), (override));

    MOCK_METHOD(void, onIceCompleted, (::ice::IceSession * session), (override));

    MOCK_METHOD(void,
        onIceCandidateChanged,
        (::ice::IceSession * session, ::ice::IceEndpoint* localEndpoint, const ::transport::SocketAddress& remotePort),
        (override));

    MOCK_METHOD(void,
        onIceCandidateAccepted,
        (::ice::IceSession * session, ::ice::IceEndpoint* localEndpoint, const ::ice::IceCandidate& remoteCandidate),
        (override));

    MOCK_METHOD(void,
        onIceDiscardCandidate,
        (::ice::IceSession * session, ::ice::IceEndpoint* localEndpoint, const ::transport::SocketAddress& remotePort),
        (override));
};

} // namespace test
