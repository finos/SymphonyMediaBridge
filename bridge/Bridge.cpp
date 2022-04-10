#include "Bridge.h"
#include "aws/AwsHarvester.h"
#include "bridge/ApiRequestHandler.h"
#include "bridge/MixerManager.h"
#include "bridge/engine/Engine.h"
#include "httpd/Httpd.h"
#include "jobmanager/JobManager.h"
#include "jobmanager/WorkerThread.h"
#include "transport/RtcePoll.h"
#include "transport/TransportFactory.h"
#include "transport/dtls/SrtpClientFactory.h"
#include "transport/dtls/SslDtls.h"
#include "utils/IdGenerator.h"
#include "utils/SsrcGenerator.h"

namespace bridge
{

std::vector<transport::SocketAddress> gatherInterfaces(const config::Config& config)
{
    auto defaultIpName = config.ice.preferredIp.get();
    const auto useLocal = defaultIpName == "localhost" || defaultIpName == "127.0.0.1";
    auto interfaces = transport::SocketAddress::activeInterfaces(useLocal, false);
    auto defaultInterface = transport::SocketAddress::parse(defaultIpName);

    std::sort(interfaces.begin(),
        interfaces.end(),
        [defaultInterface, defaultIpName](const transport::SocketAddress& a, const transport::SocketAddress& b) {
            if (a.getFamily() != b.getFamily())
            {
                return a.getFamily() == AF_INET;
            }
            if (a.equalsIp(defaultInterface) || a.getName() == defaultIpName)
            {
                return true;
            }
            return a.toString() < b.toString();
        });

    if (!config.ice.enableIpv6)
    {
        const auto newEnd = std::find_if(interfaces.begin(), interfaces.end(), [](const transport::SocketAddress& a) {
            return a.getFamily() == AF_INET6;
        });
        if (newEnd != interfaces.end())
        {
            interfaces.erase(newEnd, interfaces.end());
        }
    }
    return interfaces;
}

Bridge::Bridge(const config::Config& config)
    : _initialized(false),
      _config(config),
      _idGenerator(std::make_unique<utils::IdGenerator>()),
      _ssrcGenerator(std::make_unique<utils::SsrcGenerator>()),
      _jobManager(std::make_unique<jobmanager::JobManager>()),
      _sslDtls(std::make_unique<transport::SslDtls>()),
      _network(transport::createRtcePoll()),
      _mainPacketAllocator(std::make_unique<memory::PacketPoolAllocator>(32 * 1024, "main")),
      _sendPacketAllocator(std::make_unique<memory::PacketPoolAllocator>(128 * 1024, "send")),
      _audioPacketAllocator(std::make_unique<memory::AudioPacketPoolAllocator>(4 * 1024, "audio")),
      _engine(std::make_unique<bridge::Engine>())
{
}

Bridge::~Bridge()
{
    if (_mixerManager)
    {
        _mixerManager->stop();
    }

    _engine->stop();

    _transportFactory.reset(nullptr);

    if (_jobManager)
    {
        _jobManager->stop();
    }
    logger::info("JobManager stopped", "main");

    uint32_t n = 0;
    for (auto& workerThread : _workerThreads)
    {
        workerThread->stop();
        logger::info("stopped workerThread %d", "main", n++);
    }
}

void Bridge::initialize()
{
    _localInterfaces = gatherInterfaces(_config);
    if (_localInterfaces.size() == 0)
    {
        return;
    }
    if (_config.ice.useAwsInfo)
    {
        _iceConfig.publicIpv4 = aws::getPublicIpv4();
        if (!_iceConfig.publicIpv4.empty())
        {
            logger::info("Detected AWS public ip %s", "main", _iceConfig.publicIpv4.ipToString().c_str());
        }
        else
        {
            logger::error("Failed to harvest public ip. Stopping", "main");
            return;
        }
    }
    else if (!_config.ice.publicIpv4.get().empty())
    {
        _iceConfig.publicIpv4 = transport::SocketAddress::parse(_config.ice.publicIpv4);
    }
    if (!_config.ice.publicIpv6.get().empty())
    {
        _iceConfig.publicIpv6 = transport::SocketAddress::parse(_config.ice.publicIpv6);
    }

    startWorkerThreads();

    if (!_sslDtls->isInitialized())
    {
        logger::error("Failed to init SSL. Stopping", "main");
        return;
    }

    _rateControllerConfig.enabled = _config.rctl.enable;
    _rateControllerConfig.ipOverhead = _config.ipOverhead;
    _rateControllerConfig.bandwidthCeilingKbps = _config.rctl.ceiling;
    _rateControllerConfig.bandwidthFloorKbps = _config.rctl.floor;
    _rateControllerConfig.initialEstimateKbps = _config.rctl.initialEstimate;
    _rateControllerConfig.debugLog = _config.rctl.debugLog;

    _srtpClientFactory = std::make_unique<transport::SrtpClientFactory>(*_sslDtls);
    _bweConfig.sanitize();
    _transportFactory = transport::createTransportFactory(*_jobManager,
        *_srtpClientFactory,
        _config,
        _sctpConfig,
        _iceConfig,
        _bweConfig,
        _rateControllerConfig,
        _localInterfaces,
        *_network,
        *_mainPacketAllocator);
    if (!_transportFactory->isGood())
    {
        logger::error("Failed to initialize transport factory", "main");
        return;
    }

    _mixerManager = std::make_unique<bridge::MixerManager>(*_idGenerator,
        *_ssrcGenerator,
        *_jobManager,
        *_transportFactory,
        *_engine,
        _config,
        *_mainPacketAllocator,
        *_sendPacketAllocator,
        *_audioPacketAllocator);

    _requestHandler = std::make_unique<bridge::ApiRequestHandler>(*_mixerManager, *_sslDtls);

    _httpd = std::make_unique<httpd::Httpd>(*_requestHandler);
    if (!_httpd->start(_config.port.get()))
    {
        return;
    }

    _initialized = true;
}

void Bridge::startWorkerThreads()
{
    auto numWorkerThreads = _config.numWorkerTreads.get();
    if (numWorkerThreads == 0)
    {
        const auto hardwareConcurrency = std::thread::hardware_concurrency();
        if (hardwareConcurrency == 0)
        {
            numWorkerThreads = 7;
            logger::warn("Unable to set numWorkerTreads, defaulting to %u", "main", numWorkerThreads);
        }
        else
        {
            numWorkerThreads = std::max(hardwareConcurrency - 1, 1U);
        }
    }
    logger::info("Starting %u worker threads", "main", numWorkerThreads);

    for (int i = 0; i < numWorkerThreads; ++i)
    {
        _workerThreads.push_back(std::make_unique<jobmanager::WorkerThread>(*_jobManager));
    }
}
} // namespace bridge
