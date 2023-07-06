#pragma once
#include <cstring>
#include <sys/mman.h>

namespace memory
{
size_t roundUpToPage(size_t s);

class MemMap
{
public:
    MemMap() : _data(nullptr), _size(0) {}
    ~MemMap();

    bool allocate(size_t size, int prot, int flags, int fd = -1, off_t offset = 0);
    bool allocateAtLeast(size_t size, int prot, int flags, int fd = -1, off_t offset = 0);
    void free();

    bool isGood() const { return _data; }

    template <typename T>
    T* get(size_t offset = 0)
    {
        return reinterpret_cast<T*>(reinterpret_cast<char*>(_data) + offset);
    }

    template <typename T>
    const T* get(size_t offset) const
    {
        return reinterpret_cast<const T*>(reinterpret_cast<const char*>(_data) + offset);
    }

    size_t size() const { return _size; }

private:
    void* _data;
    size_t _size;
};
} // namespace memory
