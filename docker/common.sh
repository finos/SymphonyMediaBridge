#!/bin/bash

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
    return 1
  fi
}
