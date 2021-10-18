#include "utils/StringTokenizer.h"
#include <cassert>
#include <gtest/gtest.h>

class StringTokenizerTest : public ::testing::Test
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

TEST_F(StringTokenizerTest, tokenize)
{
    std::string data = "abc/defg/eh";

    utils::StringTokenizer::Token token0 = utils::StringTokenizer::tokenize(data.c_str(), data.length(), '/');
    EXPECT_EQ(3, token0.length);
    EXPECT_EQ(data.c_str(), token0.start);
    EXPECT_EQ(&((data.c_str())[4]), token0.next);
    EXPECT_EQ(7, token0.remainingLength);
    EXPECT_EQ(0, strncmp(token0.start, "abc", token0.length));

    utils::StringTokenizer::Token token1 = utils::StringTokenizer::tokenize(token0.next, token0.remainingLength, '/');
    EXPECT_EQ(4, token1.length);
    EXPECT_EQ(token0.next, token1.start);
    EXPECT_EQ(&((token0.next)[5]), token1.next);
    EXPECT_EQ(2, token1.remainingLength);
    EXPECT_EQ(0, strncmp(token1.start, "defg", token1.length));

    utils::StringTokenizer::Token token2 = utils::StringTokenizer::tokenize(token1.next, token1.remainingLength, '/');
    EXPECT_EQ(2, token2.length);
    EXPECT_EQ(token1.next, token2.start);
    EXPECT_EQ(nullptr, token2.next);
    EXPECT_EQ(0, token2.remainingLength);
    EXPECT_EQ(0, strncmp(token2.start, "eh", token2.length));
}

TEST_F(StringTokenizerTest, trailingDelimiter)
{
    std::string data = "a/b/";

    utils::StringTokenizer::Token token0 = utils::StringTokenizer::tokenize(data.c_str(), data.length(), '/');
    EXPECT_EQ(1, token0.length);
    EXPECT_EQ(data.c_str(), token0.start);
    EXPECT_EQ(&((data.c_str())[2]), token0.next);
    EXPECT_EQ(2, token0.remainingLength);
    EXPECT_EQ(0, strncmp(token0.start, "a", token0.length));

    utils::StringTokenizer::Token token1 = utils::StringTokenizer::tokenize(token0.next, token0.remainingLength, '/');
    EXPECT_EQ(1, token1.length);
    EXPECT_EQ(token0.next, token1.start);
    EXPECT_EQ(nullptr, token1.next);
    EXPECT_EQ(0, token1.remainingLength);
    EXPECT_EQ(0, strncmp(token1.start, "b", token1.length));
}

TEST_F(StringTokenizerTest, trailingDelimiters)
{
    std::string data = "a/b///////////";

    utils::StringTokenizer::Token token0 = utils::StringTokenizer::tokenize(data.c_str(), data.length(), '/');
    EXPECT_EQ(1, token0.length);
    EXPECT_EQ(data.c_str(), token0.start);
    EXPECT_EQ(&((data.c_str())[2]), token0.next);
    EXPECT_EQ(12, token0.remainingLength);
    EXPECT_EQ(0, strncmp(token0.start, "a", token0.length));

    utils::StringTokenizer::Token token1 = utils::StringTokenizer::tokenize(token0.next, token0.remainingLength, '/');
    EXPECT_EQ(1, token1.length);
    EXPECT_EQ(token0.next, token1.start);
    EXPECT_EQ(&((data.c_str())[4]), token1.next);
    EXPECT_EQ(10, token1.remainingLength);
    EXPECT_EQ(0, strncmp(token1.start, "b", token1.length));

    utils::StringTokenizer::Token token2 = utils::StringTokenizer::tokenize(token1.next, token1.remainingLength, '/');
    EXPECT_EQ(0, token2.length);
    EXPECT_EQ(nullptr, token2.start);
    EXPECT_EQ(nullptr, token2.next);
    EXPECT_EQ(0, token2.remainingLength);
}

TEST_F(StringTokenizerTest, startDelimiter)
{
    std::string data = "/a/b";

    utils::StringTokenizer::Token token0 = utils::StringTokenizer::tokenize(data.c_str(), data.length(), '/');
    EXPECT_EQ(1, token0.length);
    EXPECT_EQ(&((data.c_str())[1]), token0.start);
    EXPECT_EQ(&((data.c_str())[3]), token0.next);
    EXPECT_EQ(1, token0.remainingLength);
    EXPECT_EQ(0, strncmp(token0.start, "a", token0.length));

    utils::StringTokenizer::Token token1 = utils::StringTokenizer::tokenize(token0.next, token0.remainingLength, '/');
    EXPECT_EQ(1, token1.length);
    EXPECT_EQ(token0.next, token1.start);
    EXPECT_EQ(nullptr, token1.next);
    EXPECT_EQ(0, token1.remainingLength);
    EXPECT_EQ(0, strncmp(token1.start, "b", token1.length));
}

TEST_F(StringTokenizerTest, startDelimiters)
{
    std::string data = "//////////a/b";

    utils::StringTokenizer::Token token0 = utils::StringTokenizer::tokenize(data.c_str(), data.length(), '/');
    EXPECT_EQ(1, token0.length);
    EXPECT_EQ(&((data.c_str())[10]), token0.start);
    EXPECT_EQ(&((data.c_str())[12]), token0.next);
    EXPECT_EQ(1, token0.remainingLength);
    EXPECT_EQ(0, strncmp(token0.start, "a", token0.length));

    utils::StringTokenizer::Token token1 = utils::StringTokenizer::tokenize(token0.next, token0.remainingLength, '/');
    EXPECT_EQ(1, token1.length);
    EXPECT_EQ(token0.next, token1.start);
    EXPECT_EQ(nullptr, token1.next);
    EXPECT_EQ(0, token1.remainingLength);
    EXPECT_EQ(0, strncmp(token1.start, "b", token1.length));
}

TEST_F(StringTokenizerTest, onlyDelimiter)
{
    std::string data = "/";

    utils::StringTokenizer::Token token0 = utils::StringTokenizer::tokenize(data.c_str(), data.length(), '/');
    EXPECT_EQ(0, token0.length);
    EXPECT_EQ(nullptr, token0.start);
    EXPECT_EQ(nullptr, token0.next);
    EXPECT_EQ(0, token0.remainingLength);
}

TEST_F(StringTokenizerTest, empty)
{
    std::string data;

    utils::StringTokenizer::Token token0 = utils::StringTokenizer::tokenize(data.c_str(), data.length(), '/');
    EXPECT_EQ(0, token0.length);
    EXPECT_EQ(nullptr, token0.start);
    EXPECT_EQ(nullptr, token0.next);
    EXPECT_EQ(0, token0.remainingLength);
}

TEST_F(StringTokenizerTest, noDelimiter)
{
    std::string data = "abc";

    utils::StringTokenizer::Token token0 = utils::StringTokenizer::tokenize(data.c_str(), data.length(), '/');
    EXPECT_EQ(3, token0.length);
    EXPECT_EQ(data.c_str(), token0.start);
    EXPECT_EQ(nullptr, token0.next);
    EXPECT_EQ(0, token0.remainingLength);
    EXPECT_EQ(0, strncmp(token0.start, "abc", token0.length));
}
