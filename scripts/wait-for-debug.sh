#!/usr/bin/env bash
# Blocks until the OS is parked in its boot-time debugger wait. Attaching
# before that would plant breakpoints in memory the firmware hasn't loaded yet.
cd "$(dirname "$0")/.."
BUILD=${BUILD_DIR:-$(for d in build cmake-build-debug; do [ -f "$d/CMakeCache.txt" ] && echo "$d" && break; done)}
LOG=$BUILD/debugcon.log
for _ in $(seq 1 300); do
  grep -qs "Waiting for debugger" "$LOG" && exit 0
  sleep 0.1
done
echo "timed out waiting for the OS (is QEMU running and TCOS_DEBUG_WAIT_SECS > 0?)" >&2
exit 1
