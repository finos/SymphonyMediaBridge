#pragma once

#include "bridge/engine/EngineMixer.h"
#include "config/Config.h"
#include "mocks/MixerManagerAsyncMock.h"
#include <gmock/gmock.h>
#include <memory>

namespace test
{

struct EngineMixerSpy : public bridge::EngineMixer
{
    struct EngineMixerResources
    {
        EngineMixerResources()
            : id("EngineMixerSpy"),
              timerQueue(4096 * 8),
              engineTaskQueue(256),
              jobManager(timerQueue),
              backgroundJobManager(timerQueue),
              mixerManagerAsyncMock(),
              config(),
              senderAllocator(1024, "SenderAllocator-EngineMixerResources"),
              audioAllocator(1024, "AudioAllocator - EngineMixerResources"),
              mainAllocator(1024, "MainAllocator - EngineMixerResources")
        {
        }

        std::string id;
        jobmanager::TimerQueue timerQueue;
        concurrency::MpmcQueue<utils::Function> engineTaskQueue;
        jobmanager::JobManager jobManager;
        jobmanager::JobManager backgroundJobManager;
        MixerManagerAsyncMock mixerManagerAsyncMock;
        config::Config config;
        memory::PacketPoolAllocator senderAllocator;
        memory::AudioPacketPoolAllocator audioAllocator;
        memory::PacketPoolAllocator mainAllocator;
    };

public:
    // Make IncomingPacketInfo visible
    using IncomingPacketInfo = bridge::EngineMixer::IncomingPacketInfo;

    // using parent constructor
    using bridge::EngineMixer::EngineMixer;

public:
    EngineMixerSpy(EngineMixerResources& resources)
        : EngineMixerSpy(resources.id,
              resources.jobManager,
              concurrency::SynchronizationContext(resources.engineTaskQueue),
              resources.backgroundJobManager,
              resources.mixerManagerAsyncMock,
              27482742,
              resources.config,
              resources.senderAllocator,
              resources.audioAllocator,
              resources.mainAllocator,
              {4373732u},
              {api::makeSsrcGroup({{3746438, 363463}, {482473, 754432}, {93232326, 55443221}}),
                  api::makeSsrcGroup({{373434, 355625}, {6655433, 723623}, {77346343, 327362}}),
                  api::makeSsrcGroup({{9586, 1118746}, {8239, 998575}, {439846, 88564}}),
                  api::makeSsrcGroup({{4948574, 999876}, {53909000, 934774}, {1003243, 90007}}),
                  api::makeSsrcGroup({{478352, 3276565}, {77325323, 3734}, {45454927, 7008532}}),
                  api::makeSsrcGroup({{574821, 90200304}, {523263, 2346}, {292924, 545121}}),
                  api::makeSsrcGroup({{104957, 6254}, {36728372, 783423}, {846452, 522125}}),
                  api::makeSsrcGroup({{3394343, 857484}, {23623, 73443}, {9011523, 537785}}),
                  api::makeSsrcGroup({{74347399, 23294}, {8232, 27623}, {22544337, 77682}}),
                  api::makeSsrcGroup({{4966522, 88641}, {12129487, 63283}, {74634, 76145}}),
                  api::makeSsrcGroup({{2224746, 2121764}, {6523253, 213434}, {74347334, 63223}}),
                  api::makeSsrcGroup({{734634, 121378}, {80034, 700456}, {500763, 100323}})},
              9)
    {
    }

public:
    concurrency::MpmcQueue<IncomingPacketInfo>& spyIncomingBarbellSctp() { return _incomingBarbellSctp; };
    concurrency::MpmcQueue<IncomingPacketInfo>& spyIncomingForwarderAudioRtp() { return _incomingForwarderAudioRtp; };
    concurrency::MpmcQueue<IncomingPacketInfo>& spyIncomingRtcp() { return _incomingRtcp; };
    concurrency::MpmcQueue<IncomingPacketInfo>& spyIncomingForwarderVideoRtp() { return _incomingForwarderVideoRtp; };

    concurrency::MpmcHashmap32<size_t, bridge::EngineAudioStream*>& spyEngineAudioStreams()
    {
        return _engineAudioStreams;
    }
    concurrency::MpmcHashmap32<size_t, bridge::EngineVideoStream*>& spyEngineVideoStreams()
    {
        return _engineVideoStreams;
    }
    concurrency::MpmcHashmap32<size_t, bridge::EngineDataStream*>& spyEngineDataStreams() { return _engineDataStreams; }
    concurrency::MpmcHashmap32<size_t, bridge::EngineRecordingStream*>& spyEngineRecordingStreams()
    {
        return _engineRecordingStreams;
    }

    concurrency::MpmcHashmap32<size_t, bridge::EngineBarbell*>& spyEngineBarbells() { return _engineBarbells; }

    concurrency::MpmcHashmap32<uint32_t, bridge::SsrcInboundContext*>& spySsrcInboundContexts()
    {
        return _ssrcInboundContexts;
    }

    concurrency::MpmcHashmap32<uint32_t, bridge::SsrcInboundContext>& spyAllSsrcInboundContexts()
    {
        return _allSsrcInboundContexts;
    }

    static EngineMixerSpy* spy(EngineMixer* EngineMixer)
    {
        // ATTENTION:
        // We are going to reinterpret the pointer to the Spy pointer. This is only safe as long there is no virtual
        // methods (as the vtable will be wrong) and there is EngineMixerSpy doesn't add more fields that would cause
        // reading memory out of EngineMixer* boundaries
        return reinterpret_cast<EngineMixerSpy*>(EngineMixer);
    }
};

} // namespace test
