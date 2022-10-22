#include "memory/Array.h"
#include "utils/ContainerAlgorithms.h"
#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <vector>

namespace
{
} // namespace

class Complex
{
public:
    Complex() : Complex(12) {}

    Complex(uint32_t n) : m("test")
    {
        data[0] = n;
        data[1] = 15;
    }

    uint32_t data[32];
    std::string m;
};

TEST(ArrayTest, addRemove)
{
    memory::Array<Complex, 32> a;

    a.push_back(Complex());
    a.push_back(Complex(2));

    EXPECT_EQ(a[0].data[0], 12);
    EXPECT_EQ(a[1].data[0], 2);
    EXPECT_EQ(a[0].m, "test");
}

TEST(ArrayTest, append)
{
    memory::Array<Complex, 32> a;
    for (int i = 0; i < 20; ++i)
    {
        a.push_back(Complex(i));
    }

    memory::Array<Complex, 32> b;
    for (int i = 0; i < 5; ++i)
    {
        b.push_back(Complex(i + 100));
    }

    memory::Array<Complex, 32> c;
    for (int i = 0; i < 25; ++i)
    {
        c.push_back(Complex(i + 200));
    }

    EXPECT_EQ(a.insert(a.begin() + 20, c.begin(), c.end()), a.end());

    utils::append(a, b);
    EXPECT_EQ(a.size(), 25);

    EXPECT_EQ(a.insert(a.begin() + 20, b.begin(), b.end()), a.begin() + 20);
}
