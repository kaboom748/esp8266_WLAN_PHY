#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
g++ -std=c++11 -Wall -Wextra -Itests/host -Isrc tests/host/reliable_test.cpp src/RHGenericDriver.cpp src/RHDatagram.cpp src/RHReliableDatagram.cpp -o "$build/reliable_test"
g++ -std=c++11 -Wall -Wextra -Isrc tests/host/codec_test.cpp -o "$build/codec_test"
"$build/codec_test"
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/host/fsk4_test.cpp -o "$build/fsk4_test"
"$build/fsk4_test"
"$build/reliable_test"
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/host/rate_test.cpp -o "$build/rate_test"
"$build/rate_test"
g++ -std=c++11 -Wall -Wextra -fsanitize=undefined -Isrc tests/host/phase_test.cpp -o "$build/phase_test"
"$build/phase_test"
g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -Itests/host -Isrc tests/host/tdm_test.cpp src/RHGenericDriver.cpp src/ESP8266TDM.cpp -o "$build/tdm_test"
"$build/tdm_test"
g++ -std=c++11 -Wall -Wextra -fsanitize=undefined -Itests/host -Isrc tests/host/log_test.cpp -o "$build/log_test"
"$build/log_test"
