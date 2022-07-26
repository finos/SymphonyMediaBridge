#pragma once

#include <cassert>
#include <utility>

namespace utils
{

template <typename T>
class Optional
{
public:
    Optional() : _isSet(false), _data() {}

    template <typename... U>
    explicit Optional(U&&... args) : _isSet(true),
                                     _data(std::forward<U>(args)...)
    {
    }

    bool isSet() const { return _isSet; }

    const T& get() const
    {
        assert(_isSet);
        return _data;
    }

    T& get()
    {
        assert(_isSet);
        return _data;
    }

    constexpr T valueOr(const T&& defaultValue) const { return _isSet ? _data : defaultValue; }

    template <typename... U>
    T& set(U&&... args)
    {
        _data = T(std::forward<U>(args)...);
        _isSet = true;
        return _data;
    }

    bool operator==(const Optional<T>& other) const
    {
        if (!_isSet && !other._isSet)
        {
            return true;
        }

        if ((_isSet && !other._isSet) || (!_isSet && other._isSet))
        {
            return false;
        }

        return _data == other._data;
    }

    void clear() { _isSet = false; }

    using ValueType = T;

private:
    bool _isSet;
    T _data;
};

} // namespace utils
