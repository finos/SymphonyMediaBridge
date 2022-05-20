#!/bin/bash
build_type="Release"
if [ $# -eq 1 ]; then
    build_type=$1
fi

echo "create make file for $build_type"

cmake -DENABLE_LEGACY_API=ON -DENABLE_LIBATOMIC=ON -D_CMAKE_TOOLCHAIN_PREFIX=llvm- -DCMAKE_BUILD_TYPE=$build_type -DCMAKE_C_COMPILER=/usr/local/bin/clang -DCMAKE_CXX_COMPILER=/usr/local/bin/clang++ -G "Unix Makefiles" ../../
