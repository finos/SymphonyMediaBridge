#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineVideoStream.h"
#include "bridge/engine/SimulcastStream.h"
#include "jobmanager/JobManager.h"
#include "nlohmann/json.hpp"
#include "test/bridge/ActiveMediaListTestLevels.h"
#include "test/bridge/DummyRtcTransport.h"
#include "utils/StringBuilder.h"
#include <gtest/gtest.h>
#include <memory>

namespace
{

static const uint32_t defaultLastN = 2;
static const uint32_t audioLastN = 3;

void threadFunction(jobmanager::JobManager* jobManager)
{
    auto job = jobManager->wait();
    while (job)
    {
        job->run();
        jobManager->freeJob(job);
        job = jobManager->wait();
    }
}

bool endpointsContainsId(const nlohmann::json& messageJson, const char* id)
{
    const auto& endpoints = messageJson["endpoints"];

    for (const auto& endpoint : endpoints)
    {
#if ENABLE_LEGACY_API
        if (endpoint["id"].get<std::string>().compare(id) == 0)
        {
            return true;
        }
#else
        if (endpoint["endpoint"].get<std::string>().compare(id) == 0)
        {
            return true;
        }
#endif
    }

    return false;
}

} // namespace

class ActiveMediaListTest : public ::testing::Test
{
public:
    ActiveMediaListTest() : _engineAudioStreams(16), _engineVideoStreams(16) {}

private:
    void SetUp() override
    {

        for (uint32_t i = 0; i < 5; ++i)
        {
            _audioSsrcs.push_back(i);
        }

        for (uint32_t i = 10; i < 20; ++i)
        {
            _videoSsrcs.push_back(bridge::SimulcastLevel({i, i + 100}));
        }

        for (uint32_t i = 20; i < 24; ++i)
        {
            _videoPinSsrcs.push_back(bridge::SimulcastLevel({i, i + 100}));
        }

        _jobManager = std::make_unique<jobmanager::JobManager>();
        _jobQueue = std::make_unique<jobmanager::JobQueue>(*_jobManager);
        _transport = std::make_unique<DummyRtcTransport>(*_jobQueue);

        _activeMediaList =
            std::make_unique<bridge::ActiveMediaList>(_audioSsrcs, _videoSsrcs, defaultLastN, audioLastN);

        _engineAudioStreams.clear();
        _engineVideoStreams.clear();
    }

    void TearDown() override
    {
        _activeMediaList.reset();

        for (auto& videoStreamsEntry : _engineVideoStreams)
        {
            delete videoStreamsEntry.second;
        }
        _engineAudioStreams.clear();
        _engineVideoStreams.clear();

        auto thread = std::make_unique<std::thread>(threadFunction, _jobManager.get());
        _jobQueue.reset();
        _jobManager->stop();
        thread->join();
        _jobManager.reset();
    }

protected:
    std::vector<uint32_t> _audioSsrcs;
    std::vector<bridge::SimulcastLevel> _videoSsrcs;
    std::vector<bridge::SimulcastLevel> _videoPinSsrcs;

    std::unique_ptr<jobmanager::JobManager> _jobManager;
    std::unique_ptr<jobmanager::JobQueue> _jobQueue;
    std::unique_ptr<DummyRtcTransport> _transport;
    concurrency::MpmcHashmap32<size_t, bridge::EngineAudioStream*> _engineAudioStreams;
    concurrency::MpmcHashmap32<size_t, bridge::EngineVideoStream*> _engineVideoStreams;

    std::unique_ptr<bridge::ActiveMediaList> _activeMediaList;

    bridge::EngineVideoStream* addEngineVideoStream(const size_t id)
    {
        bridge::SimulcastStream simulcastStream{0};
        simulcastStream._numLevels = 1;
        simulcastStream._levels[0]._ssrc = id;
        simulcastStream._levels[0]._feedbackSsrc = 100 + id;

        auto engineVideoStream = new bridge::EngineVideoStream(std::to_string(id),
            id,
            id,
            simulcastStream,
            utils::Optional<bridge::SimulcastStream>(),
            *_transport,
            bridge::RtpMap(),
            bridge::RtpMap(),
            0,
            bridge::SsrcWhitelist({false, 0, {0, 0}}),
            true,
            _videoPinSsrcs);

        _engineVideoStreams.emplace(id, engineVideoStream);
        return engineVideoStream;
    }

    void addEngineAudioStream(const size_t id)
    {
        auto engineAudioStream = std::make_unique<bridge::EngineAudioStream>(std::to_string(id),
            id,
            id,
            utils::Optional<uint32_t>(id),
            *_transport,
            1,
            3,
            false,
            bridge::RtpMap(),
            true);

        _engineAudioStreams.emplace(id, engineAudioStream.release());
    }

    uint64_t switchDominantSpeaker(const uint64_t timestamp, const size_t endpointIdHash)
    {
        uint64_t newTimestamp = timestamp;

        for (uint32_t i = 0; i < 1000; ++i)
        {
            for (uint32_t j = 0; j < 100; ++j)
            {
                _activeMediaList->onNewAudioLevel(endpointIdHash, 1);
            }

            newTimestamp += 1000;
            bool dominantSpeakerChanged = false;
            bool userMediaMapChanged = false;
            _activeMediaList->process(newTimestamp, dominantSpeakerChanged, userMediaMapChanged);
            if (dominantSpeakerChanged)
            {
                return newTimestamp;
            }
        }

        return newTimestamp;
    }

    void zeroLevels(const size_t endpointIdHash)
    {
        for (uint32_t i = 0; i < 100; ++i)
        {
            _activeMediaList->onNewAudioLevel(endpointIdHash, 126);
        }
    }
};

TEST_F(ActiveMediaListTest, maxOneSwitchEveryTwoSeconds)
{
    uint64_t timestamp = 123456;
    const auto participant1 = 1;
    const auto participant2 = 2;

    // First added participant is set as dominant speaker, add this participant here so that switching will happen
    _activeMediaList->addAudioParticipant(3);

    _activeMediaList->addAudioParticipant(participant1);
    _activeMediaList->addAudioParticipant(participant2);

    bool dominantSpeakerChanged = false;
    bool userMediaMapChanged = false;

    _activeMediaList->process(timestamp, dominantSpeakerChanged, userMediaMapChanged);
    for (auto i = 0; i < 30; ++i)
    {
        _activeMediaList->onNewAudioLevel(participant1, 64 + (i % 40));
        _activeMediaList->onNewAudioLevel(participant2, 96 + (i % 5));
    }
    timestamp += 200;
    _activeMediaList->process(timestamp, dominantSpeakerChanged, userMediaMapChanged);
    EXPECT_TRUE(dominantSpeakerChanged);

    for (auto i = 0; i < 199; ++i)
    {
        _activeMediaList->onNewAudioLevel(participant1, 96 + (i % 5));
        _activeMediaList->onNewAudioLevel(participant2, 64 + (i % 40));
        timestamp += 10;
        _activeMediaList->process(timestamp, dominantSpeakerChanged, userMediaMapChanged);
        EXPECT_FALSE(dominantSpeakerChanged);
    }

    _activeMediaList->onNewAudioLevel(participant1, 96);
    _activeMediaList->onNewAudioLevel(participant2, 64);
    timestamp += 11;
    _activeMediaList->process(timestamp, dominantSpeakerChanged, userMediaMapChanged);
    EXPECT_TRUE(dominantSpeakerChanged);

    _activeMediaList->removeAudioParticipant(participant1);
    _activeMediaList->removeAudioParticipant(participant2);
}

TEST_F(ActiveMediaListTest, audioParticipantsAddedToAudioRewriteMap)
{
    _activeMediaList->addAudioParticipant(1);
    _activeMediaList->addAudioParticipant(2);
    _activeMediaList->addAudioParticipant(3);

    const auto& audioRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(1));
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(2));
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(3));
}

TEST_F(ActiveMediaListTest, audioParticipantsNotAddedToFullAudioRewriteMap)
{
    _activeMediaList->addAudioParticipant(1);
    _activeMediaList->addAudioParticipant(2);
    _activeMediaList->addAudioParticipant(3);
    _activeMediaList->addAudioParticipant(4);

    const auto& audioRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(1));
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(2));
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(3));
    EXPECT_EQ(audioRewriteMap.end(), audioRewriteMap.find(4));
}

TEST_F(ActiveMediaListTest, activeAudioParticipantIsSwitchedIn)
{
    _activeMediaList->addAudioParticipant(1);
    _activeMediaList->addAudioParticipant(2);
    _activeMediaList->addAudioParticipant(3);
    _activeMediaList->addAudioParticipant(4);

    const auto& audioRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
    EXPECT_EQ(audioRewriteMap.end(), audioRewriteMap.find(4));
    EXPECT_EQ(audioLastN, audioRewriteMap.size());

    _activeMediaList->onNewAudioLevel(4, 10);

    bool dominantSpeakerChanged = false;
    bool userMediaMapChanged = false;
    _activeMediaList->process(1000, dominantSpeakerChanged, userMediaMapChanged);

    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(4));
    EXPECT_EQ(4, audioRewriteMap.size());
}

TEST_F(ActiveMediaListTest, activeAudioParticipantIsSwitchedInEvenIfNotMostDominant)
{
    const size_t numParticipants = 10;
    for (size_t i = 1; i <= numParticipants; ++i)
    {
        _activeMediaList->addAudioParticipant(i);
        for (const auto element : ActiveMediaListTestLevels::silence)
        {
            _activeMediaList->onNewAudioLevel(i, element);
        }
    }

    const auto& audioRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
    EXPECT_EQ(audioRewriteMap.end(), audioRewriteMap.find(4));

    bool dominantSpeakerChanged = false;
    bool userMediaMapChanged = false;
    _activeMediaList->process(1000, dominantSpeakerChanged, userMediaMapChanged);

    for (const auto element : ActiveMediaListTestLevels::longUtterance)
    {
        _activeMediaList->onNewAudioLevel(1, element);
    }

    for (const auto element : ActiveMediaListTestLevels::longUtterance)
    {
        _activeMediaList->onNewAudioLevel(2, element);
    }

    for (const auto element : ActiveMediaListTestLevels::shortUtterance)
    {
        _activeMediaList->onNewAudioLevel(4, element);
    }

    _activeMediaList->process(2000, dominantSpeakerChanged, userMediaMapChanged);

    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(4));
    EXPECT_EQ(4, audioRewriteMap.size());
}

TEST_F(ActiveMediaListTest, activeAudioParticipantIsSwitchedInEvenIfNotMostDominantSmallList)
{
    const size_t numParticipants = 2;
    for (size_t i = 1; i <= numParticipants; ++i)
    {
        _activeMediaList->addAudioParticipant(i);
        for (const auto element : ActiveMediaListTestLevels::silence)
        {
            _activeMediaList->onNewAudioLevel(i, element);
        }
    }

    bool dominantSpeakerChanged = false;
    bool userMediaMapChanged = false;
    _activeMediaList->process(1000, dominantSpeakerChanged, userMediaMapChanged);

    for (const auto element : ActiveMediaListTestLevels::longUtterance)
    {
        _activeMediaList->onNewAudioLevel(1, element);
    }

    for (const auto element : ActiveMediaListTestLevels::shortUtterance)
    {
        _activeMediaList->onNewAudioLevel(2, element);
    }

    _activeMediaList->process(2000, dominantSpeakerChanged, userMediaMapChanged);

    const auto& audioRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(2));
    EXPECT_EQ(2, audioRewriteMap.size());
}

TEST_F(ActiveMediaListTest, activeAudioParticipantIsSwitchedInEvenIfNotMostDominantSmallLastN)
{
    auto smallActiveMediaList = std::make_unique<bridge::ActiveMediaList>(_audioSsrcs, _videoSsrcs, 1, 3);

    smallActiveMediaList->addAudioParticipant(1);
    smallActiveMediaList->addAudioParticipant(2);
    smallActiveMediaList->addAudioParticipant(3);
    smallActiveMediaList->addAudioParticipant(4);

    for (const auto element : ActiveMediaListTestLevels::longUtterance)
    {
        smallActiveMediaList->onNewAudioLevel(1, element);
    }

    for (const auto element : ActiveMediaListTestLevels::longUtterance)
    {
        smallActiveMediaList->onNewAudioLevel(2, element);
    }

    for (const auto element : ActiveMediaListTestLevels::silence)
    {
        smallActiveMediaList->onNewAudioLevel(3, element);
    }

    for (const auto element : ActiveMediaListTestLevels::shortUtterance)
    {
        smallActiveMediaList->onNewAudioLevel(4, element);
    }

    bool dominantSpeakerChanged = false;
    bool userMediaMapChanged = false;
    smallActiveMediaList->process(1000, dominantSpeakerChanged, userMediaMapChanged);

    const auto& audioRewriteMap = smallActiveMediaList->getAudioSsrcRewriteMap();
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(1));
}

TEST_F(ActiveMediaListTest, videoParticipantsAddedToVideoRewriteMap)
{
    auto videoStream1 = addEngineVideoStream(1);
    auto videoStream2 = addEngineVideoStream(2);
    auto videoStream3 = addEngineVideoStream(3);

    _activeMediaList->addVideoParticipant(1, videoStream1->_simulcastStream, videoStream1->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(2, videoStream2->_simulcastStream, videoStream2->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(3, videoStream3->_simulcastStream, videoStream3->_secondarySimulcastStream);

    const auto& videoRewriteMap = _activeMediaList->getVideoSsrcRewriteMap();
    EXPECT_NE(videoRewriteMap.end(), videoRewriteMap.find(1));
    EXPECT_NE(videoRewriteMap.end(), videoRewriteMap.find(2));
    EXPECT_NE(videoRewriteMap.end(), videoRewriteMap.find(3));
}

TEST_F(ActiveMediaListTest, videoParticipantsNotAddedToFullVideoRewriteMap)
{
    auto videoStream1 = addEngineVideoStream(1);
    auto videoStream2 = addEngineVideoStream(2);
    auto videoStream3 = addEngineVideoStream(3);
    auto videoStream4 = addEngineVideoStream(4);

    _activeMediaList->addVideoParticipant(1, videoStream1->_simulcastStream, videoStream1->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(2, videoStream2->_simulcastStream, videoStream2->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(3, videoStream3->_simulcastStream, videoStream3->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(4, videoStream4->_simulcastStream, videoStream4->_secondarySimulcastStream);

    const auto& videoRewriteMap = _activeMediaList->getVideoSsrcRewriteMap();
    EXPECT_NE(videoRewriteMap.end(), videoRewriteMap.find(1));
    EXPECT_NE(videoRewriteMap.end(), videoRewriteMap.find(2));
    EXPECT_NE(videoRewriteMap.end(), videoRewriteMap.find(3));
    EXPECT_EQ(videoRewriteMap.end(), videoRewriteMap.find(4));
}

TEST_F(ActiveMediaListTest, userMediaMapContainsAllStreamsExcludingSelf)
{
    auto videoStream1 = addEngineVideoStream(1);
    auto videoStream2 = addEngineVideoStream(2);
    auto videoStream3 = addEngineVideoStream(3);

    _activeMediaList->addVideoParticipant(1, videoStream1->_simulcastStream, videoStream1->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(2, videoStream2->_simulcastStream, videoStream2->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(3, videoStream3->_simulcastStream, videoStream3->_secondarySimulcastStream);

    utils::StringBuilder<1024> message;
    _activeMediaList->makeUserMediaMapMessage(defaultLastN, 2, 0, _engineAudioStreams, _engineVideoStreams, message);

    const auto messageJson = nlohmann::json::parse(message.build());
    EXPECT_TRUE(endpointsContainsId(messageJson, "1"));
    EXPECT_FALSE(endpointsContainsId(messageJson, "2"));
    EXPECT_TRUE(endpointsContainsId(messageJson, "3"));
}

TEST_F(ActiveMediaListTest, userMediaMapContainsOnlyLastNItems)
{
    auto videoStream1 = addEngineVideoStream(1);
    auto videoStream2 = addEngineVideoStream(2);
    auto videoStream3 = addEngineVideoStream(3);
    auto videoStream4 = addEngineVideoStream(4);

    _activeMediaList->addVideoParticipant(1, videoStream1->_simulcastStream, videoStream1->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(2, videoStream2->_simulcastStream, videoStream2->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(3, videoStream3->_simulcastStream, videoStream3->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(4, videoStream4->_simulcastStream, videoStream4->_secondarySimulcastStream);

    utils::StringBuilder<1024> message;
    _activeMediaList->makeUserMediaMapMessage(defaultLastN, 2, 0, _engineAudioStreams, _engineVideoStreams, message);

    const auto messageJson = nlohmann::json::parse(message.build());
    EXPECT_TRUE(endpointsContainsId(messageJson, "1"));
    EXPECT_FALSE(endpointsContainsId(messageJson, "2"));
    EXPECT_TRUE(endpointsContainsId(messageJson, "3"));
    EXPECT_FALSE(endpointsContainsId(messageJson, "4"));
}

TEST_F(ActiveMediaListTest, userMediaMapContainsPinnedItem)
{
    auto videoStream1 = addEngineVideoStream(1);
    auto videoStream2 = addEngineVideoStream(2);
    auto videoStream3 = addEngineVideoStream(3);
    auto videoStream4 = addEngineVideoStream(4);

    _activeMediaList->addVideoParticipant(1, videoStream1->_simulcastStream, videoStream1->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(2, videoStream2->_simulcastStream, videoStream2->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(3, videoStream3->_simulcastStream, videoStream3->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(4, videoStream4->_simulcastStream, videoStream4->_secondarySimulcastStream);

    bridge::SimulcastLevel simulcastLevel;
    videoStream4->_videoPinSsrcs.pop(simulcastLevel);
    videoStream2->_pinSsrc.set(simulcastLevel);

    utils::StringBuilder<1024> message;
    _activeMediaList->makeUserMediaMapMessage(defaultLastN, 2, 4, _engineAudioStreams, _engineVideoStreams, message);

    const auto messageJson = nlohmann::json::parse(message.build());
    EXPECT_TRUE(endpointsContainsId(messageJson, "1"));
    EXPECT_FALSE(endpointsContainsId(messageJson, "2"));
    EXPECT_FALSE(endpointsContainsId(messageJson, "3"));
    EXPECT_TRUE(endpointsContainsId(messageJson, "4"));
}

TEST_F(ActiveMediaListTest, userMediaMapUpdatedWithDominantSpeaker)
{
    _activeMediaList->addAudioParticipant(1);
    _activeMediaList->addAudioParticipant(2);
    _activeMediaList->addAudioParticipant(3);
    _activeMediaList->addAudioParticipant(4);

    auto videoStream1 = addEngineVideoStream(1);
    auto videoStream2 = addEngineVideoStream(2);
    auto videoStream3 = addEngineVideoStream(3);
    auto videoStream4 = addEngineVideoStream(4);

    _activeMediaList->addVideoParticipant(1, videoStream1->_simulcastStream, videoStream1->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(2, videoStream2->_simulcastStream, videoStream2->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(3, videoStream3->_simulcastStream, videoStream3->_secondarySimulcastStream);
    _activeMediaList->addVideoParticipant(4, videoStream4->_simulcastStream, videoStream4->_secondarySimulcastStream);

    uint64_t timestamp = 0;
    zeroLevels(1);
    zeroLevels(2);
    zeroLevels(3);
    timestamp = switchDominantSpeaker(timestamp, 4);

    utils::StringBuilder<1024> message;
    _activeMediaList->makeUserMediaMapMessage(defaultLastN, 2, 0, _engineAudioStreams, _engineVideoStreams, message);
    printf("%s\n", message.get());

    const auto messageJson = nlohmann::json::parse(message.build());
    EXPECT_TRUE(endpointsContainsId(messageJson, "1"));
    EXPECT_FALSE(endpointsContainsId(messageJson, "2"));
    EXPECT_FALSE(endpointsContainsId(messageJson, "3"));
    EXPECT_TRUE(endpointsContainsId(messageJson, "4"));
}
