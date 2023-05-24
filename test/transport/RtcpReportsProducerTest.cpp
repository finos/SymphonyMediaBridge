#include "mocks/TimeSourceMock.h"
#include "rtp/RtcpFeedback.h"
#include "rtp/RtcpHeader.h"
#include "rtp/RtpHeader.h"
#include "transport/RtcpReportProducer.h"
#include <gmock/gmock.h>

using namespace transport;
using namespace testing;

namespace
{

constexpr uint32_t SSRC_0 = 12345;

class PacketMatcher : public MatcherInterface<const memory::UniquePacket&>
{
public:
    PacketMatcher(const memory::Packet& expected) : _expectedPacket(expected) {}

    bool MatchAndExplain(const memory::UniquePacket& packet, MatchResultListener* listener) const override
    {
        if (packet->getLength() != _expectedPacket.getLength())
        {
            return false;
        }

        if (!rtp::CompoundRtcpPacket::isValid(packet->get(), packet->getLength()))
        {
            return false;
        }

        memory::Packet packetCopy(*packet);

        auto compRtcp = rtp::CompoundRtcpPacket(packetCopy.get(), packetCopy.getLength());
        for (auto& report : compRtcp)
        {
            if (report.packetType == rtp::RtcpPacketType::SENDER_REPORT)
            {
                auto sendReport = rtp::RtcpSenderReport::fromPtr(&report, report.size());
                for (uint32_t i = 0; i < sendReport->header.fmtCount; ++i)
                {
                    sendReport->reportBlocks[i].interarrivalJitter = 0;
                }
            }
            else if (report.packetType == rtp::RtcpPacketType::RECEIVER_REPORT)
            {
                auto rcvReport = rtp::RtcpReceiverReport::fromPtr(&report, report.size());
                for (uint32_t i = 0; i < rcvReport->header.fmtCount; ++i)
                {
                    rcvReport->reportBlocks[i].interarrivalJitter = 0;
                }
            }
        }

        return (0 == std::memcmp(packetCopy.get(), _expectedPacket.get(), packetCopy.getLength()));
    }

    void DescribeTo(std::ostream* os) const override
    {
        std::string s;
        s.resize(_expectedPacket.getLength());
        std::memcpy(&s.front(), _expectedPacket.get(), _expectedPacket.getLength());
        *os << "equals to: " << ::testing::PrintToString(s);
    }

    void DescribeNegationTo(std::ostream* os) const override
    {
        std::string s;
        s.resize(_expectedPacket.getLength());
        std::memcpy(&s.front(), _expectedPacket.get(), _expectedPacket.getLength());
        *os << "not equal to " << ::testing::PrintToString(s);
    }

private:
    memory::Packet _expectedPacket;
};

struct PacketGenerator
{
    PacketGenerator(memory::PacketPoolAllocator& packetPoolAllocator, uint32_t ssrc, uint32_t frequency)
        : packetPoolAllocator(packetPoolAllocator),
          ssrc(ssrc),
          frequency(frequency),
          timestamp(0),
          seq(0)
    {
    }

    void incrementTime(uint64_t time) { timestamp = timestamp + static_cast<uint32_t>(time * frequency) / 1000; }

    memory::UniquePacket generatePacket(size_t payloadSize, uint64_t timeIncrement)
    {
        timestamp = timestamp + static_cast<uint32_t>(timeIncrement * frequency) / 1000;

        auto packet = memory::makeUniquePacket(packetPoolAllocator);
        auto rtpHeader = rtp::RtpHeader::create(*packet);
        rtpHeader->timestamp = timestamp;

        packet->setLength(rtpHeader->headerLength() + payloadSize);
        return packet;
    }

    void generateAndUpdateSenderState(RtpSenderState& senderState,
        uint32_t packetCount,
        uint64_t initialTimestamp,
        uint64_t timeIncrement,
        uint32_t maxPacketSize = 1200)
    {
        uint64_t timestamp = initialTimestamp;
        for (uint32_t i = 0; i < packetCount; ++i)
        {
            uint32_t bytesToSend = 50 + ((i * 250) % maxPacketSize);
            timestamp += timeIncrement;
            senderState.onRtpSent(timestamp, *generatePacket(bytesToSend, timeIncrement));
            stats.lastTimestamp = timestamp;
            stats.octets += bytesToSend;
            stats.packetCount++;
        }
    }

    void generateAndUpdateReceiverState(RtpReceiveState& receiverState,
        uint32_t packetCount,
        uint64_t initialTimestamp,
        uint64_t timeIncrement,
        uint32_t maxPacketSize = 1200)
    {
        uint64_t timestamp = initialTimestamp;
        for (uint32_t i = 0; i < packetCount; ++i)
        {
            uint32_t bytesToSend = 50 + ((i * 250) % maxPacketSize);
            timestamp += timeIncrement;
            receiverState.onRtpReceived(*generatePacket(bytesToSend, timeIncrement), timestamp);
            stats.lastTimestamp = timestamp;
            stats.octets += bytesToSend;
            stats.packetCount++;
        }
    }

    memory::PacketPoolAllocator& packetPoolAllocator;
    uint32_t ssrc;
    uint32_t frequency;
    uint32_t timestamp;
    uint16_t seq;
    struct
    {
        uint64_t lastTimestamp = 0;
        uint32_t octets = 0;
        uint32_t packetCount = 0;
    } stats;
};

struct RtcpPacketBuilder
{
    template <class TRtcp>
    RtcpPacketBuilder& append(const TRtcp& rtcp)
    {
        if (_packet.getLength() + rtcp.size() > _packet.maxLength())
        {
            throw std::logic_error("memory::Packet size exceeded");
        }

        _packet.append(reinterpret_cast<const char*>(&rtcp), rtcp.size());
        return *this;
    }

private:
    memory::Packet _packet;
};

struct RtcpSenderMock : public RtcpReportProducer::RtcpSender
{
    MOCK_METHOD(void, sendRtcpInternal, (const memory::UniquePacket& packet, uint64_t timestamp));

    void sendRtcp(memory::UniquePacket packet, uint64_t timestamp) override { sendRtcpInternal(packet, timestamp); }
};

::testing::Matcher<const memory::UniquePacket&> packetEq(const memory::Packet& expected)
{
    return ::testing::MakeMatcher(new PacketMatcher(expected));
}

rtp::RtcpSenderReport* makeRtpSenderReport(memory::Packet& packet,
    uint32_t packetCount,
    uint32_t octetCount,
    uint32_t timestamp,
    std::chrono::system_clock::time_point wallClock)
{
    const auto ntp = utils::Time::toNtp(wallClock);
    auto* report = rtp::RtcpSenderReport::create(packet.get());
    report->ssrc = SSRC_0;
    report->packetCount = packetCount;
    report->octetCount = octetCount;
    report->rtpTimestamp = timestamp;
    report->ntpSeconds = static_cast<uint32_t>(ntp >> 32);
    report->ntpFractions = static_cast<uint32_t>(ntp & 0xFFFFFFFF);
    packet.setLength(report->size());

    return report;
}

rtp::RtcpReceiverReport* makeRtpReceiveReport(memory::Packet& packet, uint32_t ssrc)
{
    auto* report = rtp::RtcpReceiverReport::create(packet.get());
    report->ssrc = ssrc;
    packet.setLength(report->header.size());

    return report;
}

rtp::RtcpRembFeedback* makeRemb(memory::Packet& packet,
    const uint64_t timestamp,
    uint32_t senderSsrc,
    uint64_t mediaBps)
{
    auto& remb = rtp::RtcpRembFeedback::create(packet.get(), senderSsrc);
    remb.setBitrate(mediaBps);
    return &remb;
}

rtp::ReportBlock makeEmptyReportBlockFor(uint32_t ssrc)
{
    rtp::ReportBlock block;
    std::memset(&block, 0, sizeof(block));
    block.ssrc = ssrc;
    return block;
}

} // namespace

class RtcpReportsProducerTest : public ::testing::Test
{

public:
    RtcpReportsProducerTest()
        : _loggableId("RtcpReportsProducerTest", 0),
          _packetAllocator(4069, "main"),
          _outboundSsrcCounters(128),
          _inboundSsrcCounters(128)
    {
        _config.readFromString(
            R"({"rtcp.senderReport.interval": 600000000, "rtcp.senderReport.resubmitInterval": 7000000000})");
    }

protected:
    void SetUp() override
    {
        _outboundSsrcCounters.clear();
        _inboundSsrcCounters.clear();

        _rtcpSenderMock = std::make_unique<RtcpSenderMock>();
        _timeSourceMock = std::make_unique<NiceMock<test::TimeSourceMock>>();

        utils::Time::initialize(*_timeSourceMock);
    }

    void TearDown() override { utils::Time::initialize(); }

    uint64_t getConfiguredInterval() const { return _config.rtcp.senderReport.interval; }

    RtcpReportProducer createReportProducer()
    {
        return RtcpReportProducer(_loggableId,
            _config,
            _outboundSsrcCounters,
            _inboundSsrcCounters,
            _packetAllocator,
            *_rtcpSenderMock);
    }

    PacketGenerator createPacketGenerator(uint32_t ssrc, uint32_t frequency)
    {
        return PacketGenerator(_packetAllocator, ssrc, frequency);
    }

    void simulateReceiving(std::vector<PacketGenerator>& allSsrcPacketReceivers,
        uint32_t packetCount,
        uint64_t initialTimestamp,
        uint64_t timeIncrement)
    {
        for (auto& ssrcPacketGenerator : allSsrcPacketReceivers)
        {
            auto it = _inboundSsrcCounters.find(ssrcPacketGenerator.ssrc);
            if (it == _inboundSsrcCounters.end())
            {
                throw std::logic_error("Inbound ssrc not found");
            }

            ssrcPacketGenerator.generateAndUpdateReceiverState(it->second,
                packetCount,
                initialTimestamp,
                timeIncrement);
        }
    }

protected:
    std::unique_ptr<test::TimeSourceMock> _timeSourceMock;
    std::unique_ptr<RtcpSenderMock> _rtcpSenderMock;
    logger::LoggableId _loggableId;
    memory::PacketPoolAllocator _packetAllocator;
    config::Config _config;
    concurrency::MpmcHashmap32<uint32_t, RtpSenderState> _outboundSsrcCounters;
    concurrency::MpmcHashmap32<uint32_t, RtpReceiveState> _inboundSsrcCounters;
};

TEST_F(RtcpReportsProducerTest, shouldNotSendAfterInterval)
{
    uint64_t time = 0;
    auto& senderState = _outboundSsrcCounters.emplace(SSRC_0, 48000, _config).first->second;

    const std::chrono::system_clock::time_point wallClock(std::chrono::duration<long>(0xFF001122));

    ON_CALL(*_timeSourceMock, wallClock()).WillByDefault(Return(wallClock));

    auto packetGenerator = createPacketGenerator(SSRC_0, 48000);
    senderState.onRtpSent(time, *packetGenerator.generatePacket(1200, time));
    const auto timestamp = time + getConfiguredInterval();

    EXPECT_CALL(*_rtcpSenderMock, sendRtcpInternal(_, _)).Times(0);

    auto rtcpReportsProducer = createReportProducer();
    const bool rembSent = rtcpReportsProducer.sendReports(timestamp, utils::Optional<uint64_t>());
    ASSERT_EQ(false, rembSent);
}

TEST_F(RtcpReportsProducerTest, shouldSendAfterIntervalWhen5PacketsSent)
{
    const uint64_t timeIncrement = 20 * utils::Time::ms;
    auto& senderState = _outboundSsrcCounters.emplace(SSRC_0, 48000, _config).first->second;

    const uint64_t initialTimestamp = 0xFF886622;
    const std::chrono::system_clock::time_point wallClock(std::chrono::duration<long>(0xFF001122));

    auto reportBlock = makeEmptyReportBlockFor(SSRC_0);
    reportBlock.lastSR = 0x00000010 + 0x00000010;
    reportBlock.delaySinceLastSR = 0x00000010;
    senderState.onReceiverBlockReceived(initialTimestamp, utils::Time::absToNtp32(initialTimestamp), reportBlock);

    ON_CALL(*_timeSourceMock, wallClock()).WillByDefault(Return(wallClock));

    auto packetGenerator = createPacketGenerator(SSRC_0, 48000);
    packetGenerator.generateAndUpdateSenderState(senderState, 5, initialTimestamp, timeIncrement);

    const auto lastPacketTimestamp = packetGenerator.stats.lastTimestamp;
    const auto timestamp = initialTimestamp + getConfiguredInterval();

    const auto rtpTimestamp = packetGenerator.timestamp +
        static_cast<uint32_t>(((timestamp - lastPacketTimestamp) / utils::Time::ms) * 48000 / 1000);

    memory::Packet expectedPacket;
    makeRtpSenderReport(expectedPacket,
        packetGenerator.stats.packetCount,
        packetGenerator.stats.octets,
        rtpTimestamp,
        wallClock);

    EXPECT_CALL(*_rtcpSenderMock, sendRtcpInternal(packetEq(expectedPacket), Eq(timestamp)));

    auto rtcpReportsProducer = createReportProducer();
    const bool rembSent = rtcpReportsProducer.sendReports(timestamp, utils::Optional<uint64_t>());
    ASSERT_EQ(false, rembSent);
}

TEST_F(RtcpReportsProducerTest, shouldNotSendAfterIntervalWhenLessThan5PacketsSent)
{
    const uint64_t timeIncrement = 20 * utils::Time::ms;
    auto& senderState = _outboundSsrcCounters.emplace(SSRC_0, 48000, _config).first->second;

    const uint64_t initialTimestamp = 0xFF886622;
    auto timestamp = initialTimestamp;
    const std::chrono::system_clock::time_point wallClock(std::chrono::duration<long>(0xFF001122));

    auto reportBlock = makeEmptyReportBlockFor(SSRC_0);
    reportBlock.lastSR = 0x00000010 + 0x00000010;
    reportBlock.delaySinceLastSR = 0x00000010;
    senderState.onReceiverBlockReceived(timestamp, utils::Time::absToNtp32(timestamp), reportBlock);

    ON_CALL(*_timeSourceMock, wallClock()).WillByDefault(Return(wallClock));

    // Send 4 packets only
    auto packetGenerator = createPacketGenerator(SSRC_0, 48000);
    packetGenerator.generateAndUpdateSenderState(senderState, 4, initialTimestamp, timeIncrement);

    // Advance timestamp a lot
    timestamp = initialTimestamp + getConfiguredInterval() * 10;

    EXPECT_CALL(*_rtcpSenderMock, sendRtcpInternal(_, _)).Times(0);

    auto rtcpReportsProducer = createReportProducer();
    ASSERT_EQ(false, rtcpReportsProducer.sendReports(timestamp, utils::Optional<uint64_t>()));
}

TEST_F(RtcpReportsProducerTest, shouldSendReceiveReportsWithinSenderReport)
{
    constexpr uint32_t packetsToSendCount = 30;
    constexpr uint32_t packetsToReceivePerSsrc = 15;
    constexpr uint32_t receiveSsrcCount = 15;
    constexpr uint32_t firstReceiverSSrc = 40000;
    const uint64_t timeIncrement = 20 * utils::Time::ms;

    std::vector<PacketGenerator> inboundSsrcPacketGenerators;

    auto& senderState = _outboundSsrcCounters.emplace(SSRC_0, 48000, _config).first->second;

    for (uint32_t i = 0; i < receiveSsrcCount; ++i)
    {
        _inboundSsrcCounters.emplace(firstReceiverSSrc + i, _config, 48000);
        inboundSsrcPacketGenerators.push_back(createPacketGenerator(firstReceiverSSrc + i, 48000));
    }

    const uint64_t initialTimestamp = 0xFF886622;
    const std::chrono::system_clock::time_point wallClock(std::chrono::duration<long>(0xFF001122));

    auto reportBlock = makeEmptyReportBlockFor(SSRC_0);
    reportBlock.lastSR = 0x00000010 + 0x00000010;
    reportBlock.delaySinceLastSR = 0x00000010;
    senderState.onReceiverBlockReceived(initialTimestamp, utils::Time::absToNtp32(initialTimestamp), reportBlock);

    ON_CALL(*_timeSourceMock, wallClock()).WillByDefault(Return(wallClock));

    auto senderPacketGenerator = createPacketGenerator(SSRC_0, 48000);

    senderPacketGenerator.generateAndUpdateSenderState(senderState,
        packetsToSendCount,
        initialTimestamp,
        timeIncrement);

    simulateReceiving(inboundSsrcPacketGenerators, packetsToReceivePerSsrc, initialTimestamp, timeIncrement);

    const auto heightReceiveTimestamp = inboundSsrcPacketGenerators.front().stats.lastTimestamp;
    const auto timestamp = std::max(heightReceiveTimestamp + timeIncrement, initialTimestamp + getConfiguredInterval());

    const auto rtpTimestamp = senderPacketGenerator.timestamp +
        static_cast<uint32_t>(
            ((timestamp - senderPacketGenerator.stats.lastTimestamp) / utils::Time::ms) * 48000 / 1000);

    memory::Packet expectedPacket;
    auto* senderReport = makeRtpSenderReport(expectedPacket,
        senderPacketGenerator.stats.packetCount,
        senderPacketGenerator.stats.octets,
        rtpTimestamp,
        wallClock);

    // Reports are generated by reverse order of MpmcHashmap32 container
    auto beginReverseIt = std::make_reverse_iterator(_inboundSsrcCounters.end());
    auto endReverseIt = std::make_reverse_iterator(_inboundSsrcCounters.begin());
    for (auto it = beginReverseIt; it != endReverseIt; ++it)
    {
        const auto snapshot = it->second.getCumulativeSnapshot();
        auto& reportBlock = senderReport->addReportBlock(it->first);
        reportBlock.ssrc = it->first;
        reportBlock.extendedSeqNoReceived = snapshot.extendedSequenceNumber;
        reportBlock.lastSR = 0;
        reportBlock.delaySinceLastSR = 0;
        reportBlock.interarrivalJitter = 0;
    }

    expectedPacket.setLength(senderReport->size());

    EXPECT_CALL(*_rtcpSenderMock, sendRtcpInternal(packetEq(expectedPacket), Eq(timestamp)));

    auto rtcpReportsProducer = createReportProducer();
    ASSERT_EQ(false, rtcpReportsProducer.sendReports(timestamp, utils::Optional<uint64_t>()));
}

TEST_F(RtcpReportsProducerTest, shouldSendSendReportsAndReceive)
{
    constexpr uint32_t packetsToSendCount = 30;
    constexpr uint32_t packetsToReceivePerSsrc = 15;
    constexpr uint32_t receiveSsrcCount = 63;
    constexpr uint32_t firstReceiverSSrc = 40000;
    const uint64_t timeIncrement = 20 * utils::Time::ms;

    std::vector<PacketGenerator> inboundSsrcPacketGenerators;

    auto& senderState = _outboundSsrcCounters.emplace(SSRC_0, 48000, _config).first->second;

    for (uint32_t i = 0; i < receiveSsrcCount; ++i)
    {
        _inboundSsrcCounters.emplace(firstReceiverSSrc + i, _config, 48000);
        inboundSsrcPacketGenerators.push_back(createPacketGenerator(firstReceiverSSrc + i, 48000));
    }

    const uint64_t initialTimestamp = 0xFF886622;
    const std::chrono::system_clock::time_point wallClock(std::chrono::duration<long>(0xFF001122));

    auto reportBlock = makeEmptyReportBlockFor(SSRC_0);
    reportBlock.lastSR = 0x00000010 + 0x00000010;
    reportBlock.delaySinceLastSR = 0x00000010;
    senderState.onReceiverBlockReceived(initialTimestamp, utils::Time::absToNtp32(initialTimestamp), reportBlock);

    ON_CALL(*_timeSourceMock, wallClock()).WillByDefault(Return(wallClock));

    auto senderPacketGenerator = createPacketGenerator(SSRC_0, 48000);

    senderPacketGenerator.generateAndUpdateSenderState(senderState,
        packetsToSendCount,
        initialTimestamp,
        timeIncrement);

    simulateReceiving(inboundSsrcPacketGenerators, packetsToReceivePerSsrc, initialTimestamp, timeIncrement);

    const auto heightReceiveTimestamp = inboundSsrcPacketGenerators.front().stats.lastTimestamp;
    const auto timestamp = std::max(heightReceiveTimestamp + timeIncrement, initialTimestamp + getConfiguredInterval());

    const auto rtpTimestamp = senderPacketGenerator.timestamp +
        static_cast<uint32_t>(
            ((timestamp - senderPacketGenerator.stats.lastTimestamp) / utils::Time::ms) * 48000 / 1000);

    memory::Packet packet1;
    memory::Packet packet2;
    memory::Packet packet3;

    auto* senderReport = makeRtpSenderReport(packet1,
        senderPacketGenerator.stats.packetCount,
        senderPacketGenerator.stats.octets,
        rtpTimestamp,
        wallClock);

    // Reports are generated by reverse order of MpmcHashmap32 container
    auto beginReverseIt = std::make_reverse_iterator(_inboundSsrcCounters.end());
    auto endReverseIt = std::make_reverse_iterator(_inboundSsrcCounters.begin());
    auto currentIt = beginReverseIt;
    for (size_t i = 0; i < 31; ++i)
    {
        const auto snapshot = currentIt->second.getCumulativeSnapshot();
        auto& reportBlock = senderReport->addReportBlock(currentIt->first);
        reportBlock.ssrc = currentIt->first;
        reportBlock.extendedSeqNoReceived = snapshot.extendedSequenceNumber;
        reportBlock.lastSR = 0;
        reportBlock.delaySinceLastSR = 0;
        reportBlock.interarrivalJitter = 0;

        ++currentIt;
    }

    packet1.setLength(senderReport->size());

    const auto receiverReportSsrc = _outboundSsrcCounters.begin()->first;
    auto* receiverReport = makeRtpReceiveReport(packet2, receiverReportSsrc);

    // On second we only have space to 29 blocks
    for (size_t i = 0; i < 29; ++i)
    {
        const auto snapshot = currentIt->second.getCumulativeSnapshot();
        auto& reportBlock = receiverReport->addReportBlock(currentIt->first);
        reportBlock.ssrc = currentIt->first;
        reportBlock.extendedSeqNoReceived = snapshot.extendedSequenceNumber;
        reportBlock.lastSR = 0;
        reportBlock.delaySinceLastSR = 0;
        reportBlock.interarrivalJitter = 0;

        ++currentIt;
    }

    packet2.setLength(receiverReport->header.size());

    receiverReport = makeRtpReceiveReport(packet3, receiverReportSsrc);
    for (; currentIt != endReverseIt; ++currentIt)
    {
        const auto snapshot = currentIt->second.getCumulativeSnapshot();
        auto& reportBlock = receiverReport->addReportBlock(currentIt->first);
        reportBlock.ssrc = currentIt->first;
        reportBlock.extendedSeqNoReceived = snapshot.extendedSequenceNumber;
        reportBlock.lastSR = 0;
        reportBlock.delaySinceLastSR = 0;
        reportBlock.interarrivalJitter = 0;
    }

    packet3.setLength(receiverReport->header.size());

    memory::Packet compoundRtcp;
    compoundRtcp.append(packet1);
    compoundRtcp.append(packet2);

    {
        InSequence seq;

        EXPECT_CALL(*_rtcpSenderMock, sendRtcpInternal(packetEq(compoundRtcp), Eq(timestamp)));
        EXPECT_CALL(*_rtcpSenderMock, sendRtcpInternal(packetEq(packet3), Eq(timestamp)));
    }

    auto rtcpReportsProducer = createReportProducer();
    ASSERT_EQ(false, rtcpReportsProducer.sendReports(timestamp, utils::Optional<uint64_t>()));
}

TEST_F(RtcpReportsProducerTest, shouldSendRembOnThe1stPacket)
{
    constexpr uint32_t packetsToSendCount = 30;
    constexpr uint32_t packetsToReceivePerSsrc = 15;
    constexpr uint32_t receiveSsrcCount = 63;
    constexpr uint32_t firstReceiverSSrc = 40000;
    const uint64_t timeIncrement = 20 * utils::Time::ms;

    std::vector<PacketGenerator> inboundSsrcPacketGenerators;

    auto& senderState = _outboundSsrcCounters.emplace(SSRC_0, 48000, _config).first->second;

    for (uint32_t i = 0; i < receiveSsrcCount; ++i)
    {
        _inboundSsrcCounters.emplace(firstReceiverSSrc + i, _config, 48000);
        inboundSsrcPacketGenerators.push_back(createPacketGenerator(firstReceiverSSrc + i, 48000));
    }

    const uint64_t initialTimestamp = 0xFF886622;
    const std::chrono::system_clock::time_point wallClock(std::chrono::duration<long>(0xFF001122));

    auto reportBlock = makeEmptyReportBlockFor(SSRC_0);
    reportBlock.lastSR = 0x00000010 + 0x00000010;
    reportBlock.delaySinceLastSR = 0x00000010;
    senderState.onReceiverBlockReceived(initialTimestamp, utils::Time::absToNtp32(initialTimestamp), reportBlock);

    ON_CALL(*_timeSourceMock, wallClock()).WillByDefault(Return(wallClock));

    auto senderPacketGenerator = createPacketGenerator(SSRC_0, 48000);

    senderPacketGenerator.generateAndUpdateSenderState(senderState,
        packetsToSendCount,
        initialTimestamp,
        timeIncrement);

    simulateReceiving(inboundSsrcPacketGenerators, packetsToReceivePerSsrc, initialTimestamp, timeIncrement);

    const auto heightReceiveTimestamp = inboundSsrcPacketGenerators.front().stats.lastTimestamp;
    const auto timestamp = std::max(heightReceiveTimestamp + timeIncrement, initialTimestamp + getConfiguredInterval());

    const auto rtpTimestamp = senderPacketGenerator.timestamp +
        static_cast<uint32_t>(
            ((timestamp - senderPacketGenerator.stats.lastTimestamp) / utils::Time::ms) * 48000 / 1000);

    const auto receiverReportSsrc = _outboundSsrcCounters.begin()->first;
    const uint64_t mediaBps = 0x00112233;

    memory::Packet packet1;
    memory::Packet packet2;
    memory::Packet packet3;

    auto* senderReport = makeRtpSenderReport(packet1,
        senderPacketGenerator.stats.packetCount,
        senderPacketGenerator.stats.octets,
        rtpTimestamp,
        wallClock);

    // Reports are generated by reverse order of MpmcHashmap32 container
    auto beginReverseIt = std::make_reverse_iterator(_inboundSsrcCounters.end());
    auto currentIt = beginReverseIt;
    for (size_t i = 0; i < 31; ++i)
    {
        const auto snapshot = currentIt->second.getCumulativeSnapshot();
        auto& reportBlock = senderReport->addReportBlock(currentIt->first);
        reportBlock.ssrc = currentIt->first;
        reportBlock.extendedSeqNoReceived = snapshot.extendedSequenceNumber;
        reportBlock.lastSR = 0;
        reportBlock.delaySinceLastSR = 0;
        reportBlock.interarrivalJitter = 0;

        ++currentIt;
    }

    packet1.setLength(senderReport->size());

    auto* remb = makeRemb(packet2, timestamp, receiverReportSsrc, mediaBps);
    for (auto& kv : _inboundSsrcCounters)
    {
        remb->addSsrc(kv.first);
    }

    packet2.setLength(remb->header.size());

    auto* receiverReport = makeRtpReceiveReport(packet3, receiverReportSsrc);

    // On third we only have space to 17 blocks
    for (size_t i = 0; i < 17; ++i)
    {
        const auto snapshot = currentIt->second.getCumulativeSnapshot();
        auto& reportBlock = receiverReport->addReportBlock(currentIt->first);
        reportBlock.ssrc = currentIt->first;
        reportBlock.extendedSeqNoReceived = snapshot.extendedSequenceNumber;
        reportBlock.lastSR = 0;
        reportBlock.delaySinceLastSR = 0;
        reportBlock.interarrivalJitter = 0;

        ++currentIt;
    }

    packet3.setLength(receiverReport->header.size());

    memory::Packet compoundRtcp;
    compoundRtcp.append(packet1);
    compoundRtcp.append(packet2);
    compoundRtcp.append(packet3);

    {
        InSequence seq;

        EXPECT_CALL(*_rtcpSenderMock, sendRtcpInternal(packetEq(compoundRtcp), Eq(timestamp)));
        EXPECT_CALL(*_rtcpSenderMock, sendRtcpInternal(_, _));
    }

    auto rtcpReportsProducer = createReportProducer();
    ASSERT_EQ(true, rtcpReportsProducer.sendReports(timestamp, utils::Optional<uint64_t>(mediaBps)));
}
