#pragma once

#include "utils/Time.h"
#include <gmock/gmock.h>

namespace test
{

struct TimeSourceMock : public utils::TimeSource
{
    MOCK_METHOD(uint64_t, getAbsoluteTime, (), (override));
    MOCK_METHOD(void, nanoSleep, (uint64_t nanoSeconds), (override));
    MOCK_METHOD(std::chrono::system_clock::time_point, wallClock, (), (const, override));
};

} // namespace test