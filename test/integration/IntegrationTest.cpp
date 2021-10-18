#include "IntegrationTest.h"

void IntegrationTest::SetUp()
{
    using namespace std;

    utils::Time::initialize();

    idGenerator = make_unique<utils::IdGenerator>();
    ssrcGenerator = make_unique<utils::SsrcGenerator>();
    jobManager = make_unique<jobmanager::JobManager>();
    sslDtls = make_unique<transport::SslDtls>();
    transportFactory = make_unique<TransportFactoryStub>(*jobManager);
    engine = make_unique<bridge::Engine>();
    config = make_unique<config::Config>();
    poolAllocator = make_unique<memory::PacketPoolAllocator>(4096, "IntegrationTest");
}

// use this if you do not need to customize your mixermanager
void IntegrationTest::startDefaultMixerManager()
{
    mixerManager = std::make_unique<bridge::MixerManager>(*idGenerator,
        *ssrcGenerator,
        *jobManager,
        *transportFactory,
        *engine,
        *config,
        *poolAllocator);
}

void IntegrationTest::TearDown()
{
    mixerManager->stop();
    logger::info("IntegrationTest torn down", "IntegrationTest");
}

// Use this in disabled tests in case you want to test metrics during simulation test
void IntegrationTest::startHttpDaemon(int port)
{
    requestHandler = std::make_unique<bridge::RequestHandler>(*mixerManager, *sslDtls);
    httpd = std::make_unique<httpd::Httpd>(*requestHandler);
    httpd->start(port);
}
