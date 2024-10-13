#include "transport/ice/Stun.h"
#include "utils/IdGenerator.h"
#include "utils/SsrcGenerator.h"
#include <gtest/gtest.h>

template <typename GeneratorType, typename ValueType>
void generate(ValueType& a1)
{
    auto g1 = new GeneratorType();
    a1 = g1->next();
    delete g1;
}
TEST(SsrcGenerator, realloc)
{
    uint32_t a1, a2;
    generate<utils::SsrcGenerator>(a1);
    generate<utils::SsrcGenerator>(a2);
    EXPECT_NE(a1, a2);
}

TEST(StunIdGenerator, realloc)
{
    ice::Int96 a1, a2;
    generate<ice::StunTransactionIdGenerator>(a1);
    generate<ice::StunTransactionIdGenerator>(a2);
    EXPECT_NE(a1, a2);
}

TEST(IdGenerator, realloc)
{
    uint64_t a1, a2;
    generate<utils::IdGenerator>(a1);
    generate<utils::IdGenerator>(a2);
    EXPECT_NE(a1, a2);
}