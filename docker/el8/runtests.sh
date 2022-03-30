#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

function pr() {
  echo -e ${GREEN}$1${NC}
}

function generate_coverage_report() {
  if ! find . -iname "*.gcda" | grep -q "."; then
    echo No coverage data found in "$PWD"
    return 0
  fi

  if ! command -v lcov || ! command -v genhtml; then
    echo 'Failed to generate code coverage report (lcov and genhtml commands not found)'
    return 1
  fi

  # Convert coverage data from LLVM to lcov format, see http://logan.tw/posts/2015/04/28/check-code-coverage-with-clang-and-lcov/
  # (With a more recent LLVM, the same can be achieved with `llvm-cov export lcov`)
  echo '#!/bin/bash' > llvm-gcov.sh
  echo 'llvm-cov gcov "$@"' >> llvm-gcov.sh
  chmod +x llvm-gcov.sh

  if ! lcov --capture \
    --directory . \
    --gcov-tool "$(pwd)/llvm-gcov.sh" \
    --output-file coverage.info
  then
    echo "ERROR: lcov --capture returned non-zero exit code"
    return 1
  fi
  rm llvm-gcov.sh

  # Exclude external and test source from coverage
  if ! lcov --remove coverage.info \*/googletest-src/\* \*/external/\* \*/test/\* /usr/\* -o coverage.info
  then
    echo "ERROR: lcov --remove returned non-zero exit code"
    return 1
  fi

  if ! genhtml -o coverage coverage.info
  then
    echo "ERROR: genhtml returned non-zero exit code"
  fi
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
export LD_LIBRARY_PATH=../../el8/smb/libs
../../el8/smb/UnitTest --gtest_filter="*.*" --gtest_output=xml:../../el8/smb/test-results.xml
if [ $? != 0 ]; then
    pr "Testing failed"; exit 1
fi
popd || exit 1

pushd el8/smb || exit 1
generate_coverage_report || exit 1
popd
