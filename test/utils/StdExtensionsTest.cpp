#include "utils/StdExtensions.h"
#include <gtest/gtest.h>
#include <string>

TEST(StdExtensionTest, HashEquivalence)
{
    std::string stdString = "String To Hash";
    char* ptr = const_cast<char*>(stdString.c_str());
    const char* constPtr = stdString.c_str();

    auto hashValue1 = utils::hash<std::string>{}(stdString);
    auto hashValue2 = utils::hash<char*>{}(ptr);
    auto hashValue3 = utils::hash<const char*>{}(constPtr);

    ASSERT_EQ(hashValue1, hashValue2);
    ASSERT_EQ(hashValue2, hashValue3);
}