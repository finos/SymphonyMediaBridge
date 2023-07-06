#include "MemMap.h"
#include <cassert>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

namespace memory
{

namespace
{

inline size_t getPageSize()
{
    return sysconf(_SC_PAGE_SIZE);
}
} // namespace

bool MemMap::allocate(size_t size, int prot, int flags, int fd, off_t offset)
{
    assert((size & ~(getPageSize() - 1)) == size);
    assert(!_data);
    if (_data)
    {
        return false;
    }

    _data = mmap(nullptr, size, prot, flags, fd, offset);
    if (_data == MAP_FAILED)
    {
        _data = nullptr;
        return false;
    }

    _size = size;
    return true;
}

bool MemMap::allocateAtLeast(size_t size, int prot, int flags, int fd, off_t offset)
{
    return allocate(memory::roundUpToPage(size), prot, flags, fd, offset);
}

void MemMap::free()
{
    if (_data)
    {
        munmap(_data, _size);
        _data = nullptr;
        _size = 0;
    }
}

MemMap::~MemMap()
{
    free();
}

size_t roundUpToPage(size_t s)
{
    size_t remainder = s & (getPageSize() - 1);
    if (remainder > 0)
    {
        return s + getPageSize() - remainder;
    }
    return s;
}

} // namespace memory
