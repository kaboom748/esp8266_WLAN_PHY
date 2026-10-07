#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
g++ -std=c++11 -Wall -Wextra -Itests/host -Isrc tests/host/reliable_test.cpp src/RHGenericDriver.cpp src/RHDatagram.cpp src/RHReliableDatagram.cpp -o "$build/reliable_test"
g++ -std=c++11 -Wall -Wextra -Isrc tests/host/codec_test.cpp -o "$build/codec_test"
"$build/codec_test"
"$build/reliable_test"
g++ -std=c++11 -Wall -Wextra -Isrc tests/host/rate_test.cpp -o "$build/rate_test"
"$build/rate_test"
g++ -std=c++11 -Wall -Wextra -Itests/host -Isrc tests/host/tdm_test.cpp src/RHGenericDriver.cpp src/ESP8266TDM.cpp -o "$build/tdm_test"
"$build/tdm_test"
