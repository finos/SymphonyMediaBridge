#pragma once

#include "TransportStub.h"
#include "jobmanager/JobManager.h"
#include "transport/TransportFactory.h"
#include <queue>

class TransportFactoryStub : public transport::TransportFactory
{
private:
    mutable std::queue<TransportStub*> _transports;
    jobmanager::JobManager& _jobManager;

public:
    TransportStub* deqeue()
    {
        EXPECT_FALSE(_transports.empty());
        auto result = _transports.front();
        _transports.pop();
        return result;
    }

public: // transport::TransportFactory
    TransportFactoryStub(jobmanager::JobManager& jobManager) : _jobManager(jobManager) {}

    std::shared_ptr<transport::Transport> create(size_t sendPoolSize) override
    {
        auto result = std::make_shared<TransportStub>(_jobManager);
        _transports.push(result.get());
        return result;
    }

    std::shared_ptr<transport::Transport> create(const ice::IceRole iceRole, size_t sendPoolSize) override
    {
        auto result = std::make_shared<TransportStub>(_jobManager);
        _transports.push(result.get());
        return result;
    }

    virtual std::shared_ptr<transport::Transport> createOnSharedPort(const ice::IceRole iceRole,
        size_t sendPoolSize) override
    {
        auto result = std::make_shared<TransportStub>(_jobManager, endpointId);
        _transports.push(result.get());
        return result;
    }

    virtual std::shared_ptr<transport::Transport> createOnPrivatePort(const ice::IceRole iceRole,
        size_t sendPoolSize) override
    {
        auto result = std::make_shared<TransportStub>(_jobManager, endpointId);
        _transports.push(result.get());
        return result;
    }

    bool isGood() const override { return true; }
};
