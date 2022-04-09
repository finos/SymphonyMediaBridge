#include "bwe/BandwidthEstimator.h"
#include "bwe/RateController.h"
#include "config/Config.h"
#include "jobmanager/WorkerThread.h"
#include "memory/PacketPoolAllocator.h"
#include "transport/DataReceiver.h"
#include "transport/RtcePoll.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SslDtls.h"
#include "transport/ice/IceSession.h"
#include "transport/sctp/SctpConfig.h"
#include "utils/Pacer.h"
#include <cstdint>
#include <gtest/gtest.h>

namespace transport
{
class RecordingTransport;
class RtcTransport;
} // namespace transport

using namespace testing;

struct TransportIntegrationTest : public ::testing::Test
{
    memory::PacketPoolAllocator _sendAllocator;
    config::Config _config1;
    config::Config _config2;
    std::unique_ptr<jobmanager::JobManager> _jobManager;
    std::unique_ptr<memory::PacketPoolAllocator> _mainPoolAllocator;
    std::unique_ptr<transport::SslDtls> _sslDtls;
    std::unique_ptr<transport::SrtpClientFactory> _srtpClientFactory;
    std::vector<std::unique_ptr<jobmanager::WorkerThread>> _workerThreads;
    std::unique_ptr<transport::RtcePoll> _network;
    ice::IceConfig _iceConfig;
    sctp::SctpConfig _sctpConfig;
    bwe::Config _bweConfig;
    bwe::RateControllerConfig _rateControlConfig;
    utils::Pacer _pacer;
    std::unique_ptr<transport::TransportFactory> _transportFactory1;
    std::unique_ptr<transport::TransportFactory> _transportFactory2;

    TransportIntegrationTest();

    void SetUp() override;
    bool init(std::string configJson1, std::string configJson2);
    void TearDown() override;
};

struct TransportClientPair : public transport::DataReceiver
{
    TransportClientPair(transport::TransportFactory& transportFactory1,
        transport::TransportFactory& transportFactory2,
        uint32_t ssrc,
        memory::PacketPoolAllocator& allocator,
        transport::SslDtls& sslDtls,
        jobmanager::JobManager& jobManager,
        bool blockUdp);

    ~TransportClientPair();
    void connect(uint64_t timestamp);

    int64_t tryConnect(const uint64_t timestamp, const transport::SslDtls& sslDtls);
    bool isConnected();
    void stop();

    void onRtpPacketReceived(transport::RtcTransport* sender,
        memory::UniquePacket packet,
        const uint32_t extendedSequenceNumber,
        uint64_t timestamp) override;

    void onRtcpPacketDecoded(transport::RtcTransport* sender,
        memory::UniquePacket packet,
        const uint64_t timestamp) override
    {
    }

    void onConnected(transport::RtcTransport*) override {}
    bool onSctpConnectionRequest(transport::RtcTransport*, uint16_t remotePort) override { return true; }
    void onSctpEstablished(transport::RtcTransport* sender) override{};
    void onSctpMessage(transport::RtcTransport* sender,
        uint16_t streamId,
        uint16_t streamSequenceNumber,
        uint32_t payloadProtocol,
        const void* data,
        size_t length) override{};

    void onRecControlReceived(transport::RecordingTransport* sender,
        memory::UniquePacket packet,
        uint64_t timestamp) override{};

    uint32_t _ssrc;
    memory::PacketPoolAllocator& _sendAllocator;
    std::shared_ptr<transport::RtcTransport> _transport1;
    std::shared_ptr<transport::RtcTransport> _transport2;
    uint16_t _sequenceNumber;
    uint32_t _tickCount;
    uint64_t _connectStart;
    uint64_t _signalDelay;
    std::atomic_uint32_t _jobsCounter1;
    std::atomic_uint32_t _jobsCounter2;

    std::atomic_uint32_t _receivedByteCount;
    time_t _receiveStart;
    std::atomic_uint32_t _receivedPacketCount;

    jobmanager::JobManager& _jobManager;
    ice::IceCandidates _candidates1;
};
