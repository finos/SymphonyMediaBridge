#pragma once

namespace memory
{

template <typename T, size_t SIZE>
class StackArray
{
public:
    typedef const T* const_iterator;
    typedef T* iterator;

    StackArray() : _capacity(SIZE), _size(0), _dataPtr(_data) {}
    explicit StackArray(size_t size) : _capacity(size), _size(0), _dataPtr(_data)
    {
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

    const_iterator cbegin() const { return _dataPtr; }
    const_iterator cend() const { return (_dataPtr + _size); }
    const_iterator begin() const { return cbegin(); }
    const_iterator end() const { return cend(); }
    iterator begin() { return _dataPtr; }
    iterator end() { return _dataPtr + _size; }

    T& operator[](size_t pos) { return _dataPtr[pos]; }
    const T& operator[](size_t pos) const { return _dataPtr[pos]; }

    const T* data() const { return _dataPtr; }
    StackArray& append(const T* vector, size_t count)
    {
        if (_size + count <= _capacity)
        {
            std::memcpy((_dataPtr + _size), vector, count * sizeof(T));
            _size += count;
        }
        return *this;
    }

    size_t capacity() const { return _capacity; }
    size_t size() const { return _size; }
    void clear() { _size = 0; }

    void push_back(const T& value)
    {
        if (_size >= _capacity)
        {
            return;
        }
        _dataPtr[_size++] = value;
    }

private:
    const size_t _capacity;
    size_t _size;
    T* _dataPtr;
    T _data[SIZE];
};
} // namespace memory
