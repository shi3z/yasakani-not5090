#!/bin/bash
# Build helper: ./build.sh tests/layers_test   (source file tests/layers_test.cpp)
source /opt/intel/oneapi/setvars.sh >/dev/null 2>&1
exec icpx -fsycl -O2 -std=c++20 -ffp-contract=off -fno-fast-math -Xsycl-target-backend "-cl-fp32-correctly-rounded-divide-sqrt -ffp-contract=off" \
  -I "$(cd "$(dirname "$0")" && pwd)/../third_party/nlohmann" -o "$1" "$1.cpp" "${@:2}"
