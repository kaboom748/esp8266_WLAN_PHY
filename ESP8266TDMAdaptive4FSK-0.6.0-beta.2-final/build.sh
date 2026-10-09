#!/bin/sh
set -eu
cd "$(dirname "$0")"
root="$PWD"
fqbn=esp8266:esp8266:d1_mini:xtal=160,eesz=4M
sh tests/run_host_tests.sh
for role in A B
do
  arduino-cli compile --fqbn "$fqbn" --library "$root" --build-path "$root/build_$role" "$root/examples/TDM_$role"
done
