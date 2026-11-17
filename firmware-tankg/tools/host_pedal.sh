#!/bin/sh
# Builds build/host_pedal: the complete firmware logic as a host process (see tests/host_pedal.cpp). Used by PulseDeck's firmware_e2e test.
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$HERE/build"
g++ -O2 -std=c++17 -Wall -I "$HERE/src" -I "$HERE/../esp32-mock/firmware-core/include" "$HERE/tests/host_pedal.cpp" "$HERE"/../esp32-mock/firmware-core/src/*.cpp -o "$HERE/build/host_pedal"
echo "built $HERE/build/host_pedal"
