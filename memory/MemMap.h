#pragma once
#include <cstring>
#include <sys/mman.h>

namespace memory
{

class MemMap
{
public:
    MemMap() : _data(nullptr), _size(0) {}
    ~MemMap();

    bool allocate(size_t size, int prot, int flags, int fd = -1, off_t offset = 0);
    void free();

    bool isGood() const { return _data; }

    template <typename T>
    T* get()
    {
        return reinterpret_cast<T*>(_data);
    };

    template <typename T>
    const T* get() const
    {
        return reinterpret_cast<const T*>(_data);
    };

private:
    void* _data;
    size_t _size;
};
} // namespace memory
