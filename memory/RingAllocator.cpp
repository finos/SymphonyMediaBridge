#include "RingAllocator.h"
#include "utils/Allocator.h"
#include <cassert>
#include <cstddef>

namespace memory
{

namespace
{
size_t alignSize(size_t size, size_t pageSize)
{
    const auto remaining = size % pageSize;
    return size + (remaining > 0 ? pageSize - remaining : 0);
}

struct Block
{
    uint32_t guard;
    uint32_t size;
    uint8_t* data() { return reinterpret_cast<uint8_t*>(&size + 1); }
};

const uint32_t ALLOCATED = 0xAB0CADED;
const uint32_t FREE = 0xFFBBEEDD;
} // namespace

RingAllocator::RingAllocator(size_t size)
    : _size(memory::page::alignedSpace(size)),
      _area(reinterpret_cast<uint8_t*>(memory::page::allocate(_size))),
      _head(0),
      _tail(0)
{
}

RingAllocator::~RingAllocator()
{
    memory::page::free(_area, _size);
}

// allocates continuous block after tail of buffer.
void* RingAllocator::alloc(const size_t size)
{
    const size_t blockSize = sizeof(Block) + alignSize(size, sizeof(uint64_t));

    if (blockSize > capacity())
    {
        return nullptr;
    }

    const uint32_t tailSize = _size - (_tail % _size);
    if (tailSize < blockSize)
    {
        if (_tail + tailSize + blockSize - _head > _size)
        {
            return nullptr;
        }

        Block& block = reinterpret_cast<Block&>(_area[_tail % _size]);
        block.guard = FREE;
        block.size = tailSize;
        _tail += tailSize;
    }

    Block& block = reinterpret_cast<Block&>(_area[_tail % _size]);
    block.guard = ALLOCATED;
    block.size = blockSize;
    _tail += blockSize;
    return block.data();
}

void RingAllocator::free(void* data)
{
    {
        auto* p = reinterpret_cast<uint8_t*>(data) - sizeof(Block);
        Block& block = reinterpret_cast<Block&>(*p);
        assert(block.guard == ALLOCATED);
        block.guard = FREE;
    }

    for (auto* block = reinterpret_cast<const Block*>(&_area[_head % _size]); //
         _head < _tail && block->guard == FREE;
         block = reinterpret_cast<const Block*>(&_area[_head % _size]))
    {
        _head += block->size;
    }

    if (_head > _size)
    {
        _head = _head % _size;
        _tail = _tail % _size;
    }
}
} // namespace memory