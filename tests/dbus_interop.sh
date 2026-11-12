#!/usr/bin/env bash
# The kernel's D-Bus (src/DBus) against the real client libraries: tests/dbus/DBusHostServer.cpp serves a session bus and a system bus on the host's
# AF_UNIX sockets, and the host's own tools are pointed at them: libdbus (dbus-send, dbus-monitor, tests/dbus/fdclient.c), sd-bus (busctl) and GDBus
# (gdbus, notify-send through libnotify). Names, the queue, errors, introspection, credentials, monitoring, signals with match rules, descriptors passed
# with SCM_RIGHTS, and the kernel's Notifications service.
# Usage: tests/dbus_interop.sh     (needs dbus-send, busctl, gdbus and libdbus' headers; skipped without them)
set -euo pipefail
cd "$(dirname "$0")/.."
for tool in dbus-send dbus-monitor busctl gdbus pkg-config; do
  command -v "$tool" >/dev/null || { echo "skipped (no $tool)"; exit 0; }
done
pkg-config --exists dbus-1 || { echo "skipped (no libdbus headers)"; exit 0; }
work=${TMPDIR:-/tmp}/tcos-dbusinterop
# (sun_path holds 108 bytes: the sockets get a short folder of their own)
sock=$(mktemp -d /tmp/tcos-dbus.XXXXXX)
mkdir -p "$work/obj"
cleanup() { kill "${session_pid:-}" "${system_pid:-}" 2>/dev/null || true; rm -rf "$sock"; }
trap cleanup EXIT
sources=(tests/dbus/DBusHostServer.cpp src/DBus/Auth.cpp src/DBus/Bus.cpp src/DBus/BusDriver.cpp src/DBus/Match.cpp src/DBus/Message.cpp src/DBus/Notifications.cpp
         src/DBus/Wire.cpp src/Kernel/Sync.cpp tests/KernelStubs.cpp)
printf '%s\n' "${sources[@]}" | xargs -P "$(nproc)" -I{} sh -c 'g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src -w -c "{}" -o "'"$work"'/obj/$(echo "{}" | tr / _).o"'
g++ -fsanitize=address,undefined "$work"/obj/*.o -o "$work/dbushost"
gcc -O1 -g -Wall tests/dbus/fdclient.c $(pkg-config --cflags --libs dbus-1) -o "$work/fdclient"

fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
"$work/dbushost" "$sock/session" session 120 > "$work/session.out" 2> "$work/session.err" & session_pid=$!
"$work/dbushost" "$sock/system" system 120 > "$work/system.out" 2> "$work/system.err" & system_pid=$!
for _ in $(seq 50); do [ -S "$sock/session" ] && [ -S "$sock/system" ] && break; sleep 0.1; done
export DBUS_SESSION_BUS_ADDRESS=unix:path=$sock/session DBUS_SYSTEM_BUS_ADDRESS=unix:path=$sock/system
T="timeout 5"

# libdbus
check "dbus-send ListNames" "$T dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep -q 'string \"org.freedesktop.Notifications\"'"
check "dbus-send GetId" "$T dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.GetId | grep -q 0123456789abcdef0123456789abcdef"
# (an error makes the tool fail: its output is looked at, not its exit status)
check "dbus-send to a name nobody has" "{ $T dbus-send --session --print-reply --dest=org.tcos.Nobody /x org.tcos.X.Y 2>&1 || true; } | grep -q 'org.freedesktop.DBus.Error.ServiceUnknown'"
check "dbus-send on the system bus" "$T dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep -q 'string \"org.freedesktop.DBus\"'"
check "system bus refuses UpdateActivationEnvironment" "{ $T dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.UpdateActivationEnvironment dict:string:string:A,B 2>&1 || true; } | grep -q AccessDenied"
check "a well-known name of nobody (GetNameOwner)" "{ $T gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.GetNameOwner org.tcos.Nobody 2>&1 || true; } | grep -q NameHasNoOwner"
check "Peer.GetMachineId" "$T dbus-send --session --print-reply --dest=org.freedesktop.DBus / org.freedesktop.DBus.Peer.GetMachineId | grep -q fedcba9876543210fedcba9876543210"
check "a descriptor through the bus, and a name handed over (libdbus)" "$T $work/fdclient > $work/fd.out 2> $work/fd.err && grep -q 'fd: through-the-bus' $work/fd.out && grep -q 'queue: ok' $work/fd.out"
# dbus-monitor (BecomeMonitor) sees a signal and a call
($T dbus-monitor --session > "$work/monitor.txt" 2>&1 || true) & monitor=$!
sleep 0.5
$T dbus-send --session --type=signal /org/tcos/Sig org.tcos.Test.Hello string:"from dbus-send"
$T dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.GetId > /dev/null
sleep 0.5
kill $monitor 2>/dev/null || true
wait $monitor 2>/dev/null || true
check "dbus-monitor sees a signal" "grep -q 'member=Hello' $work/monitor.txt && grep -q 'from dbus-send' $work/monitor.txt"
check "dbus-monitor sees a call and its reply" "grep -q 'member=GetId' $work/monitor.txt && grep -q 'method return' $work/monitor.txt"

# sd-bus
check "busctl list" "$T busctl --user list --no-pager | grep -q org.freedesktop.Notifications"
check "busctl call RequestName" "$T busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus RequestName su org.tcos.Busctl 0 | grep -q '^u 1$'"
check "busctl GetConnectionUnixUser" "$T busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus GetConnectionUnixUser s org.freedesktop.DBus | grep -q '^u 0$'"
check "busctl introspect the bus" "$T busctl --user introspect org.freedesktop.DBus /org/freedesktop/DBus --no-pager | grep -q '.RequestName'"
check "busctl tree of Notifications" "$T busctl --user tree org.freedesktop.Notifications --no-pager | grep -q /org/freedesktop/Notifications"
check "busctl get-property Features" "$T busctl --user get-property org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus Features | grep -q HeaderFiltering"
check "busctl on the system bus" "$T busctl --system call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus GetId | grep -q 0123456789abcdef"

# GDBus
check "gdbus GetServerInformation" "$T gdbus call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications --method org.freedesktop.Notifications.GetServerInformation | grep -q TheoryCraftOS"
check "gdbus introspect" "$T gdbus introspect --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus | grep -q 'BecomeMonitor'"
check "gdbus NameHasOwner" "$T gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.NameHasOwner org.freedesktop.Notifications | grep -q true"
check "gdbus Notify" "$T gdbus call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications --method org.freedesktop.Notifications.Notify gdbus 0 '' 'From gdbus' 'a body' '[]' '{}' 3000 | grep -q '(uint32 '"
if command -v notify-send >/dev/null; then
  check "notify-send" "$T notify-send 'From notify-send' 'libnotify over the kernel bus'"
fi
# a signal with a match rule (gdbus monitor subscribes with AddMatch)
($T gdbus monitor --session --dest org.freedesktop.Notifications > "$work/gmon.txt" 2>&1 || true) & gmon=$!
sleep 0.5
id=$($T gdbus call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications --method org.freedesktop.Notifications.Notify g 0 '' 'to close' '' '[]' '{}' 0 | sed 's/.*uint32 \([0-9]*\).*/\1/')
$T gdbus call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications --method org.freedesktop.Notifications.CloseNotification "$id" > /dev/null
sleep 0.5
kill $gmon 2>/dev/null || true
wait $gmon 2>/dev/null || true
check "NotificationClosed reaches a gdbus monitor" "grep -q 'NotificationClosed' $work/gmon.txt"
sleep 0.3
check "the service showed the notifications" "grep -q 'notify: gdbus | From gdbus | a body | 3000' $work/session.out && grep -q 'notify-close' $work/session.out"
if command -v notify-send >/dev/null; then
  check "the service showed notify-send's" "grep -q 'From notify-send | libnotify over the kernel bus' $work/session.out"
fi
check "no sanitizer report" "! grep -q 'ERROR: AddressSanitizer\|runtime error' $work/session.err $work/system.err"
[ $fail -eq 0 ] && echo "dbus interop: all passed" || echo "dbus interop: FAILED"
exit $fail
