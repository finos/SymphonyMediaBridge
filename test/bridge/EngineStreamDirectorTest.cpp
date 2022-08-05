#include "bridge/engine/EngineStreamDirector.h"
#include "utils/Time.h"
#include <gtest/gtest.h>

namespace
{

bridge::SimulcastStream makeSimulcastStream(const uint32_t ssrc0,
    const uint32_t feedback0,
    const uint32_t ssrc1,
    const uint32_t feedback1,
    const uint32_t ssrc2,
    const uint32_t feedback2)
{
    bridge::SimulcastStream simulcastStream;
    memset(&simulcastStream, 0, sizeof(bridge::SimulcastStream));
    simulcastStream._numLevels = 3;
    simulcastStream._highestActiveLevel = 2;
    simulcastStream._levels[0]._ssrc = ssrc0;
    simulcastStream._levels[0]._feedbackSsrc = feedback0;
    simulcastStream._levels[0]._mediaActive = false;
    simulcastStream._levels[1]._ssrc = ssrc1;
    simulcastStream._levels[1]._feedbackSsrc = feedback1;
    simulcastStream._levels[1]._mediaActive = false;
    simulcastStream._levels[2]._ssrc = ssrc2;
    simulcastStream._levels[2]._feedbackSsrc = feedback2;
    simulcastStream._levels[2]._mediaActive = false;
    simulcastStream._contentType = bridge::SimulcastStream::VideoContentType::VIDEO;
    return simulcastStream;
}

bridge::SimulcastStream makeSimulcastStream(const uint32_t ssrc0,
    const uint32_t feedback0,
    const bridge::SimulcastStream::VideoContentType contentType = bridge::SimulcastStream::VideoContentType::VIDEO)
{
    bridge::SimulcastStream simulcastStream;
    memset(&simulcastStream, 0, sizeof(bridge::SimulcastStream));
    simulcastStream._numLevels = 1;
    simulcastStream._levels[0]._ssrc = ssrc0;
    simulcastStream._levels[0]._feedbackSsrc = feedback0;
    simulcastStream._levels[0]._mediaActive = false;
    simulcastStream._contentType = contentType;
    return simulcastStream;
}

} // namespace

class EngineStreamDirectorTest : public ::testing::Test
{
    void SetUp() override
    {
        const config::Config config;
        _engineStreamDirector = std::make_unique<bridge::EngineStreamDirector>(config);
    }
    void TearDown() override { _engineStreamDirector.reset(); }

protected:
    std::unique_ptr<bridge::EngineStreamDirector> _engineStreamDirector;

    void addActiveVideoSender(const size_t endpointIdHash, const uint32_t startSsrc)
    {
        _engineStreamDirector->addParticipant(endpointIdHash,
            makeSimulcastStream(startSsrc, startSsrc + 1, startSsrc + 2, startSsrc + 3, startSsrc + 4, startSsrc + 5));

        _engineStreamDirector->setUplinkEstimateKbps(endpointIdHash, 100000, 5 * utils::Time::sec);

        _engineStreamDirector->streamActiveStateChanged(endpointIdHash, startSsrc, true);
        _engineStreamDirector->streamActiveStateChanged(endpointIdHash, startSsrc + 2, true);
        _engineStreamDirector->streamActiveStateChanged(endpointIdHash, startSsrc + 4, true);
    }
};

TEST_F(EngineStreamDirectorTest, newSimulcastStreamIsIncludedFirstStream)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(1, 1, true, 0));
}

TEST_F(EngineStreamDirectorTest, newSecondarySimulcastStreamIsIncludedFirstStream)
{
    bridge::SimulcastStream secondary = makeSimulcastStream(7, 8);
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6), &secondary);
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(7, 1, true, 0));
}

TEST_F(EngineStreamDirectorTest, oldSimulcastStreamIsIncludedWhenNewIsAdded)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);

    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(1, 1, true, 0));
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(7, 1, true, 0));
}

TEST_F(EngineStreamDirectorTest, simulcastStreamIsNotIncludedWhenRemoved)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);

    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(1, 1, true, 0));

    _engineStreamDirector->removeParticipant(1);

    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(1, 1, true, 0));
}

TEST_F(EngineStreamDirectorTest, secondarySimulcastStreamIsNotIncludedWhenRemoved)
{
    bridge::SimulcastStream secondary = makeSimulcastStream(13, 14);
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12), &secondary);
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);

    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(7, 2, true, 0));
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(13, 2, true, 0));

    _engineStreamDirector->removeParticipant(2);

    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(7, 2, true, 0));
    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(13, 2, true, 0));
}

TEST_F(EngineStreamDirectorTest, remainingSimulcastStreamIsIncludedWhenSecondIsRemoved)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->removeParticipant(2);
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(1, 1, true, 0));
}

TEST_F(EngineStreamDirectorTest, remainingSecondarySimulcastStreamIsIncludedWhenSecondIsRemoved)
{
    bridge::SimulcastStream secondary = makeSimulcastStream(13, 14);
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12), &secondary);
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->removeParticipant(1);

    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(13, 2, true, 0));
}

TEST_F(EngineStreamDirectorTest, simulcastStreamCannotBeAddedTwice)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(7, 8, 9, 10, 11, 12));

    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(1, 1, true, 0));
    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(7, 1, true, 0));
}

TEST_F(EngineStreamDirectorTest, pinnedHighQualityStreamIsIncluded)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(3, makeSimulcastStream(13, 14, 15, 16, 17, 18));
    _engineStreamDirector->setUplinkEstimateKbps(3, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(3, 17, true);
    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(17, 3, true, 0));
    _engineStreamDirector->pin(1, 3);
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(17, 3, true, 0));
}

TEST_F(EngineStreamDirectorTest, pinnedMidQualityStreamIsIncluded)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(3, makeSimulcastStream(13, 14, 15, 16, 17, 18));
    _engineStreamDirector->setUplinkEstimateKbps(3, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(3, 15, true);

    _engineStreamDirector->pin(1, 3);
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(15, 3, true, 0));
}

TEST_F(EngineStreamDirectorTest, defaultQualityStreamIsNotIncludedWhenPinnedByAll)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->addParticipant(3, makeSimulcastStream(13, 14, 15, 16, 17, 18));
    _engineStreamDirector->setUplinkEstimateKbps(3, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(1, 1, true);
    _engineStreamDirector->streamActiveStateChanged(1, 3, true);
    _engineStreamDirector->streamActiveStateChanged(1, 5, true);

    _engineStreamDirector->streamActiveStateChanged(2, 7, true);
    _engineStreamDirector->streamActiveStateChanged(2, 9, true);
    _engineStreamDirector->streamActiveStateChanged(2, 11, true);

    _engineStreamDirector->streamActiveStateChanged(3, 13, true);
    _engineStreamDirector->streamActiveStateChanged(3, 15, true);
    _engineStreamDirector->streamActiveStateChanged(3, 17, true);

    _engineStreamDirector->pin(1, 3);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 1);

    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(1, 1, true, 0));
}

TEST_F(EngineStreamDirectorTest, defaultQualityStreamIsForwardedForNonPinTarget)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(3, makeSimulcastStream(13, 14, 15, 16, 17, 18));
    _engineStreamDirector->setUplinkEstimateKbps(3, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(1, 1, true);
    _engineStreamDirector->streamActiveStateChanged(1, 3, true);
    _engineStreamDirector->streamActiveStateChanged(1, 7, true);
    _engineStreamDirector->streamActiveStateChanged(1, 9, true);

    _engineStreamDirector->pin(2, 1);

    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(2, 13));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 15));
}

TEST_F(EngineStreamDirectorTest, secondaryDefaultQualityStreamIsForwardedForNonPinTarget)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);

    bridge::SimulcastStream secondary = makeSimulcastStream(19, 20);
    _engineStreamDirector->addParticipant(3, makeSimulcastStream(13, 14, 15, 16, 17, 18), &secondary);
    _engineStreamDirector->setUplinkEstimateKbps(3, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->pin(2, 1);

    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(2, 19));
}

TEST_F(EngineStreamDirectorTest, pinnedHighQualityStreamIsForwarded)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(1, 1, true);
    _engineStreamDirector->streamActiveStateChanged(1, 3, true);
    _engineStreamDirector->streamActiveStateChanged(1, 5, true);
    _engineStreamDirector->streamActiveStateChanged(2, 7, true);
    _engineStreamDirector->streamActiveStateChanged(2, 9, true);
    _engineStreamDirector->streamActiveStateChanged(2, 11, true);

    _engineStreamDirector->pin(1, 2);

    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(1, 11));
}

TEST_F(EngineStreamDirectorTest, unpinnedHighQualityStreamIsNoLongerForwarded)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(3, makeSimulcastStream(13, 14, 15, 16, 17, 18));
    _engineStreamDirector->setUplinkEstimateKbps(3, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(1, 1, true);
    _engineStreamDirector->streamActiveStateChanged(1, 3, true);
    _engineStreamDirector->streamActiveStateChanged(1, 5, true);
    _engineStreamDirector->streamActiveStateChanged(2, 7, true);
    _engineStreamDirector->streamActiveStateChanged(2, 9, true);
    _engineStreamDirector->streamActiveStateChanged(2, 11, true);
    _engineStreamDirector->streamActiveStateChanged(3, 13, true);
    _engineStreamDirector->streamActiveStateChanged(3, 15, true);
    _engineStreamDirector->streamActiveStateChanged(3, 17, true);

    _engineStreamDirector->pin(2, 3);
    _engineStreamDirector->pin(2, 0);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 17));
}

TEST_F(EngineStreamDirectorTest, removedPinnedHighQualityStreamIsNoLongerForwarded)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(1, 1, true);
    _engineStreamDirector->streamActiveStateChanged(1, 3, true);
    _engineStreamDirector->streamActiveStateChanged(1, 5, true);
    _engineStreamDirector->streamActiveStateChanged(2, 7, true);
    _engineStreamDirector->streamActiveStateChanged(2, 9, true);
    _engineStreamDirector->streamActiveStateChanged(2, 11, true);

    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->removeParticipant(1);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 5));
}

TEST_F(EngineStreamDirectorTest, participantForDefaultLevelSsrcIsSet)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(1, 1, true);
    _engineStreamDirector->streamActiveStateChanged(1, 3, true);
    _engineStreamDirector->streamActiveStateChanged(1, 5, true);
    _engineStreamDirector->streamActiveStateChanged(2, 7, true);
    _engineStreamDirector->streamActiveStateChanged(2, 9, true);
    _engineStreamDirector->streamActiveStateChanged(2, 11, true);

    EXPECT_EQ(1, _engineStreamDirector->getParticipantForDefaultLevelSsrc(1));
}

TEST_F(EngineStreamDirectorTest, participantForSecondaryDefaultLevelSsrcIsSet)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);

    bridge::SimulcastStream secondary = makeSimulcastStream(13, 14);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12), &secondary);

    EXPECT_EQ(2, _engineStreamDirector->getParticipantForDefaultLevelSsrc(13));
}

TEST_F(EngineStreamDirectorTest, participantForDefaultLevelSsrcReturnsZeroOnHigherLevelSsrc)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(1, 1, true);
    _engineStreamDirector->streamActiveStateChanged(1, 3, true);
    _engineStreamDirector->streamActiveStateChanged(1, 5, true);
    _engineStreamDirector->streamActiveStateChanged(2, 7, true);
    _engineStreamDirector->streamActiveStateChanged(2, 9, true);
    _engineStreamDirector->streamActiveStateChanged(2, 11, true);

    EXPECT_EQ(0, _engineStreamDirector->getParticipantForDefaultLevelSsrc(3));
}

TEST_F(EngineStreamDirectorTest, participantForDefaultLevelSsrcReturnsZeroOnUnknownSsrc)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(1, 1, true);
    _engineStreamDirector->streamActiveStateChanged(1, 3, true);
    _engineStreamDirector->streamActiveStateChanged(1, 5, true);

    EXPECT_EQ(0, _engineStreamDirector->getParticipantForDefaultLevelSsrc(7));
}

TEST_F(EngineStreamDirectorTest, feedbackSsrcReturnsDefault)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(1, 1, true);
    _engineStreamDirector->streamActiveStateChanged(1, 3, true);
    _engineStreamDirector->streamActiveStateChanged(1, 5, true);
    _engineStreamDirector->streamActiveStateChanged(2, 7, true);
    _engineStreamDirector->streamActiveStateChanged(2, 9, true);
    _engineStreamDirector->streamActiveStateChanged(2, 11, true);

    uint32_t feedbackSsrc = 0;
    const auto result = _engineStreamDirector->getFeedbackSsrc(1, feedbackSsrc);
    EXPECT_TRUE(result);
    EXPECT_EQ(2, feedbackSsrc);
}

TEST_F(EngineStreamDirectorTest, feedbackSsrcReturnsFalseWhenSsrcIsUnknown)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(1, 1, true);
    _engineStreamDirector->streamActiveStateChanged(1, 3, true);
    _engineStreamDirector->streamActiveStateChanged(1, 5, true);
    _engineStreamDirector->streamActiveStateChanged(2, 7, true);
    _engineStreamDirector->streamActiveStateChanged(2, 9, true);
    _engineStreamDirector->streamActiveStateChanged(2, 11, true);

    uint32_t feedbackSsrc = 0;
    const auto result = _engineStreamDirector->getFeedbackSsrc(100, feedbackSsrc);
    EXPECT_FALSE(result);
}

TEST_F(EngineStreamDirectorTest, setUplinkEstimateKbps)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->updateBandwidthFloor(5, 1, 1);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->updateBandwidthFloor(5, 2, 2);
    _engineStreamDirector->updateBandwidthFloor(5, 3, 2);

    EXPECT_TRUE(_engineStreamDirector->setUplinkEstimateKbps(1, 100, 1 * utils::Time::sec));
    EXPECT_FALSE(_engineStreamDirector->setUplinkEstimateKbps(1, 1000, 2 * utils::Time::sec));
    EXPECT_TRUE(_engineStreamDirector->setUplinkEstimateKbps(1, 1000, 6 * utils::Time::sec));
    EXPECT_TRUE(_engineStreamDirector->setUplinkEstimateKbps(1, 100, 7 * utils::Time::sec));
    EXPECT_FALSE(_engineStreamDirector->setUplinkEstimateKbps(1, 10000, 8 * utils::Time::sec));
    EXPECT_FALSE(_engineStreamDirector->setUplinkEstimateKbps(1, 10000, 9 * utils::Time::sec));
    EXPECT_FALSE(_engineStreamDirector->setUplinkEstimateKbps(1, 10000, 10 * utils::Time::sec));
    EXPECT_FALSE(_engineStreamDirector->setUplinkEstimateKbps(1, 100, 11 * utils::Time::sec));
    EXPECT_FALSE(_engineStreamDirector->setUplinkEstimateKbps(1, 10000, 14 * utils::Time::sec));
    EXPECT_TRUE(_engineStreamDirector->setUplinkEstimateKbps(1, 10000, 16 * utils::Time::sec));
}

TEST_F(EngineStreamDirectorTest, pinnedMidQualityStreamIsIncludedMidBandwidth)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 1000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 1000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(3, makeSimulcastStream(13, 14, 15, 16, 17, 18));
    _engineStreamDirector->setUplinkEstimateKbps(3, 1000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(3, 15, true);
    _engineStreamDirector->streamActiveStateChanged(3, 17, true);
    _engineStreamDirector->pin(1, 3);
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(15, 3, true, 0));
    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(17, 3, true, 0));

    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(1, 15));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(1, 17));
}

TEST_F(EngineStreamDirectorTest, pinnedLowQualityStreamIsIncludedLowBandwidth)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(3, makeSimulcastStream(13, 14, 15, 16, 17, 18));
    _engineStreamDirector->setUplinkEstimateKbps(3, 100, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(3, 13, true);
    _engineStreamDirector->streamActiveStateChanged(3, 15, true);
    _engineStreamDirector->streamActiveStateChanged(3, 17, true);

    _engineStreamDirector->pin(1, 3);

    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(13, 3, true, 0));
    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(1, 13));
}

TEST_F(EngineStreamDirectorTest, defaultLevelSsrcNotInLastNListIsNotUsed)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(1, 1, false, 0));
}

TEST_F(EngineStreamDirectorTest, defaultLevelSsrcNotInLastNListAndPinnedByAllIsNotIncluded)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->pin(2, 1);
    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(1, 1, false, 0));
}

TEST_F(EngineStreamDirectorTest, defaultLevelSsrcNotInLastNListAndPinnedBySomeIsNotIncluded)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 100000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(3, makeSimulcastStream(13, 14, 15, 16, 17, 18));
    _engineStreamDirector->setUplinkEstimateKbps(3, 100000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(1, 1, true);
    _engineStreamDirector->streamActiveStateChanged(1, 3, true);
    _engineStreamDirector->streamActiveStateChanged(1, 5, true);

    _engineStreamDirector->streamActiveStateChanged(2, 7, true);
    _engineStreamDirector->streamActiveStateChanged(2, 9, true);
    _engineStreamDirector->streamActiveStateChanged(2, 11, true);

    _engineStreamDirector->streamActiveStateChanged(3, 13, true);
    _engineStreamDirector->streamActiveStateChanged(3, 15, true);
    _engineStreamDirector->streamActiveStateChanged(3, 17, true);

    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(1, 3);
    _engineStreamDirector->pin(3, 2);

    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(1, 1, false, 0));
}

TEST_F(EngineStreamDirectorTest, pinnedHighQualitySsrcNotInLastNListIsUsed)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(1, 10000, 5 * utils::Time::sec);
    _engineStreamDirector->addParticipant(2, makeSimulcastStream(7, 8, 9, 10, 11, 12));
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 5 * utils::Time::sec);

    _engineStreamDirector->streamActiveStateChanged(2, 7, true);
    _engineStreamDirector->streamActiveStateChanged(2, 9, true);
    _engineStreamDirector->streamActiveStateChanged(2, 11, true);

    _engineStreamDirector->pin(1, 2);

    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(11, 2, false, 0));
}

TEST_F(EngineStreamDirectorTest, highEstimateUsesHighQualityLevel)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 0 * utils::Time::sec);

    _engineStreamDirector->addParticipant(2);
    _engineStreamDirector->pin(2, 1);

    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 61 * utils::Time::sec);

    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(5, 1, true, 0));
}

TEST_F(EngineStreamDirectorTest, midEstimateUsesMidQualityLevel)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 0 * utils::Time::sec);

    _engineStreamDirector->addParticipant(2);
    _engineStreamDirector->pin(2, 1);

    _engineStreamDirector->setUplinkEstimateKbps(2, 2000, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(2, 2000, 61 * utils::Time::sec);

    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(3, 1, true, 0));
}

TEST_F(EngineStreamDirectorTest, lowEstimateUsesLowQualityLevel)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 0 * utils::Time::sec);

    _engineStreamDirector->addParticipant(2);
    _engineStreamDirector->pin(2, 1);

    _engineStreamDirector->setUplinkEstimateKbps(2, 1, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(2, 1, 61 * utils::Time::sec);

    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(1, 1, true, 0));
}

TEST_F(EngineStreamDirectorTest, bandwidthEstimationAllNeededQualityLevelsAreUsed)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 0 * utils::Time::sec);

    _engineStreamDirector->addParticipant(2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->addParticipant(3);
    _engineStreamDirector->pin(3, 1);
    _engineStreamDirector->addParticipant(4);
    _engineStreamDirector->pin(4, 1);

    // High estimate
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 61 * utils::Time::sec);

    // Mid estimate
    _engineStreamDirector->setUplinkEstimateKbps(3, 2000, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(3, 2000, 61 * utils::Time::sec);

    // Low estimate
    _engineStreamDirector->setUplinkEstimateKbps(4, 1, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(4, 1, 61 * utils::Time::sec);

    // Used by 4
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(1, 1, true, 0));
    // Used by 3
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(3, 1, true, 0));
    // Used by 2
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(5, 1, true, 0));
}

TEST_F(EngineStreamDirectorTest, highEstimateForwardsHighQualityLevel)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 0 * utils::Time::sec);

    _engineStreamDirector->addParticipant(2);
    _engineStreamDirector->pin(2, 1);

    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 61 * utils::Time::sec);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 1));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 3));
    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(2, 5));
}

TEST_F(EngineStreamDirectorTest, midEstimateForwardsMidQualityLevel)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 0 * utils::Time::sec);

    _engineStreamDirector->addParticipant(2);
    _engineStreamDirector->pin(2, 1);

    _engineStreamDirector->setUplinkEstimateKbps(2, 2000, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(2, 2000, 61 * utils::Time::sec);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 1));
    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(2, 3));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 5));
}

TEST_F(EngineStreamDirectorTest, lowEstimateForwardsLowQualityLevel)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 0 * utils::Time::sec);

    _engineStreamDirector->addParticipant(2);
    _engineStreamDirector->pin(2, 1);

    _engineStreamDirector->setUplinkEstimateKbps(2, 100, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(2, 100, 61 * utils::Time::sec);

    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(2, 1));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 3));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 5));
}

TEST_F(EngineStreamDirectorTest, lowestEstimateForwardsNoVideo)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 0 * utils::Time::sec);

    _engineStreamDirector->addParticipant(2);
    _engineStreamDirector->pin(2, 1);

    _engineStreamDirector->setUplinkEstimateKbps(2, 1, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(2, 1, 61 * utils::Time::sec);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 1));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 3));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 5));
}

TEST_F(EngineStreamDirectorTest, bandwidthEstimationAllNeededQualityLevelsAreForwarded)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 0 * utils::Time::sec);

    _engineStreamDirector->addParticipant(2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->addParticipant(3);
    _engineStreamDirector->pin(3, 1);
    _engineStreamDirector->addParticipant(4);
    _engineStreamDirector->pin(4, 1);
    _engineStreamDirector->addParticipant(5);
    _engineStreamDirector->pin(5, 1);

    // High estimate
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(2, 10000, 61 * utils::Time::sec);

    // Mid estimate
    _engineStreamDirector->setUplinkEstimateKbps(3, 2000, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(3, 2000, 61 * utils::Time::sec);

    // Low estimate
    _engineStreamDirector->setUplinkEstimateKbps(4, 100, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(4, 100, 61 * utils::Time::sec);

    // Very low estimate
    _engineStreamDirector->setUplinkEstimateKbps(5, 99, 60 * utils::Time::sec);
    _engineStreamDirector->setUplinkEstimateKbps(5, 99, 61 * utils::Time::sec);

    // Used by 5
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(5, 1));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(5, 3));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(5, 5));

    // Used by 4
    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(4, 1));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(4, 3));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(4, 5));

    // Used by 3
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(3, 1));
    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(3, 3));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(3, 5));

    // Used by 2
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 1));
    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(2, 3));
    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(2, 5));
}

TEST_F(EngineStreamDirectorTest, reversePinCountResetWhenParticipantLeaves)
{
    _engineStreamDirector->addParticipant(1, makeSimulcastStream(1, 2, 3, 4, 5, 6));
    _engineStreamDirector->streamActiveStateChanged(1, 1, true);
    _engineStreamDirector->streamActiveStateChanged(1, 3, true);
    _engineStreamDirector->streamActiveStateChanged(1, 5, true);

    _engineStreamDirector->addParticipant(2);
    _engineStreamDirector->pin(2, 1);
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(5, 1, true, 0));

    _engineStreamDirector->removeParticipantPins(2);
    _engineStreamDirector->removeParticipant(2);

    _engineStreamDirector->addParticipant(2);

    _engineStreamDirector->pin(2, 1);
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(5, 1, true, 0));
}

TEST_F(EngineStreamDirectorTest, contentTypeSlidesNotInLastNIsNotUsedWithoutRecordingStreams)
{
    _engineStreamDirector->addParticipant(1,
        makeSimulcastStream(1, 2, bridge::SimulcastStream::VideoContentType::SLIDES));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(1, 1, false, 0));
}

TEST_F(EngineStreamDirectorTest, contentTypeSlidesNotInLastNIsUsedWithRecordingStreams)
{
    _engineStreamDirector->addParticipant(1,
        makeSimulcastStream(1, 2, bridge::SimulcastStream::VideoContentType::SLIDES));
    _engineStreamDirector->setUplinkEstimateKbps(1, 100000, 5 * utils::Time::sec);
    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(1, 1, false, 1));
}

TEST_F(EngineStreamDirectorTest, lowQualitySsrc_NotInLastN_AndNotPinned_AndAllParticipantsHavePinTargets_IsNotUsed)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);

    _engineStreamDirector->pin(1, 2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 2);
    _engineStreamDirector->pin(4, 2);

    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(13, 3, false, 0));
}

TEST_F(EngineStreamDirectorTest, midQualitySsrc_NotInLastN_AndNotPinned_AndAllParticipantsHavePinTargets_IsNotUsed)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);

    _engineStreamDirector->pin(1, 2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 2);
    _engineStreamDirector->pin(4, 2);

    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(15, 3, false, 0));
}

TEST_F(EngineStreamDirectorTest, highQualitySsrc_NotInLastN_AndNotPinned_AndAllParticipantsHavePinTargets_IsNotUsed)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);

    _engineStreamDirector->pin(1, 2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 2);
    _engineStreamDirector->pin(4, 2);

    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(17, 3, false, 0));
}

TEST_F(EngineStreamDirectorTest, lowQualitySsrc_InLastN_AndNotPinned_AndAllParticipantsHavePinTargets_IsUsed)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);

    _engineStreamDirector->pin(1, 2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 2);
    _engineStreamDirector->pin(4, 2);

    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(13, 3, true, 0));
}

TEST_F(EngineStreamDirectorTest, midQualitySsrc_InLastN_AndNotPinned_AndAllParticipantsHavePinTargets_IsNotUsed)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);

    _engineStreamDirector->pin(1, 2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 2);
    _engineStreamDirector->pin(4, 2);

    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(15, 3, true, 0));
}

TEST_F(EngineStreamDirectorTest, highQualitySsrc_InLastN_AndNotPinned_AndAllParticipantsHavePinTargets_IsNotUsed)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);

    _engineStreamDirector->pin(1, 2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 2);
    _engineStreamDirector->pin(4, 2);

    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(17, 3, true, 0));
}

TEST_F(EngineStreamDirectorTest, lowQualitySsrc_InLastN_AndNotPinned_AndSomeParticipantsHaveNoPinTargets_IsUsed)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);

    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 2);
    _engineStreamDirector->pin(4, 2);

    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(13, 3, true, 0));
}

TEST_F(EngineStreamDirectorTest, midQualitySsrc_InLastN_AndNotPinned_AndSomeParticipantsHaveNoPinTargets_IsUsed)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);

    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 2);
    _engineStreamDirector->pin(4, 2);

    EXPECT_TRUE(_engineStreamDirector->isSsrcUsed(15, 3, true, 0));
}

TEST_F(EngineStreamDirectorTest, highQualitySsrc_InLastN_AndNotPinned_AndSomeParticipantsHaveNoPinTargets_IsNotUsed)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);

    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 2);
    _engineStreamDirector->pin(4, 2);

    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(17, 3, true, 0));
}

TEST_F(EngineStreamDirectorTest, lowQualitySsrc_NotInLastN_AndNotPinned_AndSomeParticipantsHaveNoPinTargets_IsNotUsed)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);

    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 2);
    _engineStreamDirector->pin(4, 2);

    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(13, 3, false, 0));
}

TEST_F(EngineStreamDirectorTest, midQualitySsrc_NotInLastN_AndNotPinned_AndSomeParticipantsHaveNoPinTargets_IsNotUsed)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);

    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 2);
    _engineStreamDirector->pin(4, 2);

    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(15, 3, false, 0));
}

TEST_F(EngineStreamDirectorTest, highQualitySsrc_NotInLastN_AndNotPinned_AndSomeParticipantsHaveNoPinTargets_IsNotUsed)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);

    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 2);
    _engineStreamDirector->pin(4, 2);

    EXPECT_FALSE(_engineStreamDirector->isSsrcUsed(17, 3, false, 0));
}

TEST_F(EngineStreamDirectorTest, lowQualitySsrc_NotPinTarget_IsForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);

    _engineStreamDirector->pin(1, 2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 1);

    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(1, 13));
}

TEST_F(EngineStreamDirectorTest, midQualitySsrc_NotPinTarget_IsNotForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);

    _engineStreamDirector->pin(1, 2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 1);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(1, 15));
}

TEST_F(EngineStreamDirectorTest, highQualitySsrc_NotPinTarget_IsNotForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);

    _engineStreamDirector->pin(1, 2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 1);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(1, 17));
}

TEST_F(EngineStreamDirectorTest, lowQualitySsrc_PinTarget_IsNotForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);

    _engineStreamDirector->pin(1, 2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 1);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(1, 7));
}

TEST_F(EngineStreamDirectorTest, midQualitySsrc_PinTarget_IsNotForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);

    _engineStreamDirector->pin(1, 2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 1);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(1, 9));
}

TEST_F(EngineStreamDirectorTest, highQualitySsrc_PinTarget_IsForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);

    _engineStreamDirector->pin(1, 2);
    _engineStreamDirector->pin(2, 1);
    _engineStreamDirector->pin(3, 1);

    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(1, 11));
}

TEST_F(EngineStreamDirectorTest, lowQualitySsrc_HasNoPinTarget_UnderKbpsLimit_IsForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);

    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(1, 7));
}

TEST_F(EngineStreamDirectorTest, midQualitySsrc_HasNoPinTarget_UnderKbpsLimit_IsNotForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(1, 9));
}

TEST_F(EngineStreamDirectorTest, highQualitySsrc_HasNoPinTarget_UnderKbpsLimit_IsNotForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(1, 11));
}

TEST_F(EngineStreamDirectorTest, lowQualitySsrc_HasNoPinTarget_OverKbpsLimit_IsForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);
    addActiveVideoSender(5, 25);
    addActiveVideoSender(6, 31);
    addActiveVideoSender(7, 37);
    addActiveVideoSender(8, 43);

    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(1, 7));
}

TEST_F(EngineStreamDirectorTest, midQualitySsrc_HasNoPinTarget_OverKbpsLimit_IsNotForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);
    addActiveVideoSender(5, 25);
    addActiveVideoSender(6, 31);
    addActiveVideoSender(7, 37);
    addActiveVideoSender(8, 43);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(1, 9));
}

TEST_F(EngineStreamDirectorTest, highQualitySsrc_HasNoPinTarget_OverKbpsLimit_IsNotForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);
    addActiveVideoSender(4, 19);
    addActiveVideoSender(5, 25);
    addActiveVideoSender(6, 31);
    addActiveVideoSender(7, 37);
    addActiveVideoSender(8, 43);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(1, 11));
}

TEST_F(EngineStreamDirectorTest, lowQualitySsrc_HasNoPinTarget_OverEstimatedKbps_IsForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);

    _engineStreamDirector->setUplinkEstimateKbps(1, 500, 10 * utils::Time::sec);

    EXPECT_TRUE(_engineStreamDirector->shouldForwardSsrc(1, 7));
}

TEST_F(EngineStreamDirectorTest, midQualitySsrc_HasNoPinTarget_OverEstimatedKbps_IsForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);

    _engineStreamDirector->setUplinkEstimateKbps(1, 500, 10 * utils::Time::sec);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(1, 9));
}

TEST_F(EngineStreamDirectorTest, midQualitySsrc_HasNoPinTarget_OverEstimatedKbps_IsNotForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);

    _engineStreamDirector->setUplinkEstimateKbps(1, 500, 10 * utils::Time::sec);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(1, 9));
}

TEST_F(EngineStreamDirectorTest, highQualitySsrc_HasNoPinTarget_OverEstimatedKbps_IsNotForwarded)
{
    addActiveVideoSender(1, 1);
    addActiveVideoSender(2, 7);
    addActiveVideoSender(3, 13);

    _engineStreamDirector->setUplinkEstimateKbps(1, 500, 10 * utils::Time::sec);

    EXPECT_FALSE(_engineStreamDirector->shouldForwardSsrc(1, 11));
}
