#!/usr/bin/env bash

git diff --name-only master HEAD | grep -v external/ | grep -e '\.[h]$' -e '\.cpp$' | xargs clang-format -i
