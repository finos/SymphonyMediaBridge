#include "memory/MemoryFile.h"
#include <algorithm>
#include <cstring>

namespace memory
{
MemoryFile::MemoryFile(void* start, size_t size)
    : _start(reinterpret_cast<uint8_t*>(start)),
      _end(reinterpret_cast<uint8_t*>(_start + size)),
      _cursor(_start),
      _good(true),
      _readOnly(false)
{
}

MemoryFile::MemoryFile(const void* start, size_t size)
    : _start(reinterpret_cast<uint8_t*>(const_cast<void*>(start))),
      _end(reinterpret_cast<uint8_t*>(_start + size)),
      _cursor(_start),
      _good(true),
      _readOnly(true)
{
}

/**
 * Writes the number of bytes or none. Good flag will tell if file is okor operation failed.
 */
size_t MemoryFile::write(const void* data, size_t length)
{
    if (!_good || _readOnly)
    {
        return 0;
    }

    if (_cursor + length <= _end)
    {
        std::memcpy(_cursor, data, length);
        _cursor += length;
        return length;
    }
    else
    {
        _good = false;
    }

    return 0;
}

/**
 * Reads specified number of bytes or none. Good flag will tell if the read was successful.
 */
size_t MemoryFile::read(void* target, size_t length)
{
    if (!_good)
    {
        return 0;
    }

    const auto toRead = std::min(length, static_cast<size_t>(_end - _cursor));
    if (toRead > 0 && remaining() >= toRead)
    {
        std::memcpy(target, _cursor, toRead);
        _cursor += toRead;
        return toRead;
    }
    else
    {
        _good = false;
    }

    return 0;
}

void MemoryFile::setPosition(size_t p)
{
    if (static_cast<size_t>(_end - _start) >= p)
    {
        _cursor = _start + p;
    }
    else
    {
        _cursor = _end;
    }
    _good = true;
}

void MemoryFile::seek(ssize_t bytes)
{
    if (bytes >= 0)
    {
        _cursor += std::min(static_cast<size_t>(bytes), remaining());
    }
    else
    {
        _cursor -= std::min(_cursor - _start, -bytes);
    }
    _good = true;
}

MemoryFile& operator<<(MemoryFile& f, const char* s)
{
    uint16_t len = std::strlen(s);
    if (len + sizeof(uint16_t) > f.remaining())
    {
        f.write(s, len * 2); // will fail and put file !good
        return f;
    }

    f.write(&len, sizeof(len));
    f.write(s, len);
    return f;
}

MemoryFile& readString(MemoryFile& f, char* target, size_t maxLength)
{
    uint16_t len = 0;
    auto currentPos = f.getPosition();
    f.read(&len, sizeof(len));
    if (!f.isGood())
    {
        return f;
    }

    if (f.remaining() < len || len + 1 > maxLength)
    {
        f.setPosition(currentPos);
        f.read(&len, f.remaining() * 2); // casue !good
        return f;
    }

    f.read(target, len);
    target[len] = '\0';
    return f;
}

} // namespace memory
