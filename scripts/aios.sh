#!/usr/bin/env bash
# This is the build wrapper referenced by README.md. In this repo it is a
# placeholder; the real build is driven by CMake (see CMakeLists.txt).
echo "aios.sh: run the build via cmake in the build/ directory" >&2
exec cmake --build build
