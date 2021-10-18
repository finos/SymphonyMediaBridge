#include "CsvWriter.h"

CsvWriter::CsvWriter(const char* fileName)
{
    _logFile = ::fopen(fileName, "w");
}

CsvWriter::~CsvWriter()
{
    if (_logFile)
    {
        ::fclose(_logFile);
    }
}
