#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p validation
g++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc src/ModemLink.cpp src/AirModem.cpp tests/test_transparent.cpp -o validation/test_transparent
./validation/test_transparent
g++ -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc src/ModemSignal.cpp tests/test_signal.cpp -o validation/test_signal
./validation/test_signal
python3 tests/test_flow_serial.py
