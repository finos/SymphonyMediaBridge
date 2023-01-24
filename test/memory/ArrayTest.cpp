#include "memory/Array.h"
#include "logger/Logger.h"
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
    ~Complex()
    {
        data[0] = 0xCDCDCDCDu;
        delete[] dyn;
    }

    Complex(uint32_t n) : m("test"), dynSize(n)
    {
        data[0] = n;
        data[1] = 15;
        dyn = new uint32_t[n];
    }

    Complex(Complex&& o) : dynSize(o.dynSize)
    {
        data[0] = o.data[0];
        data[1] = 15;
        m = std::move(o.m);
        dyn = o.dyn;
        o.dyn = nullptr;
    }

    Complex(const Complex& o) : dynSize(o.dynSize)
    {
        data[0] = o.data[0];
        data[1] = 15;
        m = o.m;
        if (o.dyn)
        {
            dyn = new uint32_t[o.dynSize];
            std::memcpy(dyn, o.dyn, dynSize);
        }
        else
        {
            dyn = nullptr;
        }
    }

    uint32_t data[32];
    std::string m;
    uint32_t* dyn;
    const size_t dynSize;
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
    EXPECT_EQ(a.size(), 20);
    utils::append(a, b);
    EXPECT_EQ(a.size(), 25);

    EXPECT_EQ(a.insert(a.begin() + 20, b.begin(), b.end()), a.begin() + 20);
    EXPECT_EQ(a.size(), 30);
    uint32_t expectedSequence[] = {0,
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        11,
        12,
        13,
        14,
        15,
        16,
        17,
        18,
        19,
        100,
        101,
        102,
        103,
        104,
        100,
        101,
        102,
        103,
        104};
    for (size_t i = 0; i < a.size(); ++i)
    {
        EXPECT_EQ(a[i].data[0], expectedSequence[i]);
    }
}
