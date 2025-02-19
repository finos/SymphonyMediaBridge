
#include "bridge/ApiRequestHandler.h"
#include "bridge/Mixer.h"
#include "mocks/EngineMixerSpy.h"
#include "mocks/MixerManagerSpy.h"
#include "mocks/RtcTransportMock.h"
#include "transport/ProbeServer.h"
#include "transport/dtls/SslDtls.h"
#include <gtest/gtest.h>

using namespace bridge;
using namespace test;
using namespace testing;

struct JobManagerProcessor
{
    JobManagerProcessor(jobmanager::JobManager& jobManager) : jobManager(jobManager) {}
    ~JobManagerProcessor() { dropAll(); }

    size_t runPendingJobs()
    {
        size_t processed = 0;
        for (auto pendingJobIt = pendingJobs.begin(); pendingJobIt != pendingJobs.end();)
        {
            ++processed;
            if (!(*pendingJobIt)->runStep())
            {
                jobManager.freeJob(*pendingJobIt);
                pendingJobIt = pendingJobs.erase(pendingJobIt);
            }
            else
            {
                ++pendingJobIt;
            }
        }

        return processed;
    }

    size_t processAll()
    {
        size_t processed = runPendingJobs();

        jobmanager::MultiStepJob* job;
        while ((job = jobManager.pop()) != nullptr)
        {
            ++processed;
            if (!job->runStep())
            {
                jobManager.freeJob(job);
            }
            else
            {
                pendingJobs.push_back(job);
            }
        }

        processed += runPendingJobs();
        return processed;
    }

    void dropAll()
    {
        jobmanager::MultiStepJob* job;
        while ((job = jobManager.pop()) != nullptr)
        {
            jobManager.freeJob(job);
        }

        while (!pendingJobs.empty())
        {
            jobManager.freeJob(pendingJobs.back());
            pendingJobs.pop_back();
        }
    }

    jobmanager::JobManager& getJobManager() { return jobManager; }

private:
    jobmanager::JobManager& jobManager;
    std::vector<jobmanager::MultiStepJob*> pendingJobs;
};

class ApiRequestHandlerTest : public ::testing::Test
{
public:
    void SetUp() override
    {
        _transportMock = std::make_shared<NiceMock<RtcTransportMock>>();

        _mixerManagerSpyResources = MixerManagerSpy::MixerManagerSpyResources::makeDefault();

        _probeServerJobManager = std::make_unique<jobmanager::JobManager>(*_mixerManagerSpyResources->timerQueue, 128);

        // const ice::IceConfig& iceConfig, const config::Config& config, jobmanager::JobManager& jobmanager
        _probeServer = std::make_unique<transport::ProbeServer>(_iceConfig,
            _mixerManagerSpyResources->config,
            *_probeServerJobManager,
            std::thread());

        _mixerManagerSpy = std::make_unique<MixerManagerSpy>(*_mixerManagerSpyResources);

        _managerQueueProcessor = std::make_unique<JobManagerProcessor>(*_mixerManagerSpyResources->jobManager);
        _probeServerProcessor = std::make_unique<JobManagerProcessor>(*_probeServerJobManager);

        _mixerManagerSpyResources->transportFactoryMock->willReturnByDefaultForAll(_transportMock);

        ON_CALL(*_transportMock, isGatheringComplete()).WillByDefault(Return(true));
    }

    void TearDown() override
    {
        _managerQueueProcessor->dropAll();
        _probeServerProcessor->dropAll();
        std::atomic<bool> running = true;

        // Need this thread to finalize JobQueue on PrebeServer destructor
        auto thread = std::thread([this, &running]() {
            while (running)
            {
                _probeServerProcessor->processAll();
                usleep(10 * 1000);
            }
        });

        _mixerManagerSpy = nullptr;
        _probeServer = nullptr;
        _transportMock = nullptr;

        running = false;
        thread.join();

        _probeServerProcessor = nullptr;
        _managerQueueProcessor = nullptr;

        _mixerManagerSpyResources = nullptr;
        _probeServerJobManager = nullptr;
    }

    ApiRequestHandler createApiRequestHandler()
    {
        return ApiRequestHandler(*_mixerManagerSpy, _sslDtls, *_probeServer, _mixerManagerSpyResources->config);
    }

    std::string createConference(ApiRequestHandler& handler, const char* conferenceBody)
    {

        httpd::Request request("POST", "/conferences");
        request.body.append(conferenceBody, strlen(conferenceBody));

        const auto response = handler.onRequest(request);

        EXPECT_EQ(httpd::StatusCode::OK, response.statusCode);

        const auto responseJson = response.getBodyAsJson();
        auto conferenceId = responseJson["id"].get<std::string>();

        EXPECT_EQ(false, conferenceId.empty());

        return conferenceId;
    }

protected:
    ice::IceConfig _iceConfig;
    std::shared_ptr<RtcTransportMock> _transportMock;
    std::unique_ptr<MixerManagerSpy::MixerManagerSpyResources> _mixerManagerSpyResources;
    std::unique_ptr<jobmanager::JobManager> _probeServerJobManager;
    std::unique_ptr<transport::ProbeServer> _probeServer;
    std::unique_ptr<MixerManagerSpy> _mixerManagerSpy;
    transport::SslDtls _sslDtls;
    std::unique_ptr<JobManagerProcessor> _managerQueueProcessor;
    std::unique_ptr<JobManagerProcessor> _probeServerProcessor;
};

TEST_F(ApiRequestHandlerTest, createConference)
{
    auto requestHandler = createApiRequestHandler();

    const char* body = R"({
     "last-n": 9
     })";

    httpd::Request request("POST", "/conferences");
    request.body.append(body, strlen(body));

    const auto response = requestHandler.onRequest(request);

    EXPECT_EQ(httpd::StatusCode::OK, response.statusCode);
    EXPECT_EQ(false, response.body.empty());

    const auto responseJson = response.getBodyAsJson();
    const auto idIt = responseJson.find("id");
    ASSERT_NE(responseJson.end(), idIt);

    auto conferenceId = idIt->get<std::string>();
    ASSERT_EQ(false, conferenceId.empty());

    Mixer* mixer = nullptr;

    auto lock = _mixerManagerSpy->getMixer(conferenceId, mixer);

    ASSERT_NE(nullptr, mixer);

    ASSERT_EQ(true, mixer->hasVideoEnabled());
}

TEST_F(ApiRequestHandlerTest, createConferenceVideoEnabledFalse)
{
    auto requestHandler = createApiRequestHandler();

    const char* body = R"({
     "last-n": 9,
     "enable-video": false
     })";

    httpd::Request request("POST", "/conferences");
    request.body.append(body, strlen(body));

    const auto response = requestHandler.onRequest(request);

    EXPECT_EQ(httpd::StatusCode::OK, response.statusCode);
    EXPECT_EQ(false, response.body.empty());

    const auto responseJson = response.getBodyAsJson();
    const auto idIt = responseJson.find("id");
    ASSERT_NE(responseJson.end(), idIt);

    auto conferenceId = idIt->get<std::string>();
    ASSERT_EQ(false, conferenceId.empty());

    Mixer* mixer = nullptr;

    auto lock = _mixerManagerSpy->getMixer(conferenceId, mixer);

    ASSERT_NE(nullptr, mixer);

    ASSERT_EQ(false, mixer->hasVideoEnabled());

    EngineMixerSpy* engineMixerSpy = EngineMixerSpy::spy(mixer->getEngineMixer());

    // Check all video related things have zero capacity
    ASSERT_EQ(0, engineMixerSpy->spyEngineVideoStreams().capacity());
    ASSERT_EQ(0, engineMixerSpy->spyIncomingForwarderVideoRtp().capacity());

    // Check the other fields are using the write capacity values
    ASSERT_EQ(EngineMixerSpy::maxPendingRtcpPacketsVideoDisabled, engineMixerSpy->spyIncomingRtcp().capacity());
    ASSERT_EQ(EngineMixerSpy::maxSsrcsVideoDisabled, engineMixerSpy->spySsrcInboundContexts().capacity());
    ASSERT_EQ(EngineMixerSpy::maxSsrcsVideoDisabled, engineMixerSpy->spyAllSsrcInboundContexts().capacity());
}

TEST_F(ApiRequestHandlerTest, createConferenceVideoEnableTrue)
{
    auto requestHandler = createApiRequestHandler();

    const char* body = R"({
     "last-n": 9,
     "enable-video": true
     })";

    httpd::Request request("POST", "/conferences");
    request.body.append(body, strlen(body));

    const auto response = requestHandler.onRequest(request);

    EXPECT_EQ(httpd::StatusCode::OK, response.statusCode);
    EXPECT_EQ(false, response.body.empty());

    const auto responseJson = response.getBodyAsJson();
    const auto idIt = responseJson.find("id");
    ASSERT_NE(responseJson.end(), idIt);

    auto conferenceId = idIt->get<std::string>();
    ASSERT_EQ(false, conferenceId.empty());

    Mixer* mixer = nullptr;

    auto lock = _mixerManagerSpy->getMixer(conferenceId, mixer);

    ASSERT_NE(nullptr, mixer);

    ASSERT_EQ(true, mixer->hasVideoEnabled());

    EngineMixerSpy* engineMixerSpy = EngineMixerSpy::spy(mixer->getEngineMixer());

    ASSERT_EQ(EngineMixerSpy::maxStreamsPerModality, engineMixerSpy->spyEngineVideoStreams().capacity());
    ASSERT_EQ(EngineMixerSpy::maxPendingPackets, engineMixerSpy->spyIncomingForwarderVideoRtp().capacity());

    ASSERT_EQ(EngineMixerSpy::maxPendingRtcpPackets, engineMixerSpy->spyIncomingRtcp().capacity());
    ASSERT_EQ(EngineMixerSpy::maxSsrcs, engineMixerSpy->spySsrcInboundContexts().capacity());
    ASSERT_EQ(EngineMixerSpy::maxSsrcs, engineMixerSpy->spyAllSsrcInboundContexts().capacity());
}

TEST_F(ApiRequestHandlerTest, allocateEndpointWithVideoFieldWhenVideoIsDisabledShouldNotReturnVideo)
{
    auto requestHandler = createApiRequestHandler();

    const auto conferenceId = createConference(requestHandler, R"({
     "last-n": 9,
     "enable-video": false
     })");

    const char* body = R"({
        "action": "allocate",
        "bundle-transport": {
            "ice": true,
            "dtls": true
        },
        "audio": {
            "relay-type": "ssrc-rewrite"
        },
        "video": {
            "relay-type": "ssrc-rewrite"
        },
        "data": {
        }
    })";

    const auto urlPath = std::string("/conferences/").append(conferenceId).append("/session0");

    httpd::Request request("POST", urlPath.c_str());
    request.body.append(body, strlen(body));

    const auto response = requestHandler.onRequest(request);

    EXPECT_EQ(httpd::StatusCode::OK, response.statusCode);
    EXPECT_EQ(false, response.body.empty());

    const auto responseJson = response.getBodyAsJson();

    auto audioIt = responseJson.find("audio");
    auto bundleTransportIt = responseJson.find("bundle-transport");
    auto data = responseJson.find("data");

    // Check if answer contains the only 3 expected (audio, bundle-transport and data )
    EXPECT_EQ(3, responseJson.size());
    EXPECT_NE(responseJson.end(), audioIt);
    EXPECT_NE(responseJson.end(), bundleTransportIt);
    EXPECT_NE(responseJson.end(), data);

    // Because this test is explicit to test that the video is not present. Let's check it explicitly
    EXPECT_EQ(responseJson.end(), responseJson.find("video"));
}

TEST_F(ApiRequestHandlerTest, allocateEndpointWithVideoFieldWhenVideoIsEnableShouldReturnVideo)
{
    auto requestHandler = createApiRequestHandler();

    const auto conferenceId = createConference(requestHandler, R"({
     "last-n": 9,
     "enable-video": true
     })");

    const char* body = R"({
        "action": "allocate",
        "bundle-transport": {
            "ice": true,
            "dtls": true
        },
        "audio": {
            "relay-type": "ssrc-rewrite"
        },
        "video": {
            "relay-type": "ssrc-rewrite"
        },
        "data": {
        }
    })";

    const auto urlPath = std::string("/conferences/").append(conferenceId).append("/session0");

    httpd::Request request("POST", urlPath.c_str());
    request.body.append(body, strlen(body));

    const auto response = requestHandler.onRequest(request);

    EXPECT_EQ(httpd::StatusCode::OK, response.statusCode);
    EXPECT_EQ(false, response.body.empty());

    const auto responseJson = response.getBodyAsJson();

    EXPECT_EQ(4, responseJson.size());
    EXPECT_NE(responseJson.end(), responseJson.find("audio"));
    EXPECT_NE(responseJson.end(), responseJson.find("bundle-transport"));
    EXPECT_NE(responseJson.end(), responseJson.find("video"));
    EXPECT_NE(responseJson.end(), responseJson.find("data"));
}
