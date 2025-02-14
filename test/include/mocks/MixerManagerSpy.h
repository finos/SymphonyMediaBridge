#pragma once

#include "bridge/MixerManager.h"
#include "bridge/engine/Engine.h"
#include "config/Config.h"
#include "mocks/TransportFactoryMock.h"
#include "utils/IdGenerator.h"
#include "utils/SsrcGenerator.h"

namespace test
{
class MixerManagerSpy : public bridge::MixerManager
{
public:
    struct MixerManagerSpyJobManagerResources
    {
        MixerManagerSpyJobManagerResources()
            : timerQueue(std::make_unique<jobmanager::TimerQueue>(256)),
              jobManager(std::make_unique<jobmanager::JobManager>(*timerQueue))
        {
        }

        std::unique_ptr<jobmanager::TimerQueue> timerQueue;
        std::unique_ptr<jobmanager::JobManager> jobManager;
    };

    struct MixerManagerSpyResources
    {
        MixerManagerSpyResources(std::unique_ptr<MixerManagerSpyJobManagerResources> jobManagerResources,
            std::shared_ptr<TransportFactoryMock> transportFactoryMock,
            std::shared_ptr<bridge::Engine> engine)
            : idGenerator(),
              ssrcGenerator(),
              timerQueue(std::move(jobManagerResources->timerQueue)),
              jobManager(std::move(jobManagerResources->jobManager)),
              config(),
              mainAllocator(1024, "MainAllocator - MixerManagerSpyResources"),
              sendAllocator(1024, "SenderAllocator - MixerManagerSpyResources"),
              audioAllocator(1024, "AudioAllocator - MixerManagerSpyResources"),
              transportFactoryMock(std::move(transportFactoryMock)),
              engine(std::move(engine))
        {
        }

        utils::IdGenerator idGenerator;
        utils::SsrcGenerator ssrcGenerator;
        std::unique_ptr<jobmanager::TimerQueue> timerQueue;
        std::unique_ptr<jobmanager::JobManager> jobManager;
        config::Config config;
        memory::PacketPoolAllocator mainAllocator;
        memory::PacketPoolAllocator sendAllocator;
        memory::AudioPacketPoolAllocator audioAllocator;

        std::shared_ptr<TransportFactoryMock> transportFactoryMock;
        std::shared_ptr<bridge::Engine> engine;

        template <template <typename> typename TTransportTypeFactoryMockType = ::testing::NiceMock>
        static std::unique_ptr<MixerManagerSpyResources> makeDefault()
        {
            auto jobManagerResources = std::make_unique<MixerManagerSpyJobManagerResources>();
            auto transportFactoryMock = std::make_shared<TTransportTypeFactoryMockType<TransportFactoryMock>>();
            auto engine = std::make_shared<bridge::Engine>(*jobManagerResources->jobManager, std::thread());

            return std::make_unique<MixerManagerSpyResources>(std::move(jobManagerResources),
                transportFactoryMock,
                engine);
        }
    };

    // using parent constructor
    using bridge::MixerManager::MixerManager;

public:
    MixerManagerSpy(MixerManagerSpyResources& resources)
        : MixerManagerSpy(resources.idGenerator,
              resources.ssrcGenerator,
              *resources.jobManager,
              *resources.jobManager,
              *resources.transportFactoryMock,
              *resources.engine,
              resources.config,
              resources.mainAllocator,
              resources.sendAllocator,
              resources.audioAllocator)
    {
    }
};
} // namespace test
