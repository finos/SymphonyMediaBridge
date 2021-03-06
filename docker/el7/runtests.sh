#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
source "$SCRIPT_DIR/../common.sh" || exit 1

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

function pr() {
  echo -e ${GREEN}$1${NC}
}

pushd tools/testfiles

#source scl_source enable devtoolset-7
source scl_source enable llvm-toolset-7

export CC=/opt/rh/llvm-toolset-7/root/usr/bin/clang
export CXX=/opt/rh/llvm-toolset-7/root/usr/bin/clang++

ulimit -c unlimited
export ASAN_OPTIONS=disable_coredump=0:unmap_shadow_on_exit=1:abort_on_error=1:detect_leaks=0:sleep_before_dying=15
export ASAN_SYMBOLIZER_PATH=/opt/rh/llvm-toolset-7/root/usr/bin/llvm-symbolizer
export MSAN_SYMBOLIZER_PATH=/opt/rh/llvm-toolset-7/root/usr/bin/llvm-symbolizer
export LSAN_OPTIONS=log_threads=1

pr "Running tests"
export LD_LIBRARY_PATH=../../el7/smb/libs
../../el7/smb/UnitTest --gtest_filter="*.*" --gtest_output=xml:../../el7/smb/test-results.xml
if [ $? != 0 ]; then
    pr "Testing failed"; exit 1
fi
popd || exit 1

pushd el7/smb || exit 1
generate_coverage_report || exit 1
popd
