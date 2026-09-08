#!/bin/sh
set -eu
mkdir -p .build
${CXX:-c++} -std=c++17 -O2 firmware/noise_monitor/dsp.cpp firmware/tests/dsp_test.cpp -o .build/dsp_test
.build/dsp_test
