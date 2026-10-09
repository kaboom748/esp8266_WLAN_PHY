#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$root"
sh tests/run_host_tests.sh
for role in A B; do
  arduino-cli compile --fqbn 'esp8266:esp8266:d1_mini:xtal=160,eesz=4M' --library "$root" --build-path "$root/build_$role" "$root/examples/Modem_$role"
done
