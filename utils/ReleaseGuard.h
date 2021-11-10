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
    ReleaseGuard(ReleaseGuard<T>&) = delete;
    ReleaseGuard<T>& operator=(ReleaseGuard<T>&) = delete;

private:
    T& _o;
};

} // namespace utils
