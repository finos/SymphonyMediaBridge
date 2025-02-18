#include "bridge/engine/ActiveMediaList.h"
#include "bridge/engine/BarbellEndpointMap.h"
#include "bridge/engine/EngineAudioStream.h"
#include "bridge/engine/EngineVideoStream.h"
#include "bridge/engine/SimulcastStream.h"
#include "jobmanager/JobManager.h"
#include "nlohmann/json.hpp"
#include "test/bridge/ActiveMediaListTestLevels.h"
#include "test/bridge/DummyRtcTransport.h"
#include "utils/Format.h"
#include "utils/StringBuilder.h"
#include <gtest/gtest.h>
#include <memory>

namespace
{

static const uint32_t defaultLastN = 2;
static const uint32_t audioLastN = 3;

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

bool endpointsContainsId(const nlohmann::json& messageJson, const std::string& mediaModality, const char* id)
{
    const auto& endpoints = messageJson[(mediaModality + "-endpoints").c_str()];

    for (const auto& endpoint : endpoints)
    {
        if (endpoint["endpoint-id"].get<std::string>().compare(id) == 0)
        {
            return true;
        }
    }

    return false;
}

} // namespace

class ActiveMediaListTest : public ::testing::Test
{
public:
    ActiveMediaListTest() : _engineAudioStreams(16), _engineVideoStreams(16) {}

    const uint32_t AUDIO_REWRITE_COUNT = 5;
    const uint32_t VIDEO_REWRITE_COUNT = 10;

private:
    void SetUp() override
    {

        for (uint32_t i = 0; i < AUDIO_REWRITE_COUNT; ++i)
        {
            _audioSsrcs.push_back(i);
        }

        for (uint32_t i = 10; i < 10 + VIDEO_REWRITE_COUNT; ++i)
        {
            api::SsrcPair levels[3] = {{i * 3, i * 3 + 100}, {i * 3 + 1, i * 3 + 101}, {i * 3 + 2, i * 3 + 102}};
            _videoSsrcs.push_back(api::SimulcastGroup(levels));
        }

        for (uint32_t i = 20; i < 24; ++i)
        {
            _videoPinSsrcs.push_back(api::SsrcPair({i, i + 100}));
        }

        _timers = std::make_unique<jobmanager::TimerQueue>(4096 * 8);
        _jobManager = std::make_unique<jobmanager::JobManager>(*_timers);
        _jobQueue = std::make_unique<jobmanager::JobQueue>(*_jobManager);
        _transport = std::make_unique<DummyRtcTransport>(*_jobQueue);

        _activeMediaList =
            std::make_unique<bridge::ActiveMediaList>(1, _audioSsrcs, _videoSsrcs, defaultLastN, audioLastN, 18);

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

        for (auto& audioStreamEntry : _engineAudioStreams)
        {
            delete audioStreamEntry.second;
        }
        _engineAudioStreams.clear();
        _engineVideoStreams.clear();

        auto thread = std::make_unique<jobmanager::WorkerThread>(*_jobManager, true);
        _jobQueue.reset();
        _timers->stop();
        _jobManager->stop();
        thread->stop();
        _jobManager.reset();
    }

protected:
    bool _audioMapChanged = false;
    std::vector<uint32_t> _audioSsrcs;
    std::vector<api::SimulcastGroup> _videoSsrcs;
    std::vector<api::SsrcPair> _videoPinSsrcs;

    std::unique_ptr<jobmanager::TimerQueue> _timers;
    std::unique_ptr<jobmanager::JobManager> _jobManager;
    std::unique_ptr<jobmanager::JobQueue> _jobQueue;
    std::unique_ptr<DummyRtcTransport> _transport;
    concurrency::MpmcHashmap32<size_t, bridge::EngineAudioStream*> _engineAudioStreams;
    concurrency::MpmcHashmap32<size_t, bridge::EngineVideoStream*> _engineVideoStreams;

    std::unique_ptr<bridge::ActiveMediaList> _activeMediaList;

    bridge::EngineVideoStream* addEngineVideoStream(const size_t id)
    {
        bridge::SimulcastStream simulcastStream{0};
        simulcastStream.addLevel(
            bridge::SimulcastLevel{static_cast<uint32_t>(id), 100 + static_cast<uint32_t>(id), false});

        auto engineVideoStream = new bridge::EngineVideoStream(std::to_string(id),
            id,
            id,
            simulcastStream,
            utils::Optional<bridge::SimulcastStream>(),
            *_transport,
            bridge::RtpMap(),
            bridge::RtpMap(),
            bridge::SsrcWhitelist(),
            true,
            _videoPinSsrcs,
            0);

        _engineVideoStreams.emplace(id, engineVideoStream);
        return engineVideoStream;
    }

    void addEngineAudioStream(const size_t id)
    {
        std::vector<uint32_t> neighbours;
        auto engineAudioStream = std::make_unique<bridge::EngineAudioStream>(std::to_string(id),
            id,
            id,
            utils::Optional<uint32_t>(id),
            *_transport,
            bridge::MediaMode::SSRC_REWRITE,
            bridge::RtpMap(),
            bridge::RtpMap(),
            0,
            neighbours);

        _engineAudioStreams.emplace(id, engineAudioStream.release());
    }

    void switchDominantSpeaker(uint64_t& timestamp, const size_t endpointIdHash)
    {
        uint64_t newTimestamp = timestamp;

        for (uint32_t i = 0; i < 1000; ++i)
        {
            for (uint32_t j = 0; j < 100; ++j)
            {
                _activeMediaList->onNewAudioLevel(endpointIdHash, 1, false);
            }

            newTimestamp += 1000 * utils::Time::ms;
            bool dominantSpeakerChanged = false;
            bool videoMapChanged = false;
            _activeMediaList->process(newTimestamp, dominantSpeakerChanged, videoMapChanged, _audioMapChanged);
            if (dominantSpeakerChanged)
            {
                return;
            }
        }

        return;
    }

    void zeroLevels(const size_t endpointIdHash)
    {
        for (uint32_t i = 0; i < 100; ++i)
        {
            _activeMediaList->onNewAudioLevel(endpointIdHash, 126, false);
        }
    }

    void consumeLevels(uint64_t& timestamp)
    {
        timestamp += 250 * utils::Time::ms;
        bool dominantSpeakerChanged = false;
        bool videoMapChanged = false;
        _activeMediaList->process(timestamp, dominantSpeakerChanged, videoMapChanged, _audioMapChanged);
    }
};

TEST_F(ActiveMediaListTest, maxOneSwitchEveryTwoSeconds)
{
    uint64_t timestamp = 123456;
    const auto participant1 = 1;
    const auto participant2 = 2;

    // First added participant is set as dominant speaker, add this participant here so that switching will happen
    _activeMediaList->addAudioParticipant(3, "3");

    _activeMediaList->addAudioParticipant(participant1, "1");
    _activeMediaList->addAudioParticipant(participant2, "2");

    bool dominantSpeakerChanged = false;
    bool videoMapChanged = false;

    _activeMediaList->process(timestamp * utils::Time::ms, dominantSpeakerChanged, videoMapChanged, _audioMapChanged);

    for (auto i = 0; i < 30; ++i)
    {
        _activeMediaList->onNewAudioLevel(participant1, 64 + (i % 40), false);
        _activeMediaList->onNewAudioLevel(participant2, 96 + (i % 5), false);
    }
    timestamp += 200;
    _activeMediaList->process(timestamp * utils::Time::ms, dominantSpeakerChanged, videoMapChanged, _audioMapChanged);
    EXPECT_TRUE(dominantSpeakerChanged);
    EXPECT_EQ(_activeMediaList->getDominantSpeaker(), 1);

    for (auto i = 0; i < 199; ++i)
    {
        _activeMediaList->onNewAudioLevel(participant1, 96 + (i % 5), false);
        _activeMediaList->onNewAudioLevel(participant2, 64 + (i % 40), false);
        timestamp += 10;

        _activeMediaList->process(timestamp * utils::Time::ms,
            dominantSpeakerChanged,
            videoMapChanged,
            _audioMapChanged);

        EXPECT_FALSE(dominantSpeakerChanged);
    }

    _activeMediaList->onNewAudioLevel(participant1, 96, false);
    _activeMediaList->onNewAudioLevel(participant2, 64, false);
    timestamp += 500;
    _activeMediaList->process(timestamp * utils::Time::ms, dominantSpeakerChanged, videoMapChanged, _audioMapChanged);
    EXPECT_TRUE(dominantSpeakerChanged);

    _activeMediaList->removeAudioParticipant(participant1);
    _activeMediaList->removeAudioParticipant(participant2);
}

TEST_F(ActiveMediaListTest, audioParticipantsAddedToAudioRewriteMap)
{
    _activeMediaList->addAudioParticipant(1, "1");
    _activeMediaList->addAudioParticipant(2, "2");
    _activeMediaList->addAudioParticipant(3, "3");

    const auto& audioRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(1));
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(2));
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(3));
}

TEST_F(ActiveMediaListTest, audioParticipantsNotAddedToFullAudioRewriteMap)
{

    for (int i = 1; i < 7; ++i)
    {
        _activeMediaList->addAudioParticipant(i, std::to_string(i).c_str());
    }

    const auto& audioRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();

    for (int i = 1; i < 6; ++i)
    {
        EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(i));
    }
}

TEST_F(ActiveMediaListTest, activeAudioParticipantIsSwitchedIn)
{
    _activeMediaList->addAudioParticipant(1, "1");
    _activeMediaList->addAudioParticipant(2, "2");
    _activeMediaList->addAudioParticipant(3, "3");
    _activeMediaList->addAudioParticipant(4, "4");
    _activeMediaList->addAudioParticipant(5, "5");
    _activeMediaList->addAudioParticipant(6, "6");

    const auto& audioRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
    EXPECT_EQ(audioRewriteMap.end(), audioRewriteMap.find(6));
    EXPECT_EQ(audioLastN + 2, audioRewriteMap.size());

    _activeMediaList->onNewAudioLevel(6, 10, false);

    bool dominantSpeakerChanged = false;
    bool videoMapChanged = false;

    _activeMediaList->process(1000 * utils::Time::ms, dominantSpeakerChanged, videoMapChanged, _audioMapChanged);

    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(6));
    EXPECT_EQ(5, audioRewriteMap.size());
}

TEST_F(ActiveMediaListTest, activeAudioParticipantIsSwitchedInEvenIfNotMostDominant)
{
    const size_t numParticipants = 10;
    for (size_t i = 1; i <= numParticipants; ++i)
    {
        _activeMediaList->addAudioParticipant(i, std::to_string(i).c_str());
        for (const auto element : ActiveMediaListTestLevels::silence)
        {
            _activeMediaList->onNewAudioLevel(i, element, false);
        }
    }

    const auto& audioRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
    EXPECT_EQ(audioRewriteMap.end(), audioRewriteMap.find(6));

    bool dominantSpeakerChanged = false;
    bool videoMapChanged = false;

    _activeMediaList->process(1000 * utils::Time::ms, dominantSpeakerChanged, videoMapChanged, _audioMapChanged);

    for (const auto element : ActiveMediaListTestLevels::longUtterance)
    {
        _activeMediaList->onNewAudioLevel(1, element, false);
    }

    for (const auto element : ActiveMediaListTestLevels::longUtterance)
    {
        _activeMediaList->onNewAudioLevel(2, element, false);
    }

    for (const auto element : ActiveMediaListTestLevels::shortUtterance)
    {
        _activeMediaList->onNewAudioLevel(6, element, false);
    }

    _activeMediaList->process(2000 * utils::Time::ms, dominantSpeakerChanged, videoMapChanged, _audioMapChanged);

    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(6));
    EXPECT_EQ(5, audioRewriteMap.size());
}

TEST_F(ActiveMediaListTest, activeAudioParticipantIsSwitchedInEvenIfNotMostDominantSmallList)
{
    const size_t numParticipants = 2;
    for (size_t i = 1; i <= numParticipants; ++i)
    {
        _activeMediaList->addAudioParticipant(i, std::to_string(i).c_str());
        for (const auto element : ActiveMediaListTestLevels::silence)
        {
            _activeMediaList->onNewAudioLevel(i, element, false);
        }
    }

    bool dominantSpeakerChanged = false;
    bool videoMapChanged = false;

    _activeMediaList->process(1000 * utils::Time::ms, dominantSpeakerChanged, videoMapChanged, _audioMapChanged);

    for (const auto element : ActiveMediaListTestLevels::longUtterance)
    {
        _activeMediaList->onNewAudioLevel(1, element, false);
    }

    for (const auto element : ActiveMediaListTestLevels::shortUtterance)
    {
        _activeMediaList->onNewAudioLevel(2, element, false);
    }

    _activeMediaList->process(2000 * utils::Time::ms, dominantSpeakerChanged, videoMapChanged, _audioMapChanged);

    const auto& audioRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(2));
    EXPECT_EQ(2, audioRewriteMap.size());
}

TEST_F(ActiveMediaListTest, activeAudioParticipantIsSwitchedInEvenIfNotMostDominantSmallLastN)
{
    auto smallActiveMediaList = std::make_unique<bridge::ActiveMediaList>(1, _audioSsrcs, _videoSsrcs, 1, 3, 18);

    smallActiveMediaList->addAudioParticipant(1, "1");
    smallActiveMediaList->addAudioParticipant(2, "2");
    smallActiveMediaList->addAudioParticipant(3, "3");
    smallActiveMediaList->addAudioParticipant(4, "4");

    for (const auto element : ActiveMediaListTestLevels::longUtterance)
    {
        smallActiveMediaList->onNewAudioLevel(1, element, false);
    }

    for (const auto element : ActiveMediaListTestLevels::longUtterance)
    {
        smallActiveMediaList->onNewAudioLevel(2, element, false);
    }

    for (const auto element : ActiveMediaListTestLevels::silence)
    {
        smallActiveMediaList->onNewAudioLevel(3, element, false);
    }

    for (const auto element : ActiveMediaListTestLevels::shortUtterance)
    {
        smallActiveMediaList->onNewAudioLevel(4, element, false);
    }

    bool dominantSpeakerChanged = false;
    bool videoMapChanged = false;
    smallActiveMediaList->process(1000 * utils::Time::ms, dominantSpeakerChanged, videoMapChanged, _audioMapChanged);

    const auto& audioRewriteMap = smallActiveMediaList->getAudioSsrcRewriteMap();
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(1));
}

TEST_F(ActiveMediaListTest, videoParticipantsAddedToVideoRewriteMap)
{
    auto videoStream1 = addEngineVideoStream(1);
    auto videoStream2 = addEngineVideoStream(2);
    auto videoStream3 = addEngineVideoStream(3);

    _activeMediaList->addVideoParticipant(1,
        videoStream1->simulcastStream,
        videoStream1->secondarySimulcastStream,
        "1");
    _activeMediaList->addVideoParticipant(2,
        videoStream2->simulcastStream,
        videoStream2->secondarySimulcastStream,
        "2");
    _activeMediaList->addVideoParticipant(3,
        videoStream3->simulcastStream,
        videoStream3->secondarySimulcastStream,
        "3");

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

    _activeMediaList->addVideoParticipant(1,
        videoStream1->simulcastStream,
        videoStream1->secondarySimulcastStream,
        "1");
    _activeMediaList->addVideoParticipant(2,
        videoStream2->simulcastStream,
        videoStream2->secondarySimulcastStream,
        "2");
    _activeMediaList->addVideoParticipant(3,
        videoStream3->simulcastStream,
        videoStream3->secondarySimulcastStream,
        "3");
    _activeMediaList->addVideoParticipant(4,
        videoStream4->simulcastStream,
        videoStream4->secondarySimulcastStream,
        "4");

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

    _activeMediaList->addVideoParticipant(1,
        videoStream1->simulcastStream,
        videoStream1->secondarySimulcastStream,
        "1");
    _activeMediaList->addVideoParticipant(2,
        videoStream2->simulcastStream,
        videoStream2->secondarySimulcastStream,
        "2");
    _activeMediaList->addVideoParticipant(3,
        videoStream3->simulcastStream,
        videoStream3->secondarySimulcastStream,
        "3");

    utils::StringBuilder<1024> message;
    _activeMediaList->makeUserMediaMapMessage(defaultLastN, 2, 0, _engineVideoStreams, message);

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

    _activeMediaList->addVideoParticipant(1,
        videoStream1->simulcastStream,
        videoStream1->secondarySimulcastStream,
        "1");
    _activeMediaList->addVideoParticipant(2,
        videoStream2->simulcastStream,
        videoStream2->secondarySimulcastStream,
        "2");
    _activeMediaList->addVideoParticipant(3,
        videoStream3->simulcastStream,
        videoStream3->secondarySimulcastStream,
        "3");
    _activeMediaList->addVideoParticipant(4,
        videoStream4->simulcastStream,
        videoStream4->secondarySimulcastStream,
        "4");

    utils::StringBuilder<1024> message;
    _activeMediaList->makeUserMediaMapMessage(defaultLastN, 2, 0, _engineVideoStreams, message);

    const auto messageJson = nlohmann::json::parse(message.build());
    EXPECT_TRUE(endpointsContainsId(messageJson, "1"));
    EXPECT_FALSE(endpointsContainsId(messageJson, "2"));
    EXPECT_TRUE(endpointsContainsId(messageJson, "3"));
    EXPECT_FALSE(endpointsContainsId(messageJson, "4"));

    utils::StringBuilder<1024> bbMessage;
    bridge::engine::EndpointMembershipsMap noMembership(8);
    _activeMediaList->makeBarbellUserMediaMapMessage(bbMessage, noMembership, true);
    printf("%s\n", bbMessage.get());
    const auto barbellJson = nlohmann::json::parse(bbMessage.build());
    EXPECT_TRUE(endpointsContainsId(barbellJson, "video", "1"));
    EXPECT_TRUE(endpointsContainsId(barbellJson, "video", "2"));
    EXPECT_TRUE(endpointsContainsId(barbellJson, "video", "3"));
    EXPECT_FALSE(endpointsContainsId(barbellJson, "video", "4")); // will not fit within default lastN + 1
}

TEST_F(ActiveMediaListTest, userMediaMapContainsPinnedItem)
{
    auto videoStream1 = addEngineVideoStream(1);
    auto videoStream2 = addEngineVideoStream(2);
    auto videoStream3 = addEngineVideoStream(3);
    auto videoStream4 = addEngineVideoStream(4);

    _activeMediaList->addVideoParticipant(1,
        videoStream1->simulcastStream,
        videoStream1->secondarySimulcastStream,
        "1");
    _activeMediaList->addVideoParticipant(2,
        videoStream2->simulcastStream,
        videoStream2->secondarySimulcastStream,
        "2");
    _activeMediaList->addVideoParticipant(3,
        videoStream3->simulcastStream,
        videoStream3->secondarySimulcastStream,
        "3");
    _activeMediaList->addVideoParticipant(4,
        videoStream4->simulcastStream,
        videoStream4->secondarySimulcastStream,
        "4");

    bridge::SimulcastLevel simulcastLevel;
    videoStream4->videoPinSsrcs.pop(simulcastLevel);
    videoStream2->pinSsrc.set(simulcastLevel);

    utils::StringBuilder<1024> message;
    _activeMediaList->makeUserMediaMapMessage(defaultLastN, 2, 4, _engineVideoStreams, message);

    const auto messageJson = nlohmann::json::parse(message.build());
    EXPECT_TRUE(endpointsContainsId(messageJson, "1"));
    EXPECT_FALSE(endpointsContainsId(messageJson, "2"));
    EXPECT_FALSE(endpointsContainsId(messageJson, "3"));
    EXPECT_TRUE(endpointsContainsId(messageJson, "4"));
}

TEST_F(ActiveMediaListTest, userMediaMapUpdatedWithDominantSpeaker)
{
    _activeMediaList->addAudioParticipant(1, "1");
    _activeMediaList->addAudioParticipant(2, "2");
    _activeMediaList->addAudioParticipant(3, "3");
    _activeMediaList->addAudioParticipant(4, "4");

    auto videoStream1 = addEngineVideoStream(1);
    auto videoStream2 = addEngineVideoStream(2);
    auto videoStream3 = addEngineVideoStream(3);
    auto videoStream4 = addEngineVideoStream(4);

    addEngineAudioStream(1);
    addEngineAudioStream(2);
    addEngineAudioStream(3);
    addEngineAudioStream(4);

    _activeMediaList->addVideoParticipant(1,
        videoStream1->simulcastStream,
        videoStream1->secondarySimulcastStream,
        "1");
    _activeMediaList->addVideoParticipant(2,
        videoStream2->simulcastStream,
        videoStream2->secondarySimulcastStream,
        "2");
    _activeMediaList->addVideoParticipant(3,
        videoStream3->simulcastStream,
        videoStream3->secondarySimulcastStream,
        "3");
    _activeMediaList->addVideoParticipant(4,
        videoStream4->simulcastStream,
        videoStream4->secondarySimulcastStream,
        "4");

    uint64_t timestamp = 0;
    zeroLevels(1);
    zeroLevels(2);
    zeroLevels(3);
    consumeLevels(timestamp);
    switchDominantSpeaker(timestamp, 4);

    utils::StringBuilder<1024> message;
    _activeMediaList->makeUserMediaMapMessage(defaultLastN, 2, 0, _engineVideoStreams, message);
    printf("%s\n", message.get());

    const auto messageJson = nlohmann::json::parse(message.build());
    EXPECT_TRUE(endpointsContainsId(messageJson, "1"));
    EXPECT_FALSE(endpointsContainsId(messageJson, "2"));
    EXPECT_FALSE(endpointsContainsId(messageJson, "3"));
    EXPECT_TRUE(endpointsContainsId(messageJson, "4"));

    utils::StringBuilder<1024> bbMessage;
    bridge::engine::EndpointMembershipsMap noMembership(8);
    _activeMediaList->makeBarbellUserMediaMapMessage(bbMessage, noMembership, true);
    printf("%s\n", bbMessage.get());
    const auto barbellJson = nlohmann::json::parse(bbMessage.build());
    EXPECT_TRUE(endpointsContainsId(barbellJson, "video", "1"));
    EXPECT_TRUE(endpointsContainsId(barbellJson, "video", "2"));
    EXPECT_FALSE(endpointsContainsId(barbellJson, "video", "3"));
    EXPECT_TRUE(endpointsContainsId(barbellJson, "video", "4"));

    EXPECT_TRUE(endpointsContainsId(barbellJson, "audio", "1"));
    EXPECT_TRUE(endpointsContainsId(barbellJson, "audio", "2"));
    EXPECT_TRUE(endpointsContainsId(barbellJson, "audio", "3"));
    EXPECT_TRUE(endpointsContainsId(barbellJson, "audio", "4"));
}

TEST_F(ActiveMediaListTest, mutedAreNotSwitchedIn)
{
    const int memberCount = 9;
    for (int i = 1; i < memberCount; ++i)
    {
        _activeMediaList->addAudioParticipant(i, std::to_string(i).c_str());
    }

    const auto& audioRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
    EXPECT_EQ(audioRewriteMap.end(), audioRewriteMap.find(6));
    EXPECT_EQ(audioLastN + 2, audioRewriteMap.size());
    uint64_t timestamp = utils::Time::sec;
    // 2-5 are not sending media at all.
    // 6-8 behave as muted, sending level 0x7F
    for (int i = 0; i < 95; ++i)
    {
        timestamp += 20;
        _activeMediaList->onNewAudioLevel(1, 50, false);
        _activeMediaList->onNewAudioLevel(6, 0x7F, false);
        _activeMediaList->onNewAudioLevel(7, 0x7F, false);
        _activeMediaList->onNewAudioLevel(8, 0x7F, false);
        bool dominantSpeakerChanged = false;
        bool videoMapChanged = false;
        bool audioMapChanged = false;
        _activeMediaList->process(timestamp * utils::Time::ms,
            dominantSpeakerChanged,
            videoMapChanged,
            audioMapChanged);
    }

    for (int i = 1 + audioLastN + 2; i < memberCount; ++i)
    {
        EXPECT_EQ(audioRewriteMap.end(), audioRewriteMap.find(i));
    }
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(1));
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(2));
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(3));

    EXPECT_EQ(5, audioRewriteMap.size());
}

TEST_F(ActiveMediaListTest, mutedOutage)
{
    const int memberCount = 9;
    for (int i = 1; i < memberCount; ++i)
    {
        _activeMediaList->addAudioParticipant(i, std::to_string(i).c_str());
    }

    const auto& audioRewriteMap = _activeMediaList->getAudioSsrcRewriteMap();
    EXPECT_EQ(audioRewriteMap.end(), audioRewriteMap.find(6));
    EXPECT_EQ(audioLastN + 2, audioRewriteMap.size());
    uint64_t timestamp = utils::Time::sec;
    // 2-5 are not sending media at all.
    // 6-8 behave as muted, sending level 0x7F
    for (int i = 0; i < 5950; ++i)
    {
        timestamp += 10;
        if (!(i & 1) && !(i > 500 && i < 540) && !(i > 900 && i < 940))
        {
            _activeMediaList->onNewAudioLevel(1, 50, false);
            _activeMediaList->onNewAudioLevel(6, 0x7F, false);
            _activeMediaList->onNewAudioLevel(7, 0x7F, false);
            _activeMediaList->onNewAudioLevel(8, 0x7F, false);
        }
        bool dominantSpeakerChanged = false;
        bool videoMapChanged = false;
        bool audioMapChanged = false;
        _activeMediaList->process(timestamp * utils::Time::ms,
            dominantSpeakerChanged,
            videoMapChanged,
            audioMapChanged);
    }

    for (int i = 1 + audioLastN + 2; i < memberCount; ++i)
    {
        EXPECT_EQ(audioRewriteMap.end(), audioRewriteMap.find(i));
    }
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(1));
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(2));
    EXPECT_NE(audioRewriteMap.end(), audioRewriteMap.find(3));

    EXPECT_EQ(5, audioRewriteMap.size());
}
