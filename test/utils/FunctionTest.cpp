#include "utils/Function.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace testing;

namespace
{

int freeFunction0CalledCount = 0;
int freeFunction1CalledCount = 0;
int freeFunction1LastValue = 0;

struct FakeValue
{
    uint32_t value1;
    uint32_t value2;
};

bool operator==(const FakeValue& lhs, const FakeValue& rhs)
{
    return lhs.value1 == rhs.value1 && lhs.value2 == rhs.value2;
}

void freeFunction0()
{
    ++freeFunction0CalledCount;
}

void freeFunction1(int value)
{
    freeFunction1LastValue = value;
    ++freeFunction1CalledCount;
}

class CallbackClassMock
{
public:
    MOCK_METHOD(void, callback0, ());
    MOCK_METHOD(void, callback1, (int a));
    MOCK_METHOD(void, callback2, (int a, int b));
    MOCK_METHOD(void, callback3, (int a, int b, bool c));
    MOCK_METHOD(void, callback4, (int a, int b, bool c, FakeValue d));
    MOCK_METHOD(void, callback5, (int a, int b, bool c, FakeValue d, const FakeValue& e));
    MOCK_METHOD(void, callback6, (int a, int b, bool c, FakeValue d, const FakeValue& e));
    MOCK_METHOD(void, callback7, (int a, int b, bool c, FakeValue d, FakeValue& e));
};

} // namespace

TEST(FunctionTest, bindFreeFunction)
{

    freeFunction0CalledCount = 0;
    freeFunction1CalledCount = 0;
    freeFunction1LastValue = 0;

    auto freeFunctionBind0 = utils::bind(&freeFunction0);
    auto freeFunctionBind1 = utils::bind(&freeFunction1, 122);
    auto freeFunctionBind2 = utils::bind(&freeFunction1, 1223);

    freeFunctionBind0();
    ASSERT_EQ(1, freeFunction0CalledCount);
    ASSERT_EQ(0, freeFunction1CalledCount);
    ASSERT_EQ(0, freeFunction1LastValue);

    freeFunctionBind1();
    ASSERT_EQ(1, freeFunction0CalledCount);
    ASSERT_EQ(1, freeFunction1CalledCount);
    ASSERT_EQ(122, freeFunction1LastValue);

    // Multiple call Test
    freeFunctionBind1();
    ASSERT_EQ(1, freeFunction0CalledCount);
    ASSERT_EQ(2, freeFunction1CalledCount);
    ASSERT_EQ(122, freeFunction1LastValue);

    freeFunctionBind2();
    ASSERT_EQ(1, freeFunction0CalledCount);
    ASSERT_EQ(3, freeFunction1CalledCount);
    ASSERT_EQ(1223, freeFunction1LastValue);

    freeFunctionBind0();
    ASSERT_EQ(2, freeFunction0CalledCount);
    ASSERT_EQ(3, freeFunction1CalledCount);
    ASSERT_EQ(1223, freeFunction1LastValue);
}

TEST(FunctionTest, bindMemberFunction)
{
    FakeValue fakeValue0 = {1, 2};
    FakeValue fakeValue1 = {111, 222};

    FakeValue fakeOriginalValue0 = fakeValue0;
    FakeValue fakeOriginalValue1 = fakeValue1;

    StrictMock<CallbackClassMock> callbacks0;
    StrictMock<CallbackClassMock> callbacks1;
    StrictMock<CallbackClassMock> callbacks2;
    StrictMock<CallbackClassMock> callbacks3;
    StrictMock<CallbackClassMock> callbacks4;
    StrictMock<CallbackClassMock> callbacks5;
    StrictMock<CallbackClassMock> callbacks6;
    StrictMock<CallbackClassMock> callbacks7;

    auto callbackBind0 = utils::bind(&CallbackClassMock::callback0, &callbacks0);
    auto callbackBind1 = utils::bind(&CallbackClassMock::callback1, &callbacks1, 5);
    auto callbackBind2 = utils::bind(&CallbackClassMock::callback2, &callbacks2, 10, 20);
    auto callbackBind3 = utils::bind(&CallbackClassMock::callback3, &callbacks3, 30, 40, true);
    auto callbackBind4 = utils::bind(&CallbackClassMock::callback4, &callbacks4, 30, 40, false, fakeValue0);
    auto callbackBind5 = utils::bind(&CallbackClassMock::callback5, &callbacks5, 30, 40, true, fakeValue0, fakeValue1);

    auto callbackBind6 =
        utils::bind(&CallbackClassMock::callback6, &callbacks6, 30, 40, true, fakeValue0, std::cref(fakeValue1));

    auto callbackBind7 =
        utils::bind(&CallbackClassMock::callback7, &callbacks7, 30, 40, true, fakeValue0, std::ref(fakeValue1));

    // Change values to ensure we are not keep a reference
    fakeValue0.value1 = 9;
    fakeValue0.value2 = 8;

    fakeValue1.value1 = 99;
    fakeValue1.value2 = 888;

    {
        InSequence s;

        EXPECT_CALL(callbacks0, callback0());
        EXPECT_CALL(callbacks1, callback1(5));
        EXPECT_CALL(callbacks2, callback2(10, 20));
        EXPECT_CALL(callbacks3, callback3(30, 40, true));
        EXPECT_CALL(callbacks4, callback4(30, 40, false, fakeOriginalValue0));
        EXPECT_CALL(callbacks5, callback5(30, 40, true, fakeOriginalValue0, fakeOriginalValue1));
        EXPECT_CALL(callbacks6, callback6(30, 40, true, fakeOriginalValue0, fakeValue1)); // It's a std::cref
        EXPECT_CALL(callbacks7, callback7(30, 40, true, fakeOriginalValue0, fakeValue1)); // It's a std::ref
    }

    callbackBind0();
    callbackBind1();
    callbackBind2();
    callbackBind3();
    callbackBind4();
    callbackBind5();
    callbackBind6();
    callbackBind7();
}

TEST(FunctionTest, implicitConversionToFunction)
{

    freeFunction0CalledCount = 0;
    freeFunction1CalledCount = 0;
    freeFunction1LastValue = 0;

    FakeValue fakeValue0 = {1, 2};
    FakeValue fakeValue1 = {111, 222};

    FakeValue fakeOriginalValue0 = fakeValue0;

    std::vector<utils::Function> mFunctions;

    mFunctions.push_back(utils::bind(&freeFunction0));
    mFunctions.emplace_back(utils::bind(&freeFunction1, 1223));

    StrictMock<CallbackClassMock> callbacks6;
    StrictMock<CallbackClassMock> callbacks7;

    mFunctions.push_back(
        utils::bind(&CallbackClassMock::callback6, &callbacks6, 30, 40, true, fakeValue0, std::cref(fakeValue1)));

    mFunctions.push_back(
        utils::bind(&CallbackClassMock::callback7, &callbacks7, 30, 40, true, fakeValue0, std::ref(fakeValue1)));

    // Copy
    mFunctions.push_back(mFunctions.back());

    // Change values to ensure we are not keep a reference
    fakeValue0.value1 = 9;
    fakeValue0.value2 = 8;

    fakeValue1.value1 = 99;
    fakeValue1.value2 = 888;

    {
        InSequence s;

        EXPECT_CALL(callbacks6, callback6(30, 40, true, fakeOriginalValue0, fakeValue1)); // It's a std::cref
        EXPECT_CALL(callbacks7, callback7(30, 40, true, fakeOriginalValue0, fakeValue1)).Times(2);
    }

    for (auto& func : mFunctions)
    {
        func();
    }

    ASSERT_EQ(1, freeFunction0CalledCount);
    ASSERT_EQ(1, freeFunction1CalledCount);
    ASSERT_EQ(1223, freeFunction1LastValue);
}

TEST(FunctionTest, copy)
{
    FakeValue fakeValue0 = {1, 2};
    FakeValue fakeOriginalValue0 = fakeValue0;

    StrictMock<CallbackClassMock> callbacks7;

    utils::Function originalFunction =
        utils::bind(&CallbackClassMock::callback7, &callbacks7, 30, 40, true, fakeValue0, std::ref(fakeValue0));

    utils::Function copyFunction2;
    utils::Function copyFunction1(originalFunction);
    copyFunction2 = originalFunction;

    fakeValue0.value1 = 887766;
    fakeValue0.value2 = 6346;

    EXPECT_CALL(callbacks7, callback7(30, 40, true, fakeOriginalValue0, fakeValue0)).Times(3);

    originalFunction();
    copyFunction1();
    copyFunction2();
}

TEST(FunctionTest, move)
{
    FakeValue fakeValue0 = {1, 2};
    FakeValue fakeOriginalValue0 = fakeValue0;

    StrictMock<CallbackClassMock> callbacks0;
    StrictMock<CallbackClassMock> callbacks7;

    utils::Function originalFunction0 = utils::bind(&CallbackClassMock::callback0, &callbacks0);
    utils::Function originalFunction7 =
        utils::bind(&CallbackClassMock::callback7, &callbacks7, 30, 40, true, fakeValue0, std::ref(fakeValue0));

    utils::Function newFunction7;
    utils::Function newFunction0(std::move(originalFunction0));
    newFunction7 = std::move(originalFunction7);

    fakeValue0.value1 = 887766;
    fakeValue0.value2 = 634;

    EXPECT_CALL(callbacks0, callback0());
    EXPECT_CALL(callbacks7, callback7(30, 40, true, fakeOriginalValue0, fakeValue0));

    ASSERT_EQ(false, originalFunction0);
    ASSERT_EQ(false, originalFunction7);
    ASSERT_EQ(nullptr, originalFunction0);
    ASSERT_EQ(nullptr, originalFunction7);
    ASSERT_EQ(true, newFunction0);
    ASSERT_EQ(true, newFunction7);
    ASSERT_NE(nullptr, newFunction0);
    ASSERT_NE(nullptr, newFunction7);

    newFunction0();
    newFunction7();
}