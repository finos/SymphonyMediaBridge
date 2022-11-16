#pragma once

#include <cassert>
#include <sys/mman.h>
#include <unistd.h>

// set DISABLE_MMAP to disable direct page allocation mapping.
// Disabling this can be use full to run tools to leak detection that can't track mmap, like heaptrack
#if !defined(DISABLE_MMAP)
#define DISABLE_MMAP 0
#endif

#if DISABLE_MMAP
#include <stdlib.h>
#include <string.h>
#endif

namespace page_allocator
{

inline size_t getPageSize()
{
    return sysconf(_SC_PAGE_SIZE);
}

inline void* allocate(size_t size)
{
    assert((size & ~(getPageSize() - 1)) == size);
#if !DISABLE_MMAP
    auto* const mem = mmap(nullptr, size, (PROT_READ | PROT_WRITE), (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0);
#else
    auto* const mem = ::aligned_alloc(getPageSize(), size);
    // initialize memory to zero to mimics behaviour of MAP_ANONYMOUS
    ::memset(mem, 0, size);
#endif
    assert(mem && reinterpret_cast<intptr_t>(mem) != -1);
    return mem;
}

inline void free(void* mem, size_t size)
{
    assert((size & ~(getPageSize() - 1)) == size);
#if !DISABLE_MMAP
    const int ret = munmap(mem, size);
    (void)ret; // silence warning of not using variable on release builds
    assert(ret == 0);
#else
    ::free(mem);
#endif
}

inline size_t pageAlignedSpace(size_t space)
{
    const size_t pageSize = getPageSize();
    return (space + pageSize - 1) & ~(pageSize - 1);
}

} // namespace page_allocator
