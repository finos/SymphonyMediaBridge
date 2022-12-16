#!/bin/bash
build_type="Release"
if [ $# -eq 1 ]; then
    build_type=$1
fi
echo "create make file for $build_type"
source scl_source enable devtoolset-9

cmake -DENABLE_LEGACY_API=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -D_CMAKE_TOOLCHAIN_PREFIX=llvm- -DCMAKE_BUILD_TYPE=$build_type -G "Unix Makefiles" ../../
