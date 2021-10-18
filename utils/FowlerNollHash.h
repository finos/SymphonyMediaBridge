#pragma once
#include <cstddef>
#include <cstdint>
namespace utils
{
size_t FowlerNollVoHash(const void* data, size_t length);
}