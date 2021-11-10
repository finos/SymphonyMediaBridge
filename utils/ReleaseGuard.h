#pragma once

namespace utils
{

template <class T>
class ReleaseGuard
{
public:
    explicit ReleaseGuard(T& o) : _o(o) {}
    ~ReleaseGuard() { _o.release(); }

    // copies are not allowed
    ReleaseGuard(const ReleaseGuard<T>&) = delete;
    ReleaseGuard<T>& operator=(const ReleaseGuard<T>&) = delete;

private:
    T& _o;
};

} // namespace utils
