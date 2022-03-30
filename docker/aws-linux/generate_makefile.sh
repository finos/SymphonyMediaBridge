#!/bin/bash
build_type="Release"
if [ $# -eq 1 ]; then
    build_type=$1
fi
echo "create make file for $build_type"
export CC=/usr/local/bin/clang
export CXX=/usr/local/bin/clang++

cmake -DENABLE_LEGACY_API=ON -D_CMAKE_TOOLCHAIN_PREFIX=llvm- -DCMAKE_C_COMPILER=/usr/local/bin/clang -DCMAKE_CXX_COMPILER=/usr/local/bin/clang++ -DCMAKE_LINKER=/usr/local/bin/lld  -DCMAKE_BUILD_TYPE=$build_type -G "Unix Makefiles" ../../
