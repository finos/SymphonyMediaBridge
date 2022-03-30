#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace memory
{

// Allocate in fixed circular buffer.
// Blocks can be freed in any order but recycling can only occur when
// first allocated block is freed
class RingAllocator
{
public:
    explicit RingAllocator(size_t maxSize);
    ~RingAllocator();

    void* alloc(size_t size);

    template <typename T, typename... Args>
    T* instantiate(size_t extraSpace, Args&&... args)
    {
        static_assert(std::is_trivially_copyable<T>(), "Only POD types allowed. No destructor will be called");
        auto* m = alloc(sizeof(T) + extraSpace);
        if (!m)
        {
            return nullptr;
        }
        auto* item = new (m) T(std::forward<Args>(args)...);
        return item;
    }

    size_t capacity() const { return _size - (_tail - _head); }
    bool empty() const { return _head == _tail; }
    void free(void* data);

private:
    uint32_t _head;
    uint32_t _tail;
    uint8_t* _area;
    const size_t _size;
};

} // namespace memory
