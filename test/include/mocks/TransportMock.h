#pragma once

#include "logger/Logger.h"
#include <gmock/gmock.h>

namespace transport
{
class DataReceiver;
}

namespace jobmanager
{
class JobQueue;
}

namespace test
{

template <class BaseTransport>
class TransportMock : public BaseTransport
{
public:
    TransportMock() : _loggableId("TransportMock")
    {
        ON_CALL(*this, isInitialized()).WillByDefault(testing::Return(true));
        ON_CALL(*this, isRunning()).WillByDefault(testing::Return(true));
        ON_CALL(*this, hasPendingJobs()).WillByDefault(testing::Return(true));
        ON_CALL(*this, getJobCounter()).WillByDefault(testing::ReturnRef(_jobCounter));
        ON_CALL(*this, unprotect(testing::_)).WillByDefault(testing::Return(true));
        ON_CALL(*this, unprotectFirstRtp(testing::_, testing::_)).WillByDefault(testing::Return(true));
    }

    MOCK_METHOD(bool, isInitialized, (), (const override));
    MOCK_METHOD(size_t, getId, (), (const override));
    MOCK_METHOD(size_t, getEndpointIdHash, (), (const override));
    MOCK_METHOD(void, stop, (), (override));
    MOCK_METHOD(bool, isRunning, (), (const override));
    MOCK_METHOD(bool, hasPendingJobs, (), (const override));
    MOCK_METHOD(std::atomic_uint32_t&, getJobCounter, (), (override));
    MOCK_METHOD(bool, unprotect, (memory::Packet & packet), (override));
    MOCK_METHOD(bool, unprotectFirstRtp, (memory::Packet & packet, uint32_t& roc), (override));
    MOCK_METHOD(void, setDataReceiver, (transport::DataReceiver * dataReceiver), (override));
    MOCK_METHOD(bool, isConnected, (), (override));
    MOCK_METHOD(bool, start, (), (override));
    MOCK_METHOD(void, connect, (), (override));
    MOCK_METHOD(jobmanager::JobQueue&, getJobQueue, (), (override));
    MOCK_METHOD(void, protectAndSend, (memory::UniquePacket packet), (override));

    const logger::LoggableId& getLoggableId() const override { return _loggableId; }

private:
    logger::LoggableId _loggableId;
    std::atomic_uint32_t _jobCounter;
};

} // namespace test