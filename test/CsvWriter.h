#pragma once
#include <cstdarg>
#include <cstdio>
#include <cstring>

class CsvWriter
{
public:
    CsvWriter(const char* fileName);
    ~CsvWriter();

    inline __attribute__((format(printf, 2, 3))) void writeLine(const char* format, ...)
    {
        if (!_logFile)
        {
            return;
        }
        va_list arglist;
        va_start(arglist, format);
        ::vfprintf(_logFile, format, arglist);
        fprintf(_logFile, "\n");
        va_end(arglist);
    }

private:
    FILE* _logFile;
};
