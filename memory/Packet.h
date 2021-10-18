#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>

namespace memory
{

class Packet
{
public:
    Packet() : _length(0) { _data[0] = 0; }

    static const size_t size = 4096; // TODO, this is not enough for 48kHz 16bit 30ms packets, nor for 2-channel 48kHz.
                                     // Allocator should have multiple packet sizes

    unsigned char* get() { return _data; }
    const unsigned char* get() const { return _data; }

    void setLength(const size_t length)
    {
        assert(length <= size);
        _length = (length > size ? size : length);
    }

    size_t getLength() const { return _length; }

    void copyTo(Packet& dst)
    {
        std::memcpy(dst.get(), get(), getLength());
        dst.setLength(getLength());
    }

    void append(void* data, size_t length)
    {
        if (length + _length <= size)
        {
            std::memcpy(_data + _length, data, length);
            _length += length;
        }
    }

private:
    unsigned char _data[size];
    size_t _length;
};

} // namespace memory
