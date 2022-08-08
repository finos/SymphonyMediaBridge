#include "memory/Map.h"
#include <gtest/gtest.h>

using namespace memory;

TEST(Map, Initialize)
{
    ASSERT_NO_FATAL_FAILURE((memory::Map<int, int, 256>()));

    auto map = Map<int, int, 256>();

    ASSERT_EQ(256, map.capacity());
    ASSERT_EQ(0, map.size());
    ASSERT_EQ(true, map.empty());
    ASSERT_EQ(map.begin(), map.end());
    ASSERT_EQ(map.cbegin(), map.cend());
}

TEST(Map, getItemShouldReturnPointerToMutableType)
{
    auto map = Map<int, int, 256>();
    map[1] = 100;
    map[2] = 200;
    map[3] = 300;

    int* i1 = map.getItem(1);
    int* i2 = map.getItem(2);
    int* i3 = map.getItem(3);

    ASSERT_EQ(100, *i1);
    ASSERT_EQ(200, *i2);
    ASSERT_EQ(300, *i3);

    *i1 = 1111;
    *i2 = 2222;
    *i3 = 3333;

    ASSERT_EQ(1111, map[1]);
    ASSERT_EQ(2222, map[2]);
    ASSERT_EQ(3333, map[3]);

    // Assert multiple calls to getItem returns same pointer
    ASSERT_EQ(i1, map.getItem(1));
    ASSERT_EQ(i2, map.getItem(2));
    ASSERT_EQ(i3, map.getItem(3));
}

TEST(Map, getItemShouldReturnTypeWhenTypeIsPointer)
{
    auto map = Map<int, int*, 256>();
    int i1 = 100;
    int i2 = 200;
    int i3 = 300;

    map[1] = &i1;
    map[2] = &i2;
    map[3] = &i3;

    ASSERT_EQ(&i1, map.getItem(1));
    ASSERT_EQ(&i2, map.getItem(2));
    ASSERT_EQ(&i3, map.getItem(3));
}

TEST(Map, getItemShouldReturnNullPointer)
{
    auto map1 = Map<int, int, 256>();
    auto map2 = Map<int, int*, 256>();

    int i1 = 100;
    int i2 = 200;
    int i3 = 300;

    map1[1] = i1;
    map1[2] = i2;
    map1[3] = i3;

    map2[1] = &i1;
    map2[2] = &i2;
    map2[3] = &i3;

    ASSERT_EQ(nullptr, map1.getItem(10));
    ASSERT_EQ(nullptr, map2.getItem(20));
}

TEST(Map, getItemShouldReturnPointerToConst)
{
    // As we use initialized memory, const types are not supported as
    // then internal memory of Map is immutable
    //auto map1 = Map<int, const int, 256>();
    auto map2 = Map<int, const int*, 256>();

    const int i1 = 100;
    const int i2 = 200;
    const int i3 = 300;

    map2[1] = &i1;
    map2[2] = &i2;
    map2[3] = &i3;

    ASSERT_EQ(&i1, map2.getItem(1));
    ASSERT_EQ(&i2, map2.getItem(2));
    ASSERT_EQ(&i3, map2.getItem(3));
}

TEST(Map, getItemConstShouldReturnPointerToMutableType)
{
    const auto constMap1 = Map<int, int, 256>();
    const auto constMap2 = Map<int, int*, 256>();

    ASSERT_EQ(true, std::is_pointer<decltype(constMap1.getItem(0))>::value);
    ASSERT_EQ(false, std::is_const<std::remove_pointer_t<decltype(constMap1.getItem(0))>>::value);
    ASSERT_EQ(true, std::is_pointer<decltype(constMap2.getItem(0))>::value);
    ASSERT_EQ(false, std::is_const<std::remove_pointer_t<decltype(constMap2.getItem(0))>>::value);
}

TEST(Map, getItemConstShouldReturnPointerToImmutableType)
{
    const auto constMap = Map<int, const int*, 256>();

    ASSERT_EQ(true, std::is_pointer<decltype(constMap.getItem(0))>::value);
    ASSERT_EQ(true, std::is_const<std::remove_pointer_t<decltype(constMap.getItem(0))>>::value);
}

TEST(Map, getItemShouldNotReturnPointerToPointer)
{
    auto map = Map<int, int*, 256>();
    const auto constMap = Map<int, int*, 256>();

    ASSERT_EQ(true, std::is_pointer<decltype(map.getItem(0))>::value);
    ASSERT_EQ(true, std::is_pointer<decltype(constMap.getItem(0))>::value);

    // If return pointer to pointer. is_pointer would return true after call remove_ptr.
    ASSERT_EQ(false, std::is_pointer<std::remove_pointer_t<decltype(map.getItem(0))>>::value);
    ASSERT_EQ(false, std::is_pointer<std::remove_pointer_t<decltype(constMap.getItem(0))>>::value);
}