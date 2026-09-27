#!/bin/bash

find . \( -path ./3rdparty -o -path ./.claude -o -path ./build \) -prune -o -name 'CMakeLists.txt' -exec gersemi -i {} +

find . \( -path ./3rdparty -o -path ./.claude -o -path ./build \) -prune -o \( -name '*.cpp' -o -name '*.h' \) -exec clang-format -style=file -i {} +

exit 0
