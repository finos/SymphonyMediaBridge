#pragma once

#include "memory/PoolAllocator.h"
#include "memory/PacketPoolAllocator.h"
#include "memory/Array.h"
#include <vector>
#include <memory>

namespace memory
{
//template<size_t SIZE>
struct ReadonlyMemoryBuffer
{
    ReadonlyMemoryBuffer() : data(nullptr), length(0) {}

    const void* data;
    size_t length;
    char storage[8192];
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

    explicit PoolBuffer(TPoolAllocator& allocator)
        : _allocator(allocator),
          _masterChunk(nullptr),
          _size(0),
          _numChunks(0),
          _firstChunkSize(0),
          _firstChunkIsInMaster(false)
    {}

    PoolBuffer(TPoolAllocator& allocator, const void* data, size_t len)
        : _allocator(allocator),
          _masterChunk(nullptr),
          _size(0),
          _numChunks(0),
          _firstChunkSize(0),
          _firstChunkIsInMaster(false)
    {
        allocate(len);
        if (data && len > 0)
        {
            copyFrom(data, len, 0);
        }
    }

    PoolBuffer(PoolBuffer&& other) noexcept
        : _allocator(other._allocator),
          _masterChunk(other._masterChunk),
          _size(other._size),
          _numChunks(other._numChunks),
          _firstChunkSize(other._firstChunkSize),
          _firstChunkIsInMaster(other._firstChunkIsInMaster)
    {
        other._masterChunk = nullptr;
        other._size = 0;
        other._numChunks = 0;
        other._firstChunkSize = 0;
        other._firstChunkIsInMaster = false;
    }

    PoolBuffer& operator=(PoolBuffer&& other) noexcept
    {
        if (this != &other)
        {
            clear();
            _masterChunk = other._masterChunk;
            _size = other._size;
            _numChunks = other._numChunks;
            _firstChunkSize = other._firstChunkSize;
            _firstChunkIsInMaster = other._firstChunkIsInMaster;

            other._masterChunk = nullptr;
            other._size = 0;
            other._numChunks = 0;
            other._firstChunkSize = 0;
            other._firstChunkIsInMaster = false;
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
            auto masterChunkCapacity = elementSize;

            if (size == 0)
            {
                _size = 0;
                return true;
            }

            if (!_masterChunk)
            {
                _masterChunk = _allocator.allocate();
                if (!_masterChunk)
                {
                    return false;
                }
                masterChunkCapacity = elementSize;
            }

            size_t numChunks = (size + elementSize - 1) / elementSize;
            if (size <= masterChunkCapacity)
            {
                numChunks = 1;
            }
            size_t pointersAreaSize = numChunks * sizeof(void*);

            if (masterChunkCapacity > pointersAreaSize)
            {
                const size_t firstChunkSize = masterChunkCapacity - pointersAreaSize;
                if (size > firstChunkSize)
                {
                    numChunks = 1 + (size - firstChunkSize + elementSize - 1) / elementSize;
                }
                else
                {
                    numChunks = 1;
                }

                pointersAreaSize = numChunks * sizeof(void*);
                if (masterChunkCapacity > pointersAreaSize)
                {
                    _firstChunkSize = masterChunkCapacity - pointersAreaSize;
                    _numChunks = numChunks;
                    _firstChunkIsInMaster = true;
                }
                else
                {
                    _numChunks = (size + elementSize - 1) / elementSize;
                }
            }
            else
            {
                _numChunks = numChunks;
            }

            if (masterChunkCapacity < _numChunks * sizeof(void*))
            {
                logger::error("master chunk is too small to hold chunk pointers. capacity %zu, required %zu",
                    "PoolBuffer",
                    masterChunkCapacity,
                    _numChunks * sizeof(void*));
                _numChunks = 0;
                if (_masterChunk)
                {
                    _allocator.free(_masterChunk);
                    _masterChunk = nullptr;
                }
                return false;
            }

            void** chunkPointers = reinterpret_cast<void**>(_masterChunk);
            size_t i = 0;
            if (_firstChunkIsInMaster)
            {
                chunkPointers[0] = reinterpret_cast<uint8_t*>(_masterChunk) + _numChunks * sizeof(void*);
                i = 1;
            }

            for (; i < _numChunks; ++i)
            {
                chunkPointers[i] = _allocator.allocate();
                if (!chunkPointers[i])
                {
                    for (size_t j = _firstChunkIsInMaster ? 1 : 0; j < i; ++j)
                    {
                        _allocator.free(chunkPointers[j]);
                    }
                    _allocator.free(_masterChunk);
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
            for (size_t i = _firstChunkIsInMaster ? 1 : 0; i < _numChunks; ++i)
            {
                if (chunkPointers[i])
                {
                    _allocator.free(chunkPointers[i]);
                }
            }
            _allocator.free(_masterChunk);
        }
        _masterChunk = nullptr;

        _numChunks = 0;
        _size = 0;
        _firstChunkIsInMaster = false;
        _firstChunkSize = 0;
    }

    size_t size() const { return _size; }
    size_t getLength() const { return _size; }
    size_t capacity() const
    {
        if (_firstChunkIsInMaster)
        {
            return _firstChunkSize + (_numChunks - 1) * _allocator.getElementSize();
        }
        return _numChunks * _allocator.getElementSize();
    }
    bool empty() const { return _size == 0 || _numChunks == 0 || _masterChunk == nullptr; }
    size_t getChunkCount() const { return _numChunks; }
    bool isMultiChunk() const { return _numChunks > 1; }

    template <typename AllocatorT>
    size_t copyFrom(const PoolBuffer<AllocatorT>& sourceBuffer,
        size_t sourceOffset,
        size_t len,
        size_t destinationOffset = 0)
    {
        const auto destSize = size();
        const auto srcSize = sourceBuffer.size();
        if (destinationOffset >= destSize || sourceOffset >= srcSize)
        {
            return 0;
        }

        const size_t bytesToCopy = std::min({len, srcSize - sourceOffset, destSize - destinationOffset});
        if (bytesToCopy == 0)
        {
            return 0;
        }

        size_t totalBytesCopied = 0;
        size_t currentSrcOffset = sourceOffset;

        auto callback = [&](uint8_t* block, size_t blockSize) {
            const auto copied = sourceBuffer.copyTo(block, currentSrcOffset, blockSize);
            totalBytesCopied += copied;
            currentSrcOffset += copied;
            return copied == blockSize; // Continue only if we copied the whole block
        };

        forEachBlock(destinationOffset, bytesToCopy, callback);

        return totalBytesCopied;
    }

    size_t copyFrom(const PoolBuffer& src, size_t destinationOffset = 0) {
        return copyFrom(src, 0, src.size(), destinationOffset);
    }

    size_t copyFrom(const void* source, size_t len, size_t destinationOffset = 0)
    {
        const uint8_t* sourceData = static_cast<const uint8_t*>(source);
        size_t bytesCopied = 0;
        const size_t remainingToCopy = std::min(len, _size > destinationOffset ? _size - destinationOffset : 0);

        auto callback = [&](uint8_t* block, size_t blockSize) {
            std::memcpy(block, sourceData + bytesCopied, blockSize);
            bytesCopied += blockSize;
            return true;
        };

        forEachBlock(destinationOffset, remainingToCopy, callback);
        return bytesCopied;
    }

    size_t copyTo(void* destination, size_t sourceOffset, size_t count) const
    {
        if (!destination)
        {
            return 0;
        }

        size_t bytesCopied = 0;
        auto callback = [&](const uint8_t* block, size_t blockSize) {
            std::memcpy(static_cast<uint8_t*>(destination) + bytesCopied, block, blockSize);
            bytesCopied += blockSize;
            return true;
        };

        forEachBlock(sourceOffset, count, callback);
        return bytesCopied;
    }

    TPoolAllocator& getAllocator() const { return _allocator; }

    bool isNullTerminated() const { 
        if (_size == 0) {
            return false;
        }
        void** chunkPointers = reinterpret_cast<void**>(_masterChunk);
        const auto chunkAndOffset = getChunkAndOffset(_size - 1);
        const auto data = chunkPointers[chunkAndOffset.first];
        return reinterpret_cast<const char*>(data)[chunkAndOffset.second] == '\0';
     }

private:
    size_t getChunkSize(size_t chunkIndex) const
    {
        if (_firstChunkIsInMaster && chunkIndex == 0)
        {
            return _firstChunkSize;
        }
        return _allocator.getElementSize();
    }

    std::pair<size_t, size_t> getChunkAndOffset(size_t absoluteOffset) const
    {
        const auto elementSize = _allocator.getElementSize();
        if (_firstChunkIsInMaster && _numChunks > 0)
        {
            if (absoluteOffset < _firstChunkSize)
            {
                return {0, absoluteOffset};
            }
            else
            {
                return {1 + (absoluteOffset - _firstChunkSize) / elementSize,
                    (absoluteOffset - _firstChunkSize) % elementSize};
            }
        }
        else
        {
            return {absoluteOffset / elementSize, absoluteOffset % elementSize};
        }
    }

    template <typename T, typename F>
    void forEachBlockImpl(size_t offset, size_t count, F& callback) const
    {
        size_t processedBytes = 0;
        const size_t remainingToProcess = std::min(count, _size > offset ? _size - offset : 0);

        if (!_masterChunk || remainingToProcess == 0)
        {
            return;
        }

        auto chunkInfo = getChunkAndOffset(offset);
        size_t chunkIndex = chunkInfo.first;
        size_t offsetInChunk = chunkInfo.second;

        auto** chunkPointers = reinterpret_cast<void**>(_masterChunk);

        while (processedBytes < remainingToProcess && chunkIndex < _numChunks)
        {
            const auto currentChunkSize = getChunkSize(chunkIndex);
            const size_t toProcessInChunk =
                std::min(remainingToProcess - processedBytes, currentChunkSize - offsetInChunk);

            if (!callback(static_cast<T*>(static_cast<uint8_t*>(chunkPointers[chunkIndex]) + offsetInChunk),
                    toProcessInChunk))
            {
                break;
            }

            processedBytes += toProcessInChunk;
            offsetInChunk = 0;
            chunkIndex++;
        }
    }

    template <typename F>
    void forEachBlock(size_t offset, size_t count, F& callback)
    {
        forEachBlockImpl<uint8_t>(offset, count, callback);
    }

    template <typename F>
    void forEachBlock(size_t offset, size_t count, F& callback) const
    {
        forEachBlockImpl<const uint8_t>(offset, count, callback);
    }

    TPoolAllocator& _allocator;
    void* _masterChunk;
    size_t _size;
    size_t _numChunks;
    size_t _firstChunkSize;
    bool _firstChunkIsInMaster;
};
} // namespace memory