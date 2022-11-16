#pragma once

#include "utils/Allocator.h"
#include "utils/ScopedReentrancyBlocker.h"
#include <cassert>
#include <cstddef>
#include <cstring>

namespace memory
{

/**
 * Not thread safe.
 */
template <typename T, size_t S, size_t PRE_BUFFER_SIZE = 0>
class RingBuffer
{
public:
    RingBuffer()
        : _readHead(0),
          _writeHead(0),
          _length(0),
          _preBuffering(PRE_BUFFER_SIZE != 0)
#ifdef DEBUG
          ,
          _reentrancyCount(0)
#endif
    {
        _size = page_allocator::pageAlignedSpace(S * sizeof(T));
        _data = reinterpret_cast<T*>(page_allocator::allocate(_size));

        memset(_data, 0, _size);
        memset(_silenceBuffer, 0, silenceBufferSize * sizeof(T));
    }

    ~RingBuffer() { page_allocator::free(_data, _size); }

    /**
     * Reads size elements from the buffer to outData, but does not move the read head.
     */
    bool read(T* outData, const size_t size)
    {
        assert(outData);
        REENTRANCE_CHECK(_reentrancyCount);

        const auto currentLength = _length;
        if (currentLength == 0 || currentLength < size)
        {
            return false;
        }

        if (_readHead + size > S)
        {
            const auto remaining = S - _readHead;
            memcpy(&outData[0], &_data[_readHead], remaining * sizeof(T));
            memcpy(&outData[remaining], &_data[0], (size - remaining) * sizeof(T));
        }
        else
        {
            memcpy(outData, &_data[_readHead], size * sizeof(T));
        }

        return true;
    }

    /**
     * Move the read head size elements.
     */
    void drop(const size_t size)
    {
        REENTRANCE_CHECK(_reentrancyCount);

        const auto currentLength = _length;
        if (currentLength == 0 || currentLength < size)
        {
            return;
        }

        if (_readHead + size > S)
        {
            const auto remaining = S - _readHead;
            _readHead = size - remaining;
        }
        else
        {
            _readHead += size;
        }

        _length -= size;
    }

    /**
     * Reads size elements from the buffer and adds each element of the read data to mixedData, scaled with scaleFactor.
     * Does not move the read head.
     */
    bool addToMix(T* mixedData, const size_t size, const T scaleFactor)
    {
        assert(mixedData);
        assert(scaleFactor != 0);
        REENTRANCE_CHECK(_reentrancyCount);

        const auto currentLength = _length;
        if (currentLength == 0 || currentLength < size)
        {
            return false;
        }

        if (_readHead + size > S)
        {
            const auto remaining = S - _readHead;
            for (auto i = _readHead; i < _readHead + remaining; ++i)
            {
                mixedData[i - _readHead] += _data[i] / scaleFactor;
            }
            for (size_t i = 0; i < size - remaining; ++i)
            {
                mixedData[remaining + i] += _data[i] / scaleFactor;
            }
        }
        else
        {
            for (auto i = _readHead; i < _readHead + size; ++i)
            {
                mixedData[i - _readHead] += _data[i] / scaleFactor;
            }
        }

        return true;
    }

    /**
     * Reads size elements from the buffer and subtracts each element of the read data to mixedData, scaled with
     * scaleFactor. Does not move the read head.
     */
    bool removeFromMix(T* mixedData, const size_t size, const T scaleFactor)
    {
        assert(mixedData);
        assert(scaleFactor != 0);
        REENTRANCE_CHECK(_reentrancyCount);

        const auto currentLength = _length;
        if (currentLength == 0 || currentLength < size)
        {
            return false;
        }

        if (_readHead + size > S)
        {
            const auto remaining = S - _readHead;
            for (auto i = _readHead; i < _readHead + remaining; ++i)
            {
                mixedData[i - _readHead] -= _data[i] / scaleFactor;
            }
            for (size_t i = 0; i < size - remaining; ++i)
            {
                mixedData[remaining + i] -= _data[i] / scaleFactor;
            }
        }
        else
        {
            for (auto i = _readHead; i < _readHead + size; ++i)
            {
                mixedData[i - _readHead] -= _data[i] / scaleFactor;
            }
        }

        return true;
    }

    /**
     * Writes size elements to the buffer from data, and moves the write head.
     */
    bool write(const T* data, const size_t size)
    {
        assert(data);
        REENTRANCE_CHECK(_reentrancyCount);

        if (_length + size > S)
        {
            return false;
        }

        internalWrite(data, size);
        return true;
    }

    /**
     * Add size silence elements to the buffer. The write head is moved.
     */
    void insertSilence(const size_t size)
    {
        assert(size <= silenceBufferSize);
        REENTRANCE_CHECK(_reentrancyCount);

        if (_length + size > S)
        {
            assert(false);
            return;
        }

        internalWrite(_silenceBuffer, size);
    }

    size_t getLength() const { return _length; }

    bool isPreBuffering() const { return _preBuffering; }

    void setPreBuffering() { _preBuffering = true; }

private:
    static const size_t silenceBufferSize = 2048;

    T* _data;
    size_t _size;
    size_t _readHead;
    size_t _writeHead;
    size_t _length;
    bool _preBuffering;
    alignas(8) T _silenceBuffer[silenceBufferSize];

#ifdef DEBUG
    std::atomic_uint32_t _reentrancyCount;
#endif

    void internalWrite(const T* data, const size_t size)
    {
        if (_writeHead + size > S)
        {
            const auto remaining = S - _writeHead;
            memcpy(&_data[_writeHead], data, remaining * sizeof(T));
            memcpy(&_data[0], &data[remaining], (size - remaining) * sizeof(T));
            _writeHead = size - remaining;
        }
        else
        {
            memcpy(&_data[_writeHead], data, size * sizeof(T));
            _writeHead += size;
        }

        _length += size;
        if (_preBuffering && _length >= PRE_BUFFER_SIZE)
        {
            _preBuffering = false;
        }
    }
};

} // namespace memory
