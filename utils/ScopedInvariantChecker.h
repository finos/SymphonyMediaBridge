#pragma once

namespace utils
{

template <typename T>
class ScopedInvariantChecker
{
public:
    explicit ScopedInvariantChecker(T& instance) : _instance(instance) { _instance.checkInvariant(); }
    ~ScopedInvariantChecker() { _instance.checkInvariant(); }

private:
    T& _instance;
};

} // namespace utils
