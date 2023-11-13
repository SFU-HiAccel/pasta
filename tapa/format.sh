#!/bin/bash
# Requires: clang-format, yapf, isort, shfmt

set -e

cd "${0%/*}"

which clang-format
find \( \
  -path './backend/clang' -or \
  -path './backend/python/tapa/assets/clang' -or \
  -path './regression' -or \
  -path '*/build' \
  \) -prune -or \( \
  -iname '*.h' -or -iname '*.cpp' \
  \) -print0 | xargs --null clang-format -i --verbose

which yapf
which isort
find \( \
  -path './backend/python/tapa/verilog/axi_xbar.py' -or \
  -path './regression' -or \
  -path '*/build' \
  \) -prune -or \( \
  -iname '*.py' \
  \) \
  -execdir yapf --in-place --verbose '{}' ';' \
  -execdir isort '{}' ';'

which shfmt
find \( \
  -path './regression' -or \
  -path '*/build' \
  \) -prune -or \( \
  -iname '*.sh' \
  \) -print0 | xargs --null shfmt --write --indent=2
