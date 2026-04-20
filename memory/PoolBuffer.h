#pragma once

#include "memory/PoolAllocator.h"
#include "memory/PacketPoolAllocator.h"
#include "memory/Array.h"
#include <vector>
#include <memory>

class PoolBuffer_move_Test;
class PoolBuffer_moveOperator_Test;

namespace memory
{
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
          _numAdditionalChunks(0),
          _firstChunkDataOffset(0)
    {}

    PoolBuffer(TPoolAllocator& allocator, const void* data, size_t len)
        : _allocator(allocator),
          _masterChunk(nullptr),
          _size(0),
          _numAdditionalChunks(0),
          _firstChunkDataOffset(0)
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
          _numAdditionalChunks(other._numAdditionalChunks),
          _firstChunkDataOffset(other._firstChunkDataOffset)
    {
        other._masterChunk = nullptr;
        other._size = 0;
        other._numAdditionalChunks = 0;
        other._firstChunkDataOffset = 0;
    }

    PoolBuffer& operator=(PoolBuffer&& other) noexcept
    {
        if (this != &other)
        {
            free();
            _masterChunk = other._masterChunk;
            _size = other._size;
            _numAdditionalChunks = other._numAdditionalChunks;
            _firstChunkDataOffset = other._firstChunkDataOffset;
            other._masterChunk = nullptr;
            other._size = 0;
            other._numAdditionalChunks = 0;
            other._firstChunkDataOffset = 0;
        }
        return *this;
    }

    ~PoolBuffer() { free(); }

    PoolBuffer(const PoolBuffer&) = delete;
    PoolBuffer& operator=(const PoolBuffer&) = delete;

    /**
     * Allocates a primary (master) chunk that contains either inline data, pointers to
     * secondary chunks, or a combination of both.
     *
     * If the requested capacity exceeds the available inline space of the master chunk,
     * additional secondary chunks are allocated.
     *
     * The payload data begins either within the master chunk (at a specific offset),
     * or within the first secondary chunk if the master chunk's capacity is entirely
     * consumed by pointers to secondary chunks.
     */
    bool allocate(size_t size)
    {
        if (size > capacity())
        {
            free();
            const auto chunkSize = _allocator.getElementSize();

            if (size == 0)
            {
                _size = 0;
                return true;
            }

            _firstChunkDataOffset = 0;
            _numAdditionalChunks = 0;

            if (size > chunkSize) {
                // Each additional chunk adds `chunkSize` bytes of capacity but consumes
                // `sizeof(void*)` bytes in the master chunk for its pointer.
                // The net capacity increase per additional chunk is `chunkSize - sizeof(void*)`.
                const size_t pointerSize = sizeof(void*);
                if (chunkSize <= pointerSize) {
                    logger::error("master chunk is too small to hold chunk pointers and grow.", "PoolBuffer");
                    return false;
                }

                const size_t netCapacityIncrease = chunkSize - pointerSize;
                const size_t additionalSpaceNeeded = size - chunkSize;

                _numAdditionalChunks = (additionalSpaceNeeded + netCapacityIncrease - 1) / netCapacityIncrease;
                _firstChunkDataOffset = _numAdditionalChunks * pointerSize;

                if (_firstChunkDataOffset > chunkSize) {
                    logger::error("master chunk is too small to hold chunk pointers. capacity %zu, required %zu",
                                    "PoolBuffer",
                                    chunkSize,
                                    _firstChunkDataOffset);
                    return false;
                }
            }

            if (!_masterChunk && !(_masterChunk = _allocator.allocate()))
            {
                logger::error("failed to allocate master chunk.", "PoolBuffer");
                return false;
            }

            // Set zero all pointers to allow free() all in case of failure
            std::memset(_masterChunk, 0, _firstChunkDataOffset);

            void** additionalChunkPointers = reinterpret_cast<void**>(_masterChunk);
            for (size_t i = 0; i < _numAdditionalChunks; i++) {
                additionalChunkPointers[i] = _allocator.allocate();
                if (!additionalChunkPointers[i]) {
                    free();
                    logger::error("Failed to allocate %zu-th chunk, from %zu",
                                    "PoolBuffer",
                                    i + 1,
                                    _numAdditionalChunks);
                    return false;
                }
            }
        }
        _size = size;
        return true;
    }

    size_t size() const { return _size; }
    size_t getLength() const { return _size; }
    size_t capacity() const
    {
        assert(_numAdditionalChunks > 0 || 0 == _firstChunkDataOffset);
        if (!_masterChunk) {
            return 0;
        }
        return _allocator.getElementSize() * (_numAdditionalChunks + 1) - _firstChunkDataOffset;
    }
    bool empty() const { return _size == 0 || _masterChunk == nullptr; }
    size_t getChunkCount() const { return _numAdditionalChunks + 1; }
    bool isMultiChunk() const { return _numAdditionalChunks > 0; }

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

private:
    friend class ::PoolBuffer_move_Test;
    friend class ::PoolBuffer_moveOperator_Test;

    void free()
    {
        if (_masterChunk)
        {
            auto** chunkPointers = reinterpret_cast<void**>(_masterChunk);
            for (size_t i = 0; i < _numAdditionalChunks; ++i)
            {
                _allocator.free(chunkPointers[i]);
            }
            _allocator.free(_masterChunk);
        }
        _masterChunk = nullptr;
        _numAdditionalChunks = 0;
        _size = 0;
        _firstChunkDataOffset = 0;
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

        const auto elementSize = _allocator.getElementSize();
        auto shiftedOffset = offset + _firstChunkDataOffset;

        while (processedBytes < remainingToProcess)
        {
            const size_t chunkIndex = shiftedOffset / elementSize;
            const size_t offsetInChunk = shiftedOffset % elementSize;

            uint8_t* dataPtr = (chunkIndex == 0) ?
                static_cast<uint8_t*>(_masterChunk) + offsetInChunk :
                static_cast<uint8_t*>(reinterpret_cast<void**>(_masterChunk)[chunkIndex - 1]) + offsetInChunk;

            const size_t toProcessInChunk = std::min(remainingToProcess - processedBytes, elementSize - offsetInChunk);

            if (!callback(reinterpret_cast<T*>(dataPtr), toProcessInChunk))
            {
                break;
            }

            processedBytes += toProcessInChunk;
            shiftedOffset += toProcessInChunk;
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
    size_t _numAdditionalChunks;
    size_t _firstChunkDataOffset;
};
} // namespace memory