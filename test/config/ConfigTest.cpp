#include "config/ConfigReader.h"
#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>

namespace
{
class TestConfig : public config::ConfigReader
{
public:
    CFG_PROP(std::string, str1, "str1_def");
    CFG_MANDATORY_PROP(std::string, str2);
    CFG_PROP(int, int1, 11);
    CFG_MANDATORY_PROP(int, int2);
    CFG_PROP(uint64_t, uint64_1, 21);
    CFG_MANDATORY_PROP(uint64_t, uint64_2);
    CFG_GROUP()
    CFG_PROP(int, g_int, 1);
    CFG_PROP(std::string, g_str, "g1_str_def");
    CFG_GROUP_END(g1);
    CFG_GROUP()
    CFG_PROP(int, g_int, 2);
    CFG_PROP(std::string, g_str, "g2_str_def");
    CFG_GROUP()
    CFG_PROP(bool, g_bool, false);
    CFG_GROUP_END(g3);
    CFG_GROUP_END(g2);
};

void writeSampleConfig(std::ostream& s)
{
    s << "{" << std::endl;
    s << "  \"str2\": \"foo\"," << std::endl;
    s << "  \"int2\": 123," << std::endl;
    s << "  \"uint64_2\": 678," << std::endl;
    s << "  \"g1.g_str\": \"g1_str1\"," << std::endl;
    s << "  \"g2.g_int\": 987," << std::endl;
    s << "  \"g2.g3.g_bool\": true" << std::endl;
    s << "}" << std::endl;
}

void verifySampleConfig(const TestConfig& cfg)
{
    ASSERT_STREQ(cfg.str1.get().c_str(), "str1_def");
    ASSERT_STREQ(cfg.str2.get().c_str(), "foo");
    ASSERT_EQ(cfg.int1, 11);
    ASSERT_EQ(cfg.int2, 123);
    ASSERT_EQ(cfg.uint64_1, uint64_t(21));
    ASSERT_EQ(cfg.uint64_2, uint64_t(678));
    ASSERT_EQ(cfg.g1.g_int, 1);
    ASSERT_STREQ(cfg.g1.g_str.get().c_str(), "g1_str1");
    ASSERT_EQ(cfg.g2.g_int, 987);
    ASSERT_STREQ(cfg.g2.g_str.get().c_str(), "g2_str_def");
    ASSERT_EQ(cfg.g2.g3.g_bool, true);
}
} // namespace

struct ConfigTest : public ::testing::Test
{
};

TEST_F(ConfigTest, canReadFromFile)
{
    char tmpConfigFname[11] = "cfg-XXXXXX";
    const auto tmpHandle = mkstemp(tmpConfigFname);
    ASSERT_GT(tmpHandle, 0);

    std::ofstream tmpFs(tmpConfigFname);
    writeSampleConfig(tmpFs);
    tmpFs.close();

    TestConfig cfg;
    ASSERT_TRUE(cfg.readFromFile(tmpConfigFname));
    verifySampleConfig(cfg);

    close(tmpHandle);
    remove(tmpConfigFname);
}

TEST_F(ConfigTest, canReadFromString)
{
    std::ostringstream ss;
    writeSampleConfig(ss);

    TestConfig cfg;
    ASSERT_TRUE(cfg.readFromString(ss.str()));
    verifySampleConfig(cfg);
}

TEST_F(ConfigTest, failsToReadBrokenJsonAndKeepsValuesDefault)
{
    TestConfig cfg;
    ASSERT_FALSE(cfg.readFromString("{ foo }"));
    ASSERT_STREQ(cfg.str1.get().c_str(), "str1_def");
    ASSERT_STREQ(cfg.str2.get().c_str(), "");
}

TEST_F(ConfigTest, failsToReadIfMandatoryKeyIsMissingAndSetsMissingValueToTypeDefault)
{
    TestConfig cfg;
    ASSERT_FALSE(cfg.readFromString("{ \"str1\": \"a\" }"));
    ASSERT_STREQ(cfg.str1.get().c_str(), "a");
    ASSERT_STREQ(cfg.str2.get().c_str(), "");
}
