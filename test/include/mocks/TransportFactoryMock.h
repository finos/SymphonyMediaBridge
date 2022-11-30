#pragma once

#include "transport/TransportFactory.h"
#include <gmock/gmock.h>

namespace test
{

class TransportFactoryMock : public transport::TransportFactory
{
public:
    MOCK_METHOD(std::shared_ptr<transport::RtcTransport>,
        create,
        (const ice::IceRole iceRole, const size_t sendPoolSize, const size_t endpointId),
        (override));

    MOCK_METHOD(std::shared_ptr<transport::RtcTransport>,
        create,
        (const size_t sendPoolSize, const size_t endpointIdHash),
        (override));

    MOCK_METHOD(std::shared_ptr<transport::RtcTransport>,
        createOnSharedPort,
        (const ice::IceRole iceRole, const size_t sendPoolSize, const size_t endpointIdHash),
        (override));

    MOCK_METHOD(std::shared_ptr<transport::RtcTransport>,
        createOnPrivatePort,
        (const ice::IceRole iceRole, const size_t sendPoolSize, const size_t endpointIdHash),
        (override));

    MOCK_METHOD(std::unique_ptr<transport::RecordingTransport>,
        createForRecording,
        (const size_t endpointHashId,
            const size_t streamHashId,
            const transport::SocketAddress& peer,
            const uint8_t aesKey[32],
            const uint8_t salt[12]),
        (override));

    MOCK_METHOD(EndpointMetrics, getSharedUdpEndpointsMetrics, (), (const override));
    MOCK_METHOD(bool, isGood, (), (const override));

    MOCK_METHOD(std::shared_ptr<transport::RtcTransport>,
        createOnPorts,
        (const ice::IceRole iceRole,
            const size_t sendPoolSize,
            const size_t endpointIdHash,
            const transport::Endpoints& rtpPorts,
            size_t expectedInboundStreamCount,
            size_t expectedOutboundStreamCount,
            bool enableUplinkEstimation,
            bool enableDownlinkEstimation),
        (override));

    MOCK_METHOD(bool, openRtpMuxPorts, (transport::Endpoints & rtpPorts, uint32_t maxSessions), (const override));

    MOCK_METHOD(void, maintenance, (uint64_t timestamp), (override));

    MOCK_METHOD(void, registerIceListener, (transport::Endpoint::IEvents&, const std::string& ufrag), (override));
    MOCK_METHOD(void, registerIceListener, (transport::ServerEndpoint::IEvents&, const std::string& ufrag), (override));
    MOCK_METHOD(void, unregisterIceListener, (transport::Endpoint::IEvents&, const std::string& ufrag), (override));
    MOCK_METHOD(void,
        unregisterIceListener,
        (transport::ServerEndpoint::IEvents&, const std::string& ufrag),
        (override));

    void willReturnByDefaultForAll(const std::shared_ptr<transport::RtcTransport>& rtcTransport)
    {
        using namespace ::testing;

        ON_CALL(*this, create(_, _)).WillByDefault(Return(rtcTransport));
        ON_CALL(*this, create(_, _, _)).WillByDefault(Return(rtcTransport));
        ON_CALL(*this, createOnSharedPort(_, _, _)).WillByDefault(Return(rtcTransport));
        ON_CALL(*this, createOnPrivatePort(_, _, _)).WillByDefault(Return(rtcTransport));
    }

    void willReturnByDefaultForAllWeakly(const std::weak_ptr<transport::RtcTransport>& rtcTransport)
    {
        using namespace ::testing;

        const auto callback2Args = [=](const auto&, const auto&) {
            return rtcTransport.lock();
        };

        const auto callback3Args = [=](const auto&, const auto&, const auto&) {
            return rtcTransport.lock();
        };

        ON_CALL(*this, create(_, _)).WillByDefault(callback2Args);
        ON_CALL(*this, create(_, _, _)).WillByDefault(callback3Args);
        ON_CALL(*this, createOnSharedPort(_, _, _)).WillByDefault(callback3Args);
        ON_CALL(*this, createOnPrivatePort(_, _, _)).WillByDefault(callback3Args);
    }
};

} // namespace test