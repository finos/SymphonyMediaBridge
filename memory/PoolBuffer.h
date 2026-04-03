#pragma once

#include "memory/PoolAllocator.h"
#include "memory/Array.h"
#include <vector>
#include <memory>

namespace memory
{

enum class ReadMode
{
    AsIs,
    NullTerminated
};

struct ReadonlyMemoryBuffer
{
    ReadonlyMemoryBuffer() : data(nullptr), length(0) {}

    const void* data;
    size_t length;
    std::unique_ptr<memory::Array<char, 1500>> storage;
};

template <typename TPoolAllocator>
class PoolBuffer
{
    struct ChunkDeleter
    {
        TPoolAllocator* allocator;
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
        view(const std::vector<ChunkPtr>& chunks,
            size_t size,
            size_t offset,
            const TPoolAllocator& allocator)
            : _chunks(chunks),
              _size(size),
              _offset(offset),
              _allocator(allocator)
        {
        }

        view subview(size_t offset, size_t size) const
        {
            return view(_chunks, std::min(_size - offset, size), _offset + offset, _allocator);
        }

        bool isNullTerminated() const
        {
            if (_size == 0)
            {
                return false;
            }

            const auto elementSize = _allocator.getElementSize();
            const size_t lastByteAbsoluteOffset = _offset + _size - 1;
            const size_t chunkIndex = lastByteAbsoluteOffset / elementSize;
            const size_t offsetInChunk = lastByteAbsoluteOffset % elementSize;

            if (chunkIndex >= _chunks.size())
            {
                return false;
            }

            const auto* chunk = static_cast<const uint8_t*>(_chunks[chunkIndex].get());
            return chunk[offsetInChunk] == '\0';
        }

        template<size_t SIZE>
        size_t read(memory::Array<char, SIZE>& array) const
        {
            if (array.resize(_size) != _size)
            {
                return 0;
            }
            return read(array.begin(), _size);
        }

        template<size_t SIZE>
        size_t readAndAppendNullIfNeeded(memory::Array<char, SIZE>& array) const
        {
            size_t extraSize = isNullTerminated() ? 0 : 1;
            if (array.resize(_size + extraSize) != _size + extraSize)
            {
                return 0;
            }

            size_t bytesRead = read(array.begin(), _size);
            if (bytesRead < _size) {
                return 0;
            }

            if (extraSize > 0) {
                array[bytesRead++] = 0;
            }

            return bytesRead;
        }
private:
        size_t read(void* destination, size_t count) const
        {
            size_t bytesRead = 0;
            size_t currentOffset = _offset;
            size_t remainingToRead = std::min(count, _size);

            if (_chunks.empty() || remainingToRead == 0)
            {
                return 0;
            }

            const auto elementSize = _allocator.getElementSize();
            size_t chunkIndex = currentOffset / elementSize;
            size_t offsetInChunk = currentOffset % elementSize;

            while (bytesRead < remainingToRead && chunkIndex < _chunks.size())
            {
                const uint8_t* sourceChunk = static_cast<const uint8_t*>(_chunks[chunkIndex].get());
                size_t toReadFromChunk = std::min({remainingToRead - bytesRead, elementSize - offsetInChunk});

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
        const TPoolAllocator& _allocator;
    };

    explicit PoolBuffer(TPoolAllocator& allocator) : _allocator(allocator), _size(0) {}

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

    //~PoolBuffer() = default;
    ~PoolBuffer()
    {
        clear();
    }

    PoolBuffer(const PoolBuffer&) = delete;
    PoolBuffer& operator=(const PoolBuffer&) = delete;

    bool allocate(size_t size)
    {
        if (size > capacity())
        {
            clear();
            const auto elementSize = _allocator.getElementSize();
            size_t numChunks = (size + elementSize - 1) / elementSize;
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
    size_t getLength() const { return _size; }
    size_t capacity() const { return _chunks.size() * _allocator.getElementSize(); }
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

        const auto elementSize = _allocator.getElementSize();
        size_t chunkIndex = offset / elementSize;
        size_t offsetInChunk = offset % elementSize;

        while (bytesWritten < remainingToWrite && chunkIndex < _chunks.size())
        {
            uint8_t* targetChunk = static_cast<uint8_t*>(_chunks[chunkIndex].get());
            size_t toWriteInChunk = std::min(remainingToWrite - bytesWritten, elementSize - offsetInChunk);

            std::memcpy(targetChunk + offsetInChunk, source + bytesWritten, toWriteInChunk);

            bytesWritten += toWriteInChunk;
            offsetInChunk = 0;
            chunkIndex++;
        }

        return bytesWritten;
    }

    view getReader() const { return view(_chunks, _size, 0, _allocator); }

    ReadonlyMemoryBuffer getReadonlyBuffer(ReadMode readMode = ReadMode::AsIs) const
    {
        ReadonlyMemoryBuffer result;
        const bool nullTerminate = (readMode == ReadMode::NullTerminated);

        if (_size == 0)
        {
            if (nullTerminate)
            {
                result.storage = std::make_unique<memory::Array<char, 1500>>(1);
                (*result.storage)[0] = '\0';
                result.data = result.storage->data();
                result.length = 1;
            }
            return result;
        }

        size_t targetSize = _size;
        bool needsCopy = (_chunks.size() > 1);
        if (nullTerminate && !isNullTerminated())
        {
            targetSize++;
            needsCopy = true;
        }

        result.length = targetSize;

        if (!needsCopy)
        {
            result.data = _chunks[0].get();
        }
        else
        {
            result.storage = std::make_unique<memory::Array<char, 1500>>(targetSize);
            size_t bytesRead = 0;
            if (nullTerminate)
            {
                bytesRead = getReader().readAndAppendNullIfNeeded(*result.storage);
            }
            else
            {
                bytesRead = getReader().read(*result.storage);
            }

            if (bytesRead == targetSize)
            {
                result.data = result.storage->data();
            }
            else
            {
                result.length = 0;
            }
        }

        return result;
    }

    const std::vector<ChunkPtr>& getChunks() const { return _chunks; }

    bool isNullTerminated() const {
        return getReader().isNullTerminated();
    }

private:
    TPoolAllocator& _allocator;
    std::vector<ChunkPtr> _chunks;
    size_t _size;
};

template <typename T>
using UniquePoolBuffer = std::unique_ptr<PoolBuffer<T>>;

template <typename TPoolAllocator>
inline UniquePoolBuffer<TPoolAllocator> makeUniquePoolBuffer(TPoolAllocator& allocator,
    size_t length)
{
    auto buffer = std::unique_ptr<PoolBuffer<TPoolAllocator>>(new PoolBuffer<TPoolAllocator>(allocator));
    if (!buffer->allocate(length))
    {
        return nullptr;
    }
    return buffer;
}

template <typename TPoolAllocator>
inline UniquePoolBuffer<TPoolAllocator> makeUniquePoolBuffer(TPoolAllocator& allocator,
    const void* data,
    size_t length)
{
    auto buffer = makeUniquePoolBuffer(allocator, length);

    if (data)
    {
        if (buffer->write(data, length) != length)
        {
            return nullptr;
        }
    }

    return buffer;
}
}
