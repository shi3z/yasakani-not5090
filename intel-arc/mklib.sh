#!/bin/bash
# Build the drop-in engine library lib/libyskn.so   (./mklib.sh [output file]: build a private copy without touching the shared one)
source /opt/intel/oneapi/setvars.sh >/dev/null 2>&1
cd "$(dirname "$0")" && mkdir -p lib && exec icpx -fsycl -O2 -std=c++20 -ffp-contract=off -fno-fast-math \
  -Xsycl-target-backend "-cl-fp32-correctly-rounded-divide-sqrt -ffp-contract=off" \
  -I "$(cd "$(dirname "$0")" && pwd)/../third_party/nlohmann" -shared -fPIC -o "${1:-lib/libyskn.so}" src/api.cpp
