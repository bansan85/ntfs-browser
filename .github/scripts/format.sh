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

for i in {**/*.cpp,**/*.h}; do
  case "$i" in 3rdparty/*) continue ;; esac
  echo "clang-format $i... "
  clang-format -style=file "$i" -i || exit 1
done

exit 0
