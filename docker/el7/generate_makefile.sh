#!/bin/bash
build_type="Release"
if [ $# -eq 1 ]; then
    build_type=$1
fi
echo "create make file for $build_type"
source scl_source enable llvm-toolset-7
export CC=/opt/rh/llvm-toolset-7/root/usr/bin/clang
export CXX=/opt/rh/llvm-toolset-7/root/usr/bin/clang++
cmake -DENABLE_LEGACY_API=ON -D_CMAKE_TOOLCHAIN_PREFIX=llvm- -DCMAKE_BUILD_TYPE=$build_type -G "Unix Makefiles" ../../
