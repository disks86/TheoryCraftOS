#!/usr/bin/env bash
# The kernel's D-Bus (src/DBus) inside the OS: the host's real, dynamically linked D-Bus clients run on the CPU emulator (tests/EmulationRun.cpp built with
# -DTCOS_EMU_DBUS) through the OS's own Linux system calls, glibc's ld.so and the system's AF_UNIX sockets, against the buses the kernel serves at
# /run/dbus/system_bus_socket and /run/user/0/bus (DBUS_SESSION_BUS_ADDRESS of every Linux program):
#   dbus-send (libdbus) - ListNames on the session bus, GetId on the system bus, GetServerInformation of the kernel's Notifications service;
#   fdclient (tests/dbus/fdclient.c, libdbus) - two connections in one process: a pipe's read end passed with UNIX_FD and read on the other side, then
#     a name handed over from one to the other with NameOwnerChanged and NameAcquired;
#   gdbus (GDBus), when the host has it - a Notify call, shown by the service.
# Usage: tests/dbus_emulated.sh     (x86-64 only: the host's binaries; needs dbus-send and libdbus' headers)
set -euo pipefail
cd "$(dirname "$0")/.."
for tool in dbus-send pkg-config ldd readelf; do command -v "$tool" >/dev/null || { echo "skipped (no $tool)"; exit 0; }; done
pkg-config --exists dbus-1 || { echo "skipped (no libdbus headers)"; exit 0; }
# (one work folder per checkout: the objects of another worktree must not be taken for this one's)
work=${TMPDIR:-/tmp}/tcos-dbusemu-$(printf %s "$PWD" | cksum | cut -d" " -f1)
mkdir -p "$work/obj"
sources=(tests/EmulationRun.cpp src/Emulation/*.cpp src/Process/*.cpp src/Crypto/*.cpp src/Executable/*.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp
         src/Kernel/Sync.cpp tests/KernelStubs.cpp $(ls src/DBus/*.cpp | grep -v ProgramActivator))
flags="-std=c++20 -O1 -pthread -I src -w -DTCOS_EMU_DBUS"
objs=()
jobs=()
for f in "${sources[@]}"; do
  o="$work/obj/$(echo "$f" | tr '/' '_').o"
  objs+=("$o")
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ] || [ -n "$(find src -name '*.hpp' -newer "$o" -print -quit)" ]; then jobs+=("$f"); fi
done
if [ ${#jobs[@]} -gt 0 ]; then
  printf '%s\n' "${jobs[@]}" | xargs -P "$(nproc)" -I{} sh -c 'o="'"$work"'/obj/$(echo "{}" | tr / _).o"; g++ '"$flags"' -c "{}" -o "$o" || { rm -f "$o"; exit 255; }'
fi
g++ -pthread "${objs[@]}" -o "$work/emurun"
gcc -O1 -g tests/dbus/fdclient.c $(pkg-config --cflags --libs dbus-1) -o "$work/fdclient"
gcc -O1 -g tests/dbus/echoservice.c $(pkg-config --cflags --libs dbus-1) -o "$work/echoservice"
# the service started on demand inside the OS: its .service file in a folder of the Linux tree
mkdir -p "$work/services"
printf '[D-BUS Service]\nName=org.tcos.Echo\nExec=%s org.tcos.Echo 3\n' "$work/echoservice" > "$work/services/org.tcos.Echo.service"

# the files a dynamically linked program needs: its interpreter and every library ldd finds
files_for() {
  { ldd "$1" | awk '/=>/ { if ($3 ~ /^\//) print $3 } /^\t\// { print $1 }'
    readelf -l "$1" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p'; } | sort -u | paste -sd:
}
fail=0
run() {      # run <name> <program> <arguments> <what the output must hold>...   (EXTRA_FILES: more files for the Linux tree, colon separated)
  local name=$1 program=$2 args=$3; shift 3
  local files; files="$(files_for "$program")${EXTRA_FILES:+:$EXTRA_FILES}"
  TCOS_EMU_ARGS="$args" TCOS_EMU_LINUX_FILES="$files" "$work/emurun" "$program" > "$work/$name.out" 2>&1 || true
  local ok=1
  grep -q "\[exit=0 faulted=0" "$work/$name.out" || ok=0
  for want in "$@"; do grep -q -- "$want" "$work/$name.out" || ok=0; done
  if [ $ok = 1 ]; then echo "ok   $name"; else echo "FAIL $name (see $work/$name.out)"; tail -5 "$work/$name.out"; fail=1; fi
}
dbus_send=$(command -v dbus-send)
run "dbus-send ListNames (session bus)" "$dbus_send" "--session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames" \
    'string "org.freedesktop.Notifications"' 'string "org.freedesktop.DBus"'
run "dbus-send GetId (system bus)" "$dbus_send" "--system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.GetId" \
    '0123456789abcdef0123456789abcdef'
# (dbus-send cannot make Notify's a{sv} hints: it asks the Notifications service something else)
run "dbus-send GetServerInformation" "$dbus_send" "--session --print-reply --dest=org.freedesktop.Notifications /org/freedesktop/Notifications org.freedesktop.Notifications.GetServerInformation" \
    'string "TheoryCraftOS"' 'string "1.2"'
run "fdclient (UNIX_FD, name queue)" "$work/fdclient" "" 'fd: through-the-bus' 'queue: ok'
if command -v gdbus >/dev/null; then
  run "gdbus Notify" "$(command -v gdbus)" "call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications --method org.freedesktop.Notifications.Notify gdbus 0 x Hi there [] {} 2000" \
      '(uint32 1,)' 'dbus notify: gdbus | Hi | there | 2000'
fi
# a call starts its service: dbus-send asks org.tcos.Echo, the bus starts echoservice (a second emulated program, through its .service file) and the
# answer comes back from it
EXTRA_FILES="$work/echoservice:$(files_for "$work/echoservice"):$work/services/org.tcos.Echo.service" TCOS_EMU_DBUS_SERVICES="$work/services" \
  run "dbus-send starts a service on demand" "$dbus_send" "--session --print-reply --reply-timeout=60000 --dest=org.tcos.Echo /org/tcos/Echo org.tcos.Echo.Echo string:activated-inside-the-OS" \
    'string "activated-inside-the-OS"' 'dbus activate: org.tcos.Echo' 'dbus: 1 services started'
[ $fail -eq 0 ] && echo "dbus emulated: all passed" || echo "dbus emulated: FAILED"
exit $fail
