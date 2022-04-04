#!/bin/bash

if [ $# -eq 0 ]; then
  pr "Usage generate_make.sh [Debug|Release|DCheck|TCheck|LCheck]"
  exit 1
fi

rm -rf CMakeCache.txt CMakeFiles googletest-* Makefile
cmake -DCMAKE_BUILD_TYPE=$1 -DENABLE_LEGACY_API=1 -G "Unix Makefiles" .
