#pragma once

#include "memory/PoolAllocator.h"
#include "memory/PacketPoolAllocator.h"
#include "memory/Array.h"
#include <vector>
#include <memory>

namespace memory
{
struct ReadonlyMemoryBuffer
{
    ReadonlyMemoryBuffer() : data(nullptr), length(0) {}

    const void* data;
    size_t length;
    std::unique_ptr<memory::Array<char, 2048>> storage;
};

template <typename TPoolAllocator>
class PoolBuffer
{
public:
    struct Deleter
    {
        Deleter() : _allocator(nullptr) {}
        explicit Deleter(TPoolAllocator& allocator) : _allocator(&allocator) {}

        void operator()(PoolBuffer<TPoolAllocator>* p)
        {
            if (p)
            {
                p->~PoolBuffer();
                if (_allocator)
                {
                    _allocator->free(p);
                }
            }
        }

    private:
        TPoolAllocator* _allocator;
    };

    class view
    {
    public:
        view(void* const* chunkPointers,
            size_t numChunks,
            size_t size,
            size_t offset,
            const TPoolAllocator& allocator)
            : _chunkPointers(chunkPointers),
              _numChunks(numChunks),
              _size(size),
              _offset(offset),
              _allocator(allocator)
        {
        }

        view subview(size_t offset, size_t size) const
        {
            return view(_chunkPointers,
                _numChunks,
                std::min(_size - offset, size),
                _offset + offset,
                _allocator);
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

            if (chunkIndex >= _numChunks)
            {
                return false;
            }

            const auto* chunk = static_cast<const uint8_t*>(_chunkPointers[chunkIndex]);
            return chunk[offsetInChunk] == '\0';
        }

        template <size_t SIZE>
        size_t read(memory::Array<char, SIZE>& array) const
        {
            if (array.resize(_size) != _size)
            {
                return 0;
            }
            return read(array.begin(), _size);
        }

        size_t read(void* destination, size_t count) const
        {
            size_t bytesRead = 0;
            size_t currentOffset = _offset;
            size_t remainingToRead = std::min(count, _size);

            if (!_chunkPointers || remainingToRead == 0)
            {
                return 0;
            }

            const auto elementSize = _allocator.getElementSize();
            size_t chunkIndex = currentOffset / elementSize;
            size_t offsetInChunk = currentOffset % elementSize;

            while (bytesRead < remainingToRead && chunkIndex < _numChunks)
            {
                const uint8_t* sourceChunk = static_cast<const uint8_t*>(_chunkPointers[chunkIndex]);
                size_t toReadFromChunk = std::min({remainingToRead - bytesRead, elementSize - offsetInChunk});

                std::memcpy(static_cast<uint8_t*>(destination) + bytesRead, sourceChunk + offsetInChunk, toReadFromChunk);

                bytesRead += toReadFromChunk;
                offsetInChunk = 0;
                chunkIndex++;
            }
            return bytesRead;
        }

    private:
        void* const* _chunkPointers;
        size_t _numChunks;
        size_t _size;
        size_t _offset;
        const TPoolAllocator& _allocator;
    };

    explicit PoolBuffer(TPoolAllocator& allocator)
        : _allocator(allocator),
          _masterChunk(nullptr),
          _size(0),
          _numChunks(0),
          _externalMasterChunkSize(0)
    {}

    explicit PoolBuffer(TPoolAllocator& allocator, void* preallocatedMasterChunk, size_t _masterChunkSize)
        : _allocator(allocator),
          _masterChunk(preallocatedMasterChunk),
          _size(0),
          _numChunks(0),
          _externalMasterChunkSize(_masterChunkSize) // This buffer does NOT own masterChunk
    {}

    PoolBuffer(PoolBuffer&& other) noexcept
        : _allocator(other._allocator),
          _masterChunk(other._masterChunk),
          _size(other._size),
          _numChunks(other._numChunks),
          _externalMasterChunkSize(other._externalMasterChunkSize)
    {
        other._masterChunk = nullptr;
        other._size = 0;
        other._numChunks = 0;
    }

    PoolBuffer& operator=(PoolBuffer&& other) noexcept
    {
        if (this != &other)
        {
            clear();
            _masterChunk = other._masterChunk;
            _externalMasterChunkSize = other._externalMasterChunkSize;
            _size = other._size;
            _numChunks = other._numChunks;

            other._masterChunk = nullptr;
            other._externalMasterChunkSize = 0;
            other._size = 0;
            other._numChunks = 0;
        }
        return *this;
    }

    ~PoolBuffer() { clear(); }

    PoolBuffer(const PoolBuffer&) = delete;
    PoolBuffer& operator=(const PoolBuffer&) = delete;

    bool allocate(size_t size)
    {
        if (size > capacity())
        {
            clear();
            const auto elementSize = _allocator.getElementSize();
            const auto masterChunkCapacity = _externalMasterChunkSize > 0 ? _externalMasterChunkSize : elementSize;
            _numChunks = (size + elementSize - 1) / elementSize;
            if (_numChunks == 0)
            {
                _size = 0;
                return true;
            }

            if (masterChunkCapacity < _numChunks * sizeof(void*))
            {
                logger::error("master chunk is too small to hold chunk pointers. element size %zu, chunks %zu",
                    "PoolBuffer",
                    masterChunkCapacity,
                    _numChunks);
                _numChunks = 0;
                return false;
            }

            if (0 == _externalMasterChunkSize || !_masterChunk) {
                _masterChunk = _allocator.allocate();
                _externalMasterChunkSize = 0;
            }
            if (!_masterChunk)
            {
                _numChunks = 0;
                return false;
            }

            void** chunkPointers = reinterpret_cast<void**>(_masterChunk);
            for (size_t i = 0; i < _numChunks; ++i)
            {
                chunkPointers[i] = _allocator.allocate();
                if (!chunkPointers[i])
                {
                    for (size_t j = 0; j < i; ++j)
                    {
                        _allocator.free(chunkPointers[j]);
                    }
                    if (0 == _externalMasterChunkSize) {
                        _allocator.free(_masterChunk);
                    }
                    _masterChunk = nullptr;
                    _numChunks = 0;
                    return false;
                }
            }
        }
        _size = size;
        return true;
    }

    void clear()
    {
        if (_masterChunk)
        {
            void** chunkPointers = reinterpret_cast<void**>(_masterChunk);
            for (size_t i = 0; i < _numChunks; ++i)
            {
                if (chunkPointers[i])
                {
                    _allocator.free(chunkPointers[i]);
                }
            }
            if (0 == _externalMasterChunkSize) {
                _allocator.free(_masterChunk);
            }
        }
        if (0 == _externalMasterChunkSize) {
            _masterChunk = nullptr;
        }

        _numChunks = 0;
        _size = 0;
    }

    size_t size() const { return _size; }
    size_t getLength() const { return _size; }
    size_t capacity() const { return _numChunks * _allocator.getElementSize(); }
    bool empty() const { return _size == 0 || _numChunks == 0 || _masterChunk == nullptr; }
    size_t getChunkCount() const { return _numChunks; }
    bool isMultiChunk() const { return _numChunks > 1; }

    template<typename AllocatorT>
    size_t write(const PoolBuffer<AllocatorT>& src, size_t srcOffset, size_t len, size_t dstOffset = 0)
    {
        const auto destSize = size();
        const auto srcSize = src.size();

        if (dstOffset >= destSize || srcOffset >= srcSize)
        {
            return 0;
        }

        size_t bytesToWrite = std::min(len, srcSize - srcOffset);
        bytesToWrite = std::min(bytesToWrite, destSize - dstOffset);

        if (bytesToWrite == 0 || !_masterChunk)
        {
            return 0;
        }

        const auto elementSize = _allocator.getElementSize();
        auto** chunkPointers = reinterpret_cast<void**>(_masterChunk);
        size_t totalBytesWritten = 0;
        size_t currentDestOffset = dstOffset;
        size_t currentSrcOffset = srcOffset;

        while (totalBytesWritten < bytesToWrite)
        {
            const auto chunkIndex = currentDestOffset / elementSize;
            if (chunkIndex >= _numChunks)
            {
                break;
            }
            const auto offsetInChunk = currentDestOffset % elementSize;

            auto* chunk = static_cast<uint8_t*>(chunkPointers[chunkIndex]);
            const auto spaceInChunk = elementSize - offsetInChunk;
            const auto toCopy = std::min(bytesToWrite - totalBytesWritten, spaceInChunk);

            const auto copied = src.copy(chunk + offsetInChunk, currentSrcOffset, toCopy);
            if (copied == 0)
            {
                break;
            }

            totalBytesWritten += copied;
            currentDestOffset += copied;
            currentSrcOffset += copied;
        }

        return totalBytesWritten;
    }

    size_t write(const PoolBuffer& src, size_t dstOffset = 0) {
        return write(src, 0, src.size(), dstOffset);
    }

    size_t write(const void* data, size_t len, size_t dstOffset = 0)
    {
        const uint8_t* source = static_cast<const uint8_t*>(data);
        size_t bytesWritten = 0;
        size_t remainingToWrite = std::min(len, _size > dstOffset ? _size - dstOffset : 0);

        if (!_masterChunk || remainingToWrite == 0)
        {
            return 0;
        }

        const auto elementSize = _allocator.getElementSize();
        size_t chunkIndex = dstOffset / elementSize;
        size_t offsetInChunk = dstOffset % elementSize;
        void** chunkPointers = reinterpret_cast<void**>(_masterChunk);

        while (bytesWritten < remainingToWrite && chunkIndex < _numChunks)
        {
            uint8_t* targetChunk = static_cast<uint8_t*>(chunkPointers[chunkIndex]);
            size_t toWriteInChunk = std::min(remainingToWrite - bytesWritten, elementSize - offsetInChunk);

            std::memcpy(targetChunk + offsetInChunk, source + bytesWritten, toWriteInChunk);

            bytesWritten += toWriteInChunk;
            offsetInChunk = 0;
            chunkIndex++;
        }

        return bytesWritten;
    }

    size_t copy(void* dest, size_t offset, size_t count) const
    {
        if (!dest)
        {
            return 0;
        }

        size_t bytesCopied = 0;
        const size_t remainingToCopy = std::min(count, _size > offset ? _size - offset : 0);

        if (!_masterChunk || remainingToCopy == 0)
        {
            return 0;
        }

        const auto elementSize = _allocator.getElementSize();
        size_t chunkIndex = offset / elementSize;
        size_t offsetInChunk = offset % elementSize;
        void** chunkPointers = reinterpret_cast<void**>(_masterChunk);

        while (bytesCopied < remainingToCopy && chunkIndex < _numChunks)
        {
            const uint8_t* sourceChunk = static_cast<const uint8_t*>(chunkPointers[chunkIndex]);
            size_t toCopyFromChunk = std::min({remainingToCopy - bytesCopied, elementSize - offsetInChunk});

            std::memcpy(static_cast<uint8_t*>(dest) + bytesCopied, sourceChunk + offsetInChunk, toCopyFromChunk);

            bytesCopied += toCopyFromChunk;
            offsetInChunk = 0;
            chunkIndex++;
        }

        return bytesCopied;
    }


    view getReader() const
    {
        return view(reinterpret_cast<void**>(_masterChunk), _numChunks, _size, 0, _allocator);
    }

    ReadonlyMemoryBuffer getReadonlyBuffer() const
    {
        ReadonlyMemoryBuffer result;

        if (_size == 0)
        {
            return result;
        }

        size_t targetSize = _size;
        bool needsCopy = (_numChunks > 1);

        result.length = targetSize;

        if (!needsCopy)
        {
            result.data = reinterpret_cast<void**>(_masterChunk)[0];
        }
        else
        {
            result.storage = std::make_unique<memory::Array<char, 2048>>(targetSize);
            size_t bytesRead = 0;
            bytesRead = getReader().read(*result.storage);

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

    ReadonlyMemoryBuffer getFirstChunk() const
    {
        ReadonlyMemoryBuffer result;
        if (_masterChunk && _numChunks > 0)
        {
            void** chunkPointers = reinterpret_cast<void**>(_masterChunk);
            result.data = chunkPointers[0];
            result.length = std::min(_size, _allocator.getElementSize());
        }
        return result;
    }

    TPoolAllocator& getAllocator() const { return _allocator; }

    bool isNullTerminated() const { return getReader().isNullTerminated(); }

private:
    TPoolAllocator& _allocator;
    void* _masterChunk;
    size_t _size;
    size_t _numChunks;
    size_t _externalMasterChunkSize;
};

template <typename TPoolAllocator>
using UniquePoolBuffer = std::unique_ptr<PoolBuffer<TPoolAllocator>, typename PoolBuffer<TPoolAllocator>::Deleter>;

template <typename TPoolAllocator>
inline UniquePoolBuffer<TPoolAllocator> makeUniquePoolBuffer(TPoolAllocator& allocator,
    size_t length)
{
    auto pointer = allocator.allocate();
    assert(pointer);
    if (!pointer)
    {
        logger::error("Unable to allocate pool buffer, no space left in pool %s",
            "PoolBuffer",
            allocator.getName().c_str());
        return UniquePoolBuffer<TPoolAllocator>();
    }

    constexpr auto poolBufferAlignment = alignof(PoolBuffer<TPoolAllocator>);
    const auto alignedPoolBufferSize =
        (sizeof(PoolBuffer<TPoolAllocator>) + poolBufferAlignment - 1) & ~(poolBufferAlignment - 1);

    const auto elementSize = allocator.getElementSize();
    const auto storageSize = elementSize > alignedPoolBufferSize ? elementSize - alignedPoolBufferSize : 0;
    const auto maxChunkCount = elementSize > sizeof(void*) ? elementSize / sizeof(void*) : 0;
    const auto optChunkCount = storageSize > sizeof(void*) ? storageSize / sizeof(void*) : 0;

    // Check that we can theoretically fit enough pointers to chunk into master chunk to accomodate all data.
    if (maxChunkCount * elementSize < length) {
        logger::error("Unable to allocate pool buffer, master chunk it too small %s",
            "PoolBuffer",
            allocator.getName().c_str());
        allocator.free(pointer);
        return UniquePoolBuffer<TPoolAllocator>();
    }

    // Try to fit master chunk in already allocated pointer right after PoolBuffer*.
    void* masterChunk = reinterpret_cast<char*>(pointer) + alignedPoolBufferSize;
    auto buffer = (optChunkCount * elementSize >= length)
        ? new (pointer) PoolBuffer<TPoolAllocator>(allocator, masterChunk, storageSize)
        : new (pointer) PoolBuffer<TPoolAllocator>(allocator);

    auto smartBuffer =
        UniquePoolBuffer<TPoolAllocator>(buffer, typename PoolBuffer<TPoolAllocator>::Deleter(allocator));

    if (!smartBuffer->allocate(length))
    {
        return UniquePoolBuffer<TPoolAllocator>();
    }
    return smartBuffer;
}

template <typename TPoolAllocator>
inline UniquePoolBuffer<TPoolAllocator> makeUniquePoolBuffer(TPoolAllocator& allocator,
    const void* data,
    size_t length)
{
    auto buffer = makeUniquePoolBuffer(allocator, length);
    if (!buffer)
    {
        return buffer;
    }

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
