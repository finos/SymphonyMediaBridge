#include "MemMap.h"
#include <cassert>
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
    _data = mmap(0, size, prot, flags, fd, offset);
    if (_data = MAP_FAILED)
    {
        _data = nullptr;
    }

    return _data != nullptr;
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

} // namespace memory
