#pragma once
#include <string>

namespace memory
{
class MemoryFile
{
public:
    MemoryFile(void* start, size_t size);
    MemoryFile(const void* start, size_t size);

    bool isEof() const { return _cursor == _end; }
    bool isGood() const { return _good; }
    size_t write(const void* data, size_t length);
    size_t read(void* target, size_t length);

    void rewind() { _cursor = _start; }

    size_t remaining() const { return _end - _cursor; }

    size_t getPosition() const { return _cursor - _start; }
    void setPosition(size_t p);
    void seek(ssize_t bytes);

    void closeWrite() { _end = _cursor; }

private:
    uint8_t* const _start;
    uint8_t* _end;
    uint8_t* _cursor;
    bool _good;
    bool _readOnly;
};

MemoryFile& operator<<(MemoryFile&, const char* s);
MemoryFile& operator>>(MemoryFile&, char* s);

MemoryFile& operator<<(MemoryFile& f, uint32_t s);
MemoryFile& operator>>(MemoryFile& f, uint32_t& s);
MemoryFile& operator<<(MemoryFile& f, uint16_t s);
MemoryFile& operator>>(MemoryFile& f, uint16_t& s);
MemoryFile& operator<<(MemoryFile& f, uint8_t s);
MemoryFile& operator>>(MemoryFile& f, uint8_t& s);
} // namespace memory
