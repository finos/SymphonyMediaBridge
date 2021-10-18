#include "utils/StringBuilder.h"
#include <gtest/gtest.h>

class StringBuilderTest : public ::testing::Test
{
    void SetUp() override
    {
        // Code here will be called immediately after the constructor (right
        // before each test).
    }

    void TearDown() override
    {
        // Code here will be called immediately after each test (right
        // before the destructor).
    }
};

TEST_F(StringBuilderTest, appendString)
{
    utils::StringBuilder<11> stringBuilder;

    stringBuilder.append(std::string("01234"));
    stringBuilder.append(std::string("56789"));

    const auto result = stringBuilder.build();
    ASSERT_EQ("0123456789", result);
}

TEST_F(StringBuilderTest, appendCString)
{
    utils::StringBuilder<11> stringBuilder;

    stringBuilder.append("01234");
    stringBuilder.append("56789");

    const auto result = stringBuilder.build();
    ASSERT_EQ("0123456789", result);
}

TEST_F(StringBuilderTest, appendWithLength)
{
    utils::StringBuilder<256> stringBuilder;

    stringBuilder.append("01234", 5);
    stringBuilder.append("56789", 5);
    stringBuilder.append("ABC", 3);
    stringBuilder.append("DEFGH", 1);
    stringBuilder.append("I", 5);

    const auto result = stringBuilder.build();
    ASSERT_EQ("0123456789ABCDI", result);
}

TEST_F(StringBuilderTest, appendUint32)
{
    utils::StringBuilder<6> stringBuilder;

    stringBuilder.append(123);
    stringBuilder.append(45);

    const auto result = stringBuilder.build();
    ASSERT_EQ("12345", result);
}

TEST_F(StringBuilderTest, appendNoSpaceLeft)
{
    utils::StringBuilder<4> stringBuilder;

    stringBuilder.append("abc");
    stringBuilder.append("d");

    const auto result = stringBuilder.build();
    ASSERT_EQ("abc", result);
}

TEST_F(StringBuilderTest, clear)
{
    utils::StringBuilder<11> stringBuilder;

    stringBuilder.append(std::string("01234"));

    const auto result = stringBuilder.build();
    ASSERT_EQ("01234", result);

    stringBuilder.clear();
    const auto emptyResult = stringBuilder.build();
    ASSERT_TRUE(emptyResult.empty());
}
