#pragma once
#include "utils/Optional.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace api
{

struct SsrcPair
{
    uint32_t main;
    uint32_t feedback;
};

template <size_t SIZE>
struct SimulcastLevelArray
{
    typedef const SsrcPair* const_iterator;
    typedef SsrcPair* iterator;

    SimulcastLevelArray() : _count(0) {}

    template <size_t N>
    explicit SimulcastLevelArray(SsrcPair (&levelsList)[N]) : _count(N)
    {
        static_assert(N <= MAX_SIZE, "must not exceed capacity of SimulcastLevelArray");
        for (size_t i = 0; i < N; ++i)
        {
            _levels[i] = levelsList[i];
        }
    }

    SimulcastLevelArray(const SimulcastLevelArray& o) { std::memcpy(this, &o, sizeof(o)); };
    SimulcastLevelArray& operator=(const SimulcastLevelArray& o)
    {
        std::memcpy(this, &o, sizeof(o));
        return *this;
    }

    void push_back(const SsrcPair& p)
    {
        if (_count == SIZE)
        {
            return;
        }
        _levels[_count++] = p;
    }

    SsrcPair& operator[](size_t i) { return _levels[i]; }
    const SsrcPair& operator[](size_t i) const { return _levels[i]; }
    size_t size() const { return _count; }

    const_iterator cbegin() const { return _levels; }
    const_iterator cend() const { return (_levels + _count); }
    const_iterator begin() const { return cbegin(); }
    const_iterator end() const { return cend(); }
    iterator begin() { return _levels; }
    iterator end() { return _levels + _count; }

    utils::Optional<uint32_t> getMainFor(uint32_t feedbackSsrc)
    {
        for (auto& p : *this)
        {
            if (p.feedback == feedbackSsrc)
            {
                return utils::Optional<uint32_t>(p.main);
            }
        }
        return utils::Optional<uint32_t>();
    }

    utils::Optional<uint32_t> getFeedbackFor(uint32_t mainSsrc)
    {
        for (auto& p : *this)
        {
            if (p.main == mainSsrc)
            {
                return utils::Optional<uint32_t>(p.feedback);
            }
        }
        return utils::Optional<uint32_t>();
    }

    bool containsMainSsrc(uint32_t ssrc)
    {
        for (auto& p : *this)
        {
            if (p.main == ssrc)
            {
                return true;
            }
        }

        return false;
    }

private:
    SsrcPair _levels[SIZE];
    uint32_t _count = 0;
    static const size_t MAX_SIZE = SIZE;
};

using SimulcastGroup = SimulcastLevelArray<3>;

template <size_t N>
SimulcastGroup makeSsrcGroup(const SsrcPair (&levels)[N])
{
    SimulcastGroup result;
    for (size_t i = 0; i < N; ++i)
    {
        result.push_back(levels[i]);
    }
    return result;
}

template <size_t N>
void append(SimulcastGroup v, SsrcPair (&levels)[N])
{
    for (size_t i = 0; i < N; ++i)
    {
        v.push_back(levels[i]);
    }
}

} // namespace api
