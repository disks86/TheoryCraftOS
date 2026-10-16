#!/usr/bin/env bash
# Builds and runs the host-side unit tests (the freestanding code compiled for
# the development machine, with the address and undefined-behaviour sanitizers).
# Usage: tests/run_host_tests.sh [test-name...]   (default: all)
set -euo pipefail
cd "$(dirname "$0")/.."
out=${TMPDIR:-/tmp}/tcos-host-tests
mkdir -p "$out"
cxx=${CXX:-g++}
flags="-std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -I src -Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -Wno-unused-variable"

build_and_run() {
  local name=$1; shift
  echo "== $name"
  $cxx $flags "$@" -o "$out/$name"
  "$out/$name"
}

want() { [ $# -eq 0 ] && return 0; for t in "${WANTED[@]}"; do [ "$t" = "$1" ] && return 0; done; return 1; }
WANTED=("$@")
if [ ${#WANTED[@]} -eq 0 ]; then WANTED=(ArrayViewTest StringTest ParserFuzzTest NetworkTest); fi
for t in "${WANTED[@]}"; do
  case $t in
    ArrayViewTest) build_and_run ArrayViewTest tests/ArrayViewTest.cpp ;;
    StringTest) build_and_run StringTest tests/StringTest.cpp ;;
    ParserFuzzTest) build_and_run ParserFuzzTest tests/ParserFuzzTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                      src/Hardware/Bus/Usb/Hid/HidReportParser.cpp src/Hardware/Bus/Usb/Hid/UsbHidInterface.cpp ;;
    NetworkTest) build_and_run NetworkTest tests/NetworkTest.cpp tests/KernelStubs.cpp src/Kernel/Sync.cpp \
                   src/Network/Checksum.cpp src/Network/NetworkStack.cpp src/Network/NetworkStackSet.cpp \
                   src/Network/Udp/*.cpp src/Network/Tcp/*.cpp src/Network/Dhcp/*.cpp src/Network/Dns/*.cpp ;;
    *) echo "unknown test $t"; exit 2 ;;
  esac
done
