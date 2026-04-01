#pragma once

#include "memory/PoolAllocator.h"

namespace memory
{

template <size_t ELEMENT_SIZE>
class PoolBuffer
{
    struct ChunkDeleter
    {
        PoolAllocator<ELEMENT_SIZE>* allocator;
        void operator()(void* p) const
        {
            if (allocator)
            {
                allocator->free(p);
            }
        }
    };
    using ChunkPtr = std::unique_ptr<void, ChunkDeleter>;

public:
    class view
    {
    public:
        view(const std::vector<ChunkPtr>& chunks, size_t size, size_t offset)
            : _chunks(chunks),
              _size(size),
              _offset(offset)
        {
        }

        view subview(size_t offset, size_t size) const
        {
            return view(_chunks, std::min(_size - offset, size), _offset + offset);
        }

        size_t read(void* destination, size_t count) const
        {
            size_t bytesRead = 0;
            size_t currentOffset = _offset;
            size_t remainingToRead = std::min(count, _size);

            if (_chunks.empty() || remainingToRead == 0)
            {
                return 0;
            }

            size_t chunkIndex = currentOffset / ELEMENT_SIZE;
            size_t offsetInChunk = currentOffset % ELEMENT_SIZE;

            while (bytesRead < remainingToRead && chunkIndex < _chunks.size())
            {
                const uint8_t* sourceChunk = static_cast<const uint8_t*>(_chunks[chunkIndex].get());
                size_t toReadFromChunk = std::min({remainingToRead - bytesRead, ELEMENT_SIZE - offsetInChunk});

                std::memcpy(static_cast<uint8_t*>(destination) + bytesRead, sourceChunk + offsetInChunk, toReadFromChunk);

                bytesRead += toReadFromChunk;
                offsetInChunk = 0;
                chunkIndex++;
            }
            return bytesRead;
        }

    private:
        const std::vector<ChunkPtr>& _chunks;
        size_t _size;
        size_t _offset;
    };

    explicit PoolBuffer(PoolAllocator<ELEMENT_SIZE>& allocator) : _allocator(allocator), _size(0) {}

    PoolBuffer(PoolBuffer&& other) noexcept
        : _allocator(other._allocator),
          _chunks(std::move(other._chunks)),
          _size(other._size)
    {
        other._size = 0;
    }

    PoolBuffer& operator=(PoolBuffer&& other) noexcept
    {
        if (this != &other)
        {
            _chunks = std::move(other._chunks);
            _size = other._size;
            other._size = 0;
        }
        return *this;
    }

    ~PoolBuffer() = default;

    PoolBuffer(const PoolBuffer&) = delete;
    PoolBuffer& operator=(const PoolBuffer&) = delete;

    bool allocate(size_t size)
    {
        if (size > capacity())
        {
            clear();
            size_t numChunks = (size + ELEMENT_SIZE - 1) / ELEMENT_SIZE;
            _chunks.reserve(numChunks);
            for (size_t i = 0; i < numChunks; ++i)
            {
                void* chunk = _allocator.allocate();
                if (!chunk)
                {
                    _chunks.clear();
                    _size = 0;
                    return false;
                }
                _chunks.emplace_back(chunk, ChunkDeleter{&_allocator});
            }
        }
        _size = size;
        return true;
    }

    void clear()
    {
        _chunks.clear();
        _size = 0;
    }

    size_t size() const { return _size; }
    size_t capacity() const { return _chunks.size() * ELEMENT_SIZE; }
    bool empty() const { return _size == 0; }

    size_t write(const void* data, size_t len, size_t offset = 0)
    {
        const uint8_t* source = static_cast<const uint8_t*>(data);
        size_t bytesWritten = 0;
        size_t remainingToWrite = std::min(len, _size - offset);

        if (_chunks.empty() || offset >= _size)
        {
            return 0;
        }

        size_t chunkIndex = offset / ELEMENT_SIZE;
        size_t offsetInChunk = offset % ELEMENT_SIZE;

        while (bytesWritten < remainingToWrite && chunkIndex < _chunks.size())
        {
            uint8_t* targetChunk = static_cast<uint8_t*>(_chunks[chunkIndex].get());
            size_t toWriteInChunk = std::min(remainingToWrite - bytesWritten, ELEMENT_SIZE - offsetInChunk);

            std::memcpy(targetChunk + offsetInChunk, source + bytesWritten, toWriteInChunk);

            bytesWritten += toWriteInChunk;
            offsetInChunk = 0;
            chunkIndex++;
        }

        return bytesWritten;
    }

    view getReader() const { return view(_chunks, _size, 0); }

    const std::vector<ChunkPtr>& getChunks() const { return _chunks; }

private:
    PoolAllocator<ELEMENT_SIZE>& _allocator;
    std::vector<ChunkPtr> _chunks;
    size_t _size;
};

}
