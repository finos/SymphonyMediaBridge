#!/bin/bash
build_type="Release"
if [ $# -eq 1 ]; then
    build_type=$1
fi
echo "create make file for $build_type"
source scl_source enable llvm-toolset-7

cmake -DENABLE_LEGACY_API=ON -DCMAKE_C_COMPILER=/opt/rh/llvm-toolset-7/root/usr/bin/clang -DCMAKE_CXX_COMPILER=/opt/rh/llvm-toolset-7/root/usr/bin/clang++ -D_CMAKE_TOOLCHAIN_PREFIX=llvm- -DCMAKE_BUILD_TYPE=$build_type -G "Unix Makefiles" ../../
