#pragma once

namespace memory
{

template <typename T, size_t SIZE>
class StackArray
{
public:
    explicit StackArray(size_t size) : _size(size)
    {
        _dataPtr = _data;
        if (size > SIZE)
        {
            _dataPtr = new T[size];
        }
    }

    StackArray(const StackArray&) = delete;
    StackArray& operator=(const StackArray&) = delete;

    ~StackArray()
    {
        if (_dataPtr != _data)
        {
            delete[] _dataPtr;
        }
    }

    T& operator[](size_t pos) { return _dataPtr[pos]; }
    const T& operator[](size_t pos) const { return _dataPtr[pos]; }

    const T* data() const { return _dataPtr; }
    T* data() { return _dataPtr; }

    size_t size() const { return _size; }

private:
    const size_t _size;
    T* _dataPtr;
    T _data[SIZE];
};
} // namespace memory