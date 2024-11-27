#include "TestValues.h"
#include "concurrency/MpmcHashmap.h"
#include "logger/Logger.h"
#include "utils/SocketAddress.h"
#include "utils/SsrcGenerator.h"
#include <cassert>
#include <cstdint>
#include <gtest/gtest.h>
#include <thread>
#include <utils/Time.h>

void murmurMain(concurrency::MurmurHashIndex& index,
    int id,
    size_t keyCount,
    std::atomic_bool& running,
    std::atomic_int& totalOps)
{
    std::set<uint32_t> keys;

    while (keys.size() < keyCount)
    {
        keys.insert(keyCount * id * 4 + (rand() % (keyCount * 4)));
    }

    logger::debug("key range %u %u thread %d", "", *keys.begin(), *keys.rbegin(), id);

    struct KeyInfo
    {
        uint32_t key = 0;
        bool inserted = false;
    };
    std::vector<KeyInfo> keyVector;
    for (auto key : keys)
    {
        keyVector.push_back({key, false});
    }
    assert(keys.size() == keyVector.size());
    int ops = 0;
    for (int i = 0; running.load(); ++i)
    {
        auto& keyInfo = keyVector[rand() % keyVector.size()];
        if (index.add(keyInfo.key, keyInfo.key + 32))
        {
            ++ops;
            keyInfo.inserted = true;
            ASSERT_TRUE(index.containsKey(keyInfo.key));
        }
        else
        {
            ASSERT_TRUE(index.containsKey(keyInfo.key));
            ASSERT_TRUE(keyInfo.inserted);
        }

        if ((i % 4) == 0)
        {
            auto& key2 = keyVector[rand() % keyVector.size()];
            const bool hasKey = index.containsKey(key2.key);
            ASSERT_EQ(hasKey, key2.inserted);
            ASSERT_EQ(hasKey, index.remove(key2.key));
            key2.inserted = false;
            ++ops;
        }
    }

    for (auto& item : keyVector)
    {
        ASSERT_EQ(index.containsKey(item.key), item.inserted);
        if (item.inserted)
        {
            uint32_t dummy;
            ASSERT_TRUE(index.get(item.key, dummy));
        }
    }

    totalOps += ops;
}

TEST(MpmcMap, MurmurConcurrency)
{
    const int TCOUNT = 8;
    std::thread* threads[TCOUNT];

    concurrency::MurmurHashIndex index(4096 * TCOUNT * 2);

    std::atomic_int totalOps(0);
    std::atomic_bool running(true);
    for (int i = 0; i < TCOUNT; ++i)
    {
        threads[i] =
            new std::thread([&index, &running, i, &totalOps]() { murmurMain(index, i, 4096, running, totalOps); });
    }

    auto start = utils::Time::getAbsoluteTime();
    while (utils::Time::diffLT(start, utils::Time::getAbsoluteTime(), 10 * utils::Time::sec))
    {
        utils::Time::nanoSleep(100 * utils::Time::ms);
    }
    running = false;
    for (int i = 0; i < TCOUNT; ++i)
    {
        threads[i]->join();
        delete threads[i];
    }

    logger::info("ops %d", "", totalOps.load());
}

TEST(MpmcMap, fullCondition)
{
    concurrency::MpmcHashmap32<uint32_t, Simple> hmap(64);
    for (int i = 0; i < 67; ++i)
    {
        EXPECT_TRUE(i >= 64 || hmap.emplace(i, Simple()).second);
    }
}

TEST(MpmcMap, emptyCondition)
{
    concurrency::MpmcHashmap32<uint32_t, Simple> hmap(64);
    for (int i = 0; i < 64; ++i)
    {
        EXPECT_TRUE(hmap.emplace(i, Simple()).second);
    }
    for (int i = 0; i < 64; ++i)
    {
        auto it = hmap.find(i);
        EXPECT_TRUE(it != hmap.cend());
    }
    for (int i = 0; i < 64; ++i)
    {
        hmap.erase(i);
    }
    for (int i = 0; i < 64; ++i)
    {
        EXPECT_FALSE(hmap.contains(i));
    }
}

template <typename KeyT>
struct Complicated
{
    Complicated(KeyT _ssrc, int _seq) : ssrc(_ssrc), seqNo(_seq) {}
    KeyT ssrc;
    int seqNo;
    std::atomic_flag f = ATOMIC_FLAG_INIT;
    int data[32];
};

template <typename HMAP>
__attribute__((no_sanitize("thread"))) void hasherRun(int threadIndex, HMAP& map, bool& running)
{
    struct KeyInfo
    {
        uint64_t key;
        int seqno;
    };
    std::vector<KeyInfo> keys;
    utils::SsrcGenerator gen;
    uint32_t ops = 0;
    int seqCounter = 0;
    int addedKeys = 0;
    const int MAX_KEYS_TO_ADD = 512;
    while (running)
    {
        auto randomInt = rand();
        if ((randomInt % 1024 < 500) && addedKeys < MAX_KEYS_TO_ADD)
        {
            const uint64_t key = threadIndex * 0x10000 + (randomInt % 0x10000);
            int seqNo = seqCounter++;
            if (map.emplace(key, key, seqNo).second)
            {
                ++addedKeys;
                keys.push_back(KeyInfo{key, seqNo});
                auto it = map.find(key);
                ops += 2;
                EXPECT_TRUE(it != map.cend());
                EXPECT_EQ(it->second.seqNo, seqNo);
                EXPECT_EQ(it->second.ssrc, key);
            }
        }

        if ((randomInt % 1024) < 100 && keys.size() > 200)
        {
            auto pos = randomInt % keys.size();
            auto it = std::next(keys.begin(), pos);
            ASSERT_NE(it, keys.end());
            map.erase(it->key);
            keys.erase(it);
            ++ops;
            // cannot put expectation since it may be inserted by other thread
        }

        if ((randomInt % 1024) < 100)
        {
            for (auto& item : map)
            {
                ++ops;
                EXPECT_GE(item.second.seqNo, 0);
            }
            // cannot put expectation since it may be insterted by other thread
        }

        if (keys.size() > 0)
        {
            auto pos = randomInt % keys.size();
            auto& key = keys[pos];
            auto it = map.find(key.key);
            EXPECT_TRUE(it != map.cend());
            ++ops;
            EXPECT_EQ(it->second.seqNo, key.seqno);
            EXPECT_EQ(it->second.ssrc, key.key);
        }
    }

    int keysFound = 0;
    for (auto& item : map)
    {
        EXPECT_GE(item.second.seqNo, 0);
        for (auto& key : keys)
        {
            if (key.key == item.first)
            {
                EXPECT_EQ(item.second.seqNo, key.seqno);
                keysFound++;
                break;
            }
        }
    }
    EXPECT_EQ(keysFound, keys.size());
    logger::info("complete keys %zu ops %u", "hasherRun", keys.size(), ops);
}

TEST(MpmcMap, concurrency)
{
    using HMap = concurrency::MpmcHashmap32<uint64_t, Complicated<uint32_t>>;
    HMap hmap(4096);
    bool running = true;
    const int THREADS = 4;
    std::thread* workers[THREADS];
    for (int i = 0; i < THREADS; ++i)
    {
        workers[i] = new std::thread(hasherRun<HMap>, i, std::ref(hmap), std::ref(running));
    }

    utils::Time::nanoSleep(10 * utils::Time::sec);
    running = false;
    for (int i = 0; i < THREADS; ++i)
    {
        workers[i]->join();
        delete workers[i];
    }
}

TEST(MpmcMap, performance)
{
#ifdef NOPERF_TEST
    GTEST_SKIP();
#endif
    using HMap = concurrency::MpmcHashmap32<uint64_t, Complicated<uint32_t>>;
    HMap hmap(4096);
    bool running = true;
    const int THREADS = 8;
    std::thread* workers[THREADS];
    for (int i = 0; i < THREADS; ++i)
    {
        workers[i] = new std::thread(hasherRun<HMap>, i, std::ref(hmap), std::ref(running));
    }

    utils::Time::nanoSleep(4 * utils::Time::sec);
    running = false;
    for (int i = 0; i < THREADS; ++i)
    {
        workers[i]->join();
        delete workers[i];
    }
}

TEST(MpmcMap, reuse)
{
    using HMap = concurrency::MpmcHashmap32<uint64_t, Complicated<uint32_t>>;
    HMap hmap(4096);

    std::set<uint32_t> keys;
    const int KEYS = 4096 + 1300;
    for (int i = 0; i < KEYS; ++i)
    {
        keys.insert(rand());
    }

    int i = 0;
    for (auto key : keys)
    {
        ASSERT_TRUE(hmap.emplace(key, key, i).second);
        if (i++ % 3 == 0)
        {
            hmap.erase(key);
        }
    }

    i = 0;
    for (auto key : keys)
    {
        EXPECT_EQ(hmap.contains(key), i++ % 3 != 0);
    }
}

std::string generateKey()
{
    const char* validChars = "1234567890QWERTYUIOPLKJHGFDSAZAXCVBNM";
    std::string tmp;
    for (int i = 0; i < 18; ++i)
    {
        tmp += validChars[rand() % strlen(validChars)];
    }
    return tmp;
}

TEST(MpmcMap, stringMap)
{
    using HMap = concurrency::MpmcHashmap32<std::string, Complicated<std::string>>;
    HMap hmap(4096);

    std::set<std::string> keys;
    const int KEYS = 2700;
    for (int i = 0; i < KEYS; ++i)
    {
        keys.insert(generateKey());
    }

    int i = 0;
    for (auto key : keys)
    {
        hmap.emplace(key, key, i);
        if (i++ % 5 == 0)
        {
            hmap.erase(key);
        }
    }

    i = 0;
    for (auto key : keys)
    {
        EXPECT_EQ(hmap.contains(key), i++ % 5 != 0);
    }
}

TEST(MpmcMap, iteration)
{
    using HMap = concurrency::MpmcHashmap32<uint64_t, Complicated<uint32_t>>;
    HMap hmap(4096);

    std::set<uint32_t> keys;
    const int KEYS = 4096 + 1300;
    while (keys.size() < KEYS)
    {
        keys.insert(rand());
    }

    int i = 0;
    for (auto key : keys)
    {
        ASSERT_TRUE(hmap.emplace(key, key, i).second);
        if (i++ % 3 == 0)
        {
            hmap.erase(key);
            for (auto& item : hmap)
            {
                ASSERT_TRUE(item.first != key);
                ASSERT_TRUE(item.second.ssrc != key);
            }
        }
    }

    i = 0;
    for (auto key : keys)
    {
        EXPECT_EQ(hmap.contains(key), i % 3 != 0);
        if ((i % 3) == 0)
        {
            for (auto& item : hmap)
            {
                EXPECT_TRUE(item.first != key);
            }
        }
        ++i;
    }
}

class RequiresDestruction
{
public:
    RequiresDestruction(std::atomic_int& counter) : _counter(counter)
    {
        ++counter;
        ++_allCount;
    }
    ~RequiresDestruction()
    {
        --_counter;
        --_allCount;
    }

    static std::atomic_int _allCount;

private:
    std::atomic_int& _counter;
};
std::atomic_int RequiresDestruction::_allCount(0);

TEST(MpmcMap, entryDestruction)
{
    using HMap = concurrency::MpmcHashmap32<uint32_t, RequiresDestruction>;
    auto hmap = new HMap(128);

    std::atomic_int count(0);
    for (int i = 0; i < 100; ++i)
    {
        hmap->emplace(i, count);
    }

    hmap->erase(55);
    hmap->erase(45);
    EXPECT_EQ(count, 100);
    EXPECT_EQ(RequiresDestruction::_allCount, 100);
    delete hmap;
    EXPECT_EQ(count, 0);
    EXPECT_EQ(RequiresDestruction::_allCount, 0);
}

TEST(MpmcMap, clear)
{
    using HMap = concurrency::MpmcHashmap32<uint32_t, RequiresDestruction>;
    auto hmap = new HMap(128);

    std::atomic_int count(0);
    for (int i = 0; i < 100; ++i)
    {
        hmap->emplace(i, count);
    }

    hmap->erase(55);
    hmap->erase(45);
    EXPECT_EQ(hmap->size(), 98);
    hmap->clear();
    EXPECT_EQ(hmap->size(), 0);
    auto itPair = hmap->emplace(405, count);
    EXPECT_TRUE(itPair.second);
    EXPECT_EQ(itPair.first->first, 405);
    EXPECT_EQ(count, 101); // items are not recycled yet
    delete hmap;
    EXPECT_EQ(count, 0);
}

TEST(MpmcMap, reinit)
{
    using HMap = concurrency::MpmcHashmap32<uint32_t, RequiresDestruction>;
    auto hmap = new HMap(128);

    std::atomic_int count(0);
    for (int i = 0; i < 100; ++i)
    {
        hmap->emplace(i, count);
    }

    hmap->erase(55);
    hmap->erase(45);
    hmap->reInitialize();
    EXPECT_EQ(count, 0);
    EXPECT_EQ(hmap->size(), 0);
    auto itPair = hmap->emplace(405, count);
    EXPECT_TRUE(itPair.second);
    EXPECT_EQ(itPair.first->first, 405);
    EXPECT_EQ(count, 1);
    delete hmap;
    EXPECT_EQ(count, 0);
}

class MockTransport
{
public:
    MockTransport(transport::SocketAddress& g) : _address(g) {}

    transport::SocketAddress _address;
};

TEST(MpmcMap, socketAddress)
{
    // MockTransport aTransport(transport::SocketAddress::parse("35.145.12.34", 1408));
    concurrency::MpmcHashmap32<transport::SocketAddress, MockTransport*> testMap(8 * 1024);

    transport::SocketAddress t(0x23ED5A00 + 35, 1408);

    for (int ip = 35; ip < 36; ++ip)
    {
        for (int port = 1000; port < 8432; ++port)
        {
            transport::SocketAddress a(0x23ED5A00 + ip, port);
            ASSERT_TRUE(testMap.find(a) == testMap.cend());
            auto* mock1 = new MockTransport(a);
            auto it = testMap.emplace(a, mock1);
            ASSERT_TRUE(it.second);
            ASSERT_TRUE(it.first->second->_address == a);
            auto mock2 = new MockTransport(a);
            auto it2 = testMap.emplace(a, mock2);
            ASSERT_TRUE(!it2.second);
            ASSERT_EQ(it2.first->second, mock1);
            delete mock2;
        }
    }

    for (int ip = 35; ip < 36; ++ip)
    {
        for (int port = 1000; port < 8432; ++port)
        {
            transport::SocketAddress a(0x23ED5A00 + ip, port);

            auto it = testMap.find(a);
            ASSERT_TRUE(it != testMap.cend());
            ASSERT_EQ(a, it->second->_address);
        }
    }

    for (auto aPair : testMap)
    {
        ASSERT_TRUE(aPair.first == aPair.second->_address);
        delete aPair.second;
        testMap.erase(aPair.first);
    }

    ASSERT_EQ(testMap.size(), 0);
    ASSERT_EQ(testMap.find(t), testMap.cend());
}

TEST(MpmcMap, socketAddErase)
{
    concurrency::MpmcHashmap32<transport::SocketAddress, MockTransport*> testMap(8 * 1024);

    transport::SocketAddress t(0x23ED5A00 + 35, 1408);

    for (int ip = 35; ip < 36; ++ip)
    {
        for (int port = 1000; port < 16432; ++port)
        {
            transport::SocketAddress a(0x23ED5A00 + ip, port);
            ASSERT_TRUE(testMap.find(a) == testMap.cend());
            auto* mock1 = new MockTransport(a);
            auto it = testMap.emplace(a, mock1);
            ASSERT_TRUE(it.second);
            ASSERT_TRUE(it.first->second->_address == a);
            auto mock2 = new MockTransport(a);
            auto it2 = testMap.emplace(a, mock2);
            ASSERT_TRUE(!it2.second);
            ASSERT_EQ(it2.first->second, mock1);
            delete mock2;
            delete mock1;

            if ((port & 1) == 1)
            {
                testMap.erase(a);
            }
        }
    }
    auto first = reinterpret_cast<const uint8_t*>(&*testMap.begin());
    auto expectedEnd =
        first + 8 * 1024 * (sizeof(std::pair<transport::SocketAddress, MockTransport*>) + sizeof(uint64_t) * 2);
    ASSERT_EQ(reinterpret_cast<const uint8_t*>(&*testMap.cend()), expectedEnd);
}

TEST(MpmcMap, getItem)
{
    Simple a;
    Simple b;
    concurrency::MpmcHashmap32<uint64_t, Simple> hmap1(1024);
    concurrency::MpmcHashmap32<uint64_t, Simple&> hmap2(1024);
    concurrency::MpmcHashmap32<uint64_t, Simple*> hmap3(1024);
    hmap1.emplace(1, a);
    hmap2.emplace(2, b);
    hmap3.emplace(3, &b);

    Simple* p = nullptr;
    p = hmap1.getItem(1);
    EXPECT_EQ(p, &hmap1.find(1)->second);

    p = hmap2.getItem(2);
    EXPECT_EQ(p, &b);

    p = hmap3.getItem(3);
    EXPECT_EQ(p, &b);
}
