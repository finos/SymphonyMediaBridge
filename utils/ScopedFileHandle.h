#pragma once

#include <cstdio>

namespace utils
{

class ScopedFileHandle
{
public:
    explicit ScopedFileHandle(FILE* file) : _file(file) {}

    ~ScopedFileHandle()
    {
        if (_file)
        {
            fclose(_file);
        }
    }

    FILE* get() { return _file; }

    operator bool() const { return _file != nullptr; }

private:
    FILE* _file;
};

} // namespace utils
