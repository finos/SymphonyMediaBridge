#!/bin/bash
build_type="Release"
if [ $# -eq 1 ]; then
    build_type=$1
fi
echo "create make file for $build_type"
export CC=clang
export CXX=clang++
cmake -DENABLE_LEGACY_API=ON -D_CMAKE_TOOLCHAIN_PREFIX=llvm- -DCMAKE_BUILD_TYPE=$build_type -G "Unix Makefiles" ../../
