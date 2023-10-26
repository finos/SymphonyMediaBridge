#pragma once
#include <algorithm>
#include <math.h>

namespace math
{

template <typename T>
class WelfordVariance
{
public:
    WelfordVariance() : _mean(0), _s(0), _count(0) {}

    void add(T value)
    {
        ++_count;
        const T prevMean = _mean;
        _mean += (value - _mean) / _count;
        _s += (value - _mean) * (value - prevMean);
    }

    T getMean() const { return _mean; }
    T getVariance() const
    {
        if (_count < 2)
        {
            return 0;
        }
        return _s / (_count - 1);
    }

private:
    T _mean;
    T _s;
    uint32_t _count;
};

template <typename T>
class RollingWelfordVariance
{
public:
    RollingWelfordVariance(uint32_t windowSize)
        : _windowSize(windowSize),
          _mean(0),
          _count(0),
          _cursor(0),
          _varianceAcc(0),
          _values(new T[windowSize]),
          _varValues(new T[windowSize])
    {
    }

    ~RollingWelfordVariance()
    {
        delete[] _values;
        delete[] _varValues;
    }

    void add(T value)
    {
        _cursor = (_cursor + 1) % _windowSize;
        const auto oldMean = getMean();
        if (_count >= _windowSize)
        {
            _mean += value - _values[_cursor];
            const auto newMean = getMean();
            const auto newVarValue = (value - oldMean) * (value - newMean);
            _varianceAcc += newVarValue - _varValues[_cursor];
            _values[_cursor] = value;
            _varValues[_cursor] = newVarValue;
        }
        else
        {
            ++_count;
            _mean += value;
            auto newMean = getMean();
            const auto newVarValue = (value - oldMean) * (value - newMean);
            _varianceAcc += newVarValue;
            _values[_cursor] = value;
            _varValues[_cursor] = newVarValue;
        }
    }

    T getMean() const
    {
        if (_count == 0)
        {
            return 0;
        }
        return _mean / _count;
    }

    T getVariance() const
    {
        if (_count < 2)
        {
            return 0;
        }
        return _varianceAcc / _count;
    }

private:
    const uint32_t _windowSize;
    T _mean;
    uint32_t _count;
    uint32_t _cursor;
    T _varianceAcc;

    T* _values;
    T* _varValues;
};

} // namespace math
