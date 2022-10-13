#pragma once

namespace memory
{

template <typename T, size_t SIZE>
class Array
{
public:
    typedef const T* const_iterator;
    typedef T* iterator;

    Array() : _capacity(SIZE), _size(0), _dataPtr(reinterpret_cast<T*>(_data)) {}
    explicit Array(size_t size) : _capacity(size), _size(0), _dataPtr(reinterpret_cast<T*>(_data))
    {
        if (size > SIZE)
        {
            _dataPtr = reinterpret_cast<T*>(malloc(size * sizeof(T)));
        }
    }

    Array(const Array&) = delete;
    Array& operator=(const Array& other)
    {
        clear();
        if (other.size() > _capacity)
        {
            if (_dataPtr != reinterpret_cast<T*>(_data))
            {
                free(_dataPtr);
            }
            _dataPtr = reinterpret_cast<T*>(malloc(other._capacity * sizeof(T)));
        }

        append(other._dataPtr, other._size);
        return *this;
    }

    ~Array()
    {
        clear();
        if (_dataPtr != reinterpret_cast<T*>(_data))
        {
            free(_dataPtr);
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
    Array& append(const T* vector, size_t count)
    {
        if (_size + count <= _capacity)
        {

            for (size_t i = 0; i < count; ++i)
            {
                new (&_dataPtr[_size++]) T(vector[i]);
            }
        }

        return *this;
    }

    size_t capacity() const { return _capacity; }
    size_t size() const { return _size; }
    bool empty() const { return _size == 0; }

    void clear()
    {
        for (size_t i = 0; i < _size; ++i)
        {
            _dataPtr[i].~T();
        }
        _size = 0;
    }

    void push_back(const T& value)
    {
        if (_size >= _capacity)
        {
            return;
        }
        new (&_dataPtr[_size++]) T(value);
    }

    T& back()
    {
        if (_size == 0)
        {
            return *_dataPtr;
        }
        return _dataPtr[_size - 1];
    }

    const T& back() const { return const_cast<Array<T, SIZE>&>(*this).back(); }

private:
    const size_t _capacity;
    size_t _size;
    T* _dataPtr;
    typename std::aligned_storage<sizeof(T), alignof(T)>::type _data[SIZE];
};
} // namespace memory
