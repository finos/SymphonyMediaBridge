#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

function pr() {
  echo -e ${GREEN}$1${NC}
}

pushd tools/testfiles

export CC=clang
export CXX=clang++

ulimit -c unlimited
export ASAN_OPTIONS=disable_coredump=0:unmap_shadow_on_exit=1:abort_on_error=1:detect_leaks=0:sleep_before_dying=15
export ASAN_SYMBOLIZER_PATH=llvm-symbolizer
export MSAN_SYMBOLIZER_PATH=llvm-symbolizer
export LSAN_OPTIONS=log_threads=1

pr "Running tests"
export LD_LIBRARY_PATH=../../aws-linux/smb/libs
../../aws-linux/smb/UnitTest --gtest_filter="*.*" --gtest_output=xml:../../aws-linux/smb/test-results.xml
if [ $? != 0 ]; then
    pr "Testing failed"; exit 1
fi

pr "Running tests2"
../../aws-linux/smb/UnitTest2 --gtest_filter="*.*" --gtest_output=xml:../../aws-linux/smb/test-results2.xml
if [ $? != 0 ]; then
    pr "Testing failed"; exit 1
fi

popd || exit 1

pushd aws-linux/smb || exit 1
popd
