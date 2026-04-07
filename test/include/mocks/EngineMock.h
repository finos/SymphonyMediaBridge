#pragma once

#include "bridge/engine/IEngine.h"
#include "concurrency/MpmcQueue.h"
#include "gmock/gmock.h"

namespace bridge
{

class EngineMock : public IEngine
{
public:
    EngineMock() : _tasks(512),
                   _synchronizationContext(_tasks)
    {
    }

    MOCK_METHOD(void, setMessageListener, (MixerManagerAsync * messageListener), (override));
    MOCK_METHOD(void, stop, (), (override));
    MOCK_METHOD(void, run, (), (override));
    MOCK_METHOD(bool, post, (utils::Function && task), (override));
    MOCK_METHOD(EngineStats::EngineStats, getStats, (), (override));
    MOCK_METHOD(bool, asyncAddMixer, (EngineMixer * engineMixer), (override));
    MOCK_METHOD(bool, asyncRemoveMixer, (EngineMixer * engineMixer), (override));

    concurrency::SynchronizationContext getSynchronizationContext() override { return _synchronizationContext; }

    void processTasks()
    {
        utils::Function task;
        while (_tasks.pop(task))
        {
            task();
        }
    }

private:
    concurrency::MpmcQueue<utils::Function> _tasks;
    concurrency::SynchronizationContext _synchronizationContext;
};

} // namespace bridge