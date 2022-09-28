#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>

namespace memory
{

template <size_t PacketSize>
class FixedPacket
{
public:
    FixedPacket() : endpointIdHash(0), _length(0) { _data[0] = 0; }

    static constexpr size_t size = PacketSize;
    static_assert(PacketSize % 8 == 0, "packet size must be 8B aligned");

    static size_t maxLength() { return PacketSize; }

    unsigned char* get() { return _data; }
    const unsigned char* get() const { return _data; }

    void setLength(const size_t length)
    {
        assert(length <= size);
        _length = (length > size ? size : length);
    }

    size_t getLength() const { return _length; }

    void copyTo(FixedPacket<PacketSize>& dst)
    {
        std::memcpy(dst.get(), get(), getLength());
        dst.setLength(getLength());
        dst.endpointIdHash = endpointIdHash;
    }

    void append(const void* data, size_t length)
    {
        if (length + _length <= size)
        {
            std::memcpy(_data + _length, data, length);
            _length += length;
        }
    }

    void clear() { std::memset(_data, 0, size); }

    size_t endpointIdHash = 0;

private:
    unsigned char _data[size];
    size_t _length;
};

class Packet : public FixedPacket<1504>
{
};

} // namespace memory
