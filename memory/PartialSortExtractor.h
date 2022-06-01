#include <algorithm>

namespace memory
{
template <typename T>
class PartialSortExtractor
{
public:
    PartialSortExtractor(T* startIt, T* endIt) : _heap(startIt), _heapEnd(endIt)
    {
        static_assert(std::is_trivial<T>(), "T must be a trivial type");
        std::make_heap(startIt, endIt);
    }

    const T& top() const { return *_heap; }
    T& top() { return *_heap; }

    void pop()
    {
        if (!empty())
        {
            std::pop_heap(_heap, _heapEnd--);
        }
    }

    bool empty() const { return _heap == _heapEnd; }

private:
    T* _heap;
    T* _heapEnd;
};
} // namespace memory
