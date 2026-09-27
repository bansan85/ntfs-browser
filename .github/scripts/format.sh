#!/bin/bash
# Need bash for shopt

shopt -s globstar dotglob

# Vendored submodules under 3rdparty/ carry their own style; reformatting
# them would leave the tree dirty and fail the format workflow.
for i in **/CMakeLists.txt; do
  case "$i" in 3rdparty/*) continue ;; esac
  echo "cmake-format $i"
  cmake-format -i "$i" || exit 1
done

find . \( -path ./3rdparty -o -path ./.claude -o -path ./build \) -prune -o \( -name '*.cpp' -o -name '*.h' \) -exec clang-format -style=file -i {} +

exit 0
