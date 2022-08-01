#!/bin/bash
# Need bash for shopt

shopt -s globstar dotglob

for i in **/CMakeLists.txt; do
  echo "cmake-format $i"
  cmake-format -i "$i" || exit 1
done

for i in {**/*.cpp,**/*.h}; do
  echo "clang-format $i... "
  clang-format-14 -style=file "$i" -i || exit 1
done

exit 0
