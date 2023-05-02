#pragma once
#include "memory/Allocator.h"
#include "utils/ScopedReentrancyBlocker.h"
#include <algorithm>
#include <atomic>
#include <cassert>

namespace rtp
{

/**
 * Stream buffer that facilitates mixing.
 * You can add and subtract the front of the buffer to a mixed output.
 * You can then pop the front of the buffer.
 * Single producer single consumer thread safe.
 */
template <typename T>
class SpscAudioBuffer
{
public:
    SpscAudioBuffer(size_t count, uint32_t replayMemory)
        : _size(memory::page::alignedSpace((count + replayMemory) * sizeof(T))),
          _readHead(0),
          _writeHead(0),
          _length(0),
          _recentReadSize(0),
          _replayMemorySize(replayMemory)
#ifdef DEBUG
          ,
          _reentranceRead(0),
          _reentranceWrite(0)
#endif
    {
        _data = reinterpret_cast<T*>(memory::page::allocate(_size));
        std::memset(_data, 0, _size);
    }

    ~SpscAudioBuffer() { memory::page::free(_data, _size); }

    void popFront(const uint32_t count)
    {
        REENTRANCE_CHECK(_reentranceRead);

        const auto currentLength = _length.load(std::memory_order_consume);
        const auto toPop = std::min(currentLength, count);

        const auto maxItems = _size / sizeof(T);
        if (_readHead + toPop > maxItems)
        {
            const auto remaining = maxItems - _readHead;
            _readHead = toPop - remaining;
        }
        else
        {
            _readHead += toPop;
        }

        _recentReadSize = 0;
        _length.fetch_sub(toPop);
    }

    /**
     * copies data from replay backlog memory. Useful for concealment attempts
     * You must make sure there is backlog data or you will get whatever garbage is in the buffer
     */
    void replay(T* mixedData, const uint32_t count) const
    {
        assert(mixedData);
        assert(count <= _replayMemorySize);
        REENTRANCE_CHECK(_reentranceRead);

        if (count > _replayMemorySize)
        {
            return;
        }

        const auto maxItems = _size / sizeof(T);
        const auto readStart = (_readHead + maxItems - count) % maxItems;
        if (readStart + count > maxItems)
        {
            const auto remaining = maxItems - readStart;
            for (size_t i = 0; i < remaining; ++i)
            {
                mixedData[i] = _data[readStart + i];
            }
            for (size_t i = 0; i < count - remaining; ++i)
            {
                mixedData[remaining + i] = _data[i];
            }
        }
        else
        {
            for (size_t i = 0; i < count; ++i)
            {
                mixedData[i] = _data[i + readStart];
            }
        }
    }

    /**
     * Writes count elements to the buffer from data, and moves the write head.
     */
    bool append(const T* data, const uint32_t count)
    {
        assert(data);
        REENTRANCE_CHECK(_reentranceWrite);

        const auto maxItems = _size / sizeof(T);

        const auto currentLength = _length.load(std::memory_order_consume);
        if (currentLength + count + _replayMemorySize > maxItems)
        {
            return false;
        }

        if (_writeHead + count > maxItems)
        {
            const auto remaining = maxItems - _writeHead;
            std::memcpy(&_data[_writeHead], data, remaining * sizeof(T));
            std::memcpy(&_data[0], &data[remaining], (count - remaining) * sizeof(T));
            _writeHead = count - remaining;
        }
        else
        {
            std::memcpy(&_data[_writeHead], data, count * sizeof(T));
            _writeHead += count;
        }

        _length.fetch_add(count);
        return true;
    }

    /**
     * Add count silence elements to the buffer. The write head is moved.
     */
    void appendSilence(const uint32_t count)
    {
        REENTRANCE_CHECK(_reentranceWrite);
        const auto maxItems = _size / sizeof(T);

        const auto currentLength = _length.load(std::memory_order_consume);
        const auto toWrite = std::min(currentLength, count);

        if (_writeHead + toWrite + _replayMemorySize > maxItems)
        {
            const auto remaining = maxItems - _writeHead;
            std::memset(&_data[_writeHead], 0, remaining * sizeof(T));
            std::memset(&_data[0], 0, (toWrite - remaining) * sizeof(T));
            _writeHead = count - remaining;
        }
        else
        {
            std::memset(&_data[_writeHead], 0, toWrite * sizeof(T));
            _writeHead += toWrite;
        }

        _length.fetch_add(toWrite);
    }

    /**
     * Reads count elements from the buffer and adds each element of the read data to mixedData, scaled with
     * scaleFactor. read head is untouched.
     */
    size_t fetch(T* mixedData, const uint32_t count) const
    {
        assert(mixedData);
        REENTRANCE_CHECK(_reentranceRead);

        const auto currentLength = _length.load(std::memory_order_consume);
        const auto toRead = std::min(currentLength, count);

        const auto maxItems = _size / sizeof(T);
        if (_readHead + toRead > maxItems)
        {
            const auto remaining = maxItems - _readHead;
            for (size_t i = 0; i < remaining; ++i)
            {
                mixedData[i] = _data[i + _readHead];
            }
            for (size_t i = 0; i < toRead - remaining; ++i)
            {
                mixedData[remaining + i] = _data[i];
            }
        }
        else
        {
            for (size_t i = 0; i < toRead; ++i)
            {
                mixedData[i] = _data[i + _readHead];
            }
        }

        _recentReadSize = toRead;
        return toRead;
    }

    size_t size() const { return _length.load(std::memory_order_consume); }
    bool empty() const { return size() == 0; }

private:
    const uint32_t _size;
    T* _data;
    uint32_t _readHead;
    uint32_t _writeHead;
    std::atomic_uint32_t _length;
    mutable uint32_t _recentReadSize;
    const uint32_t _replayMemorySize;

#ifdef DEBUG
    mutable std::atomic_uint32_t _reentranceRead;
    mutable std::atomic_uint32_t _reentranceWrite;
#endif
};

} // namespace rtp
