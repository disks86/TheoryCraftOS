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
cleanup() { kill "${session_pid:-}" "${system_pid:-}" "${act_pid:-}" 2>/dev/null || true; rm -rf "$sock"; }
trap cleanup EXIT
sources=(tests/dbus/DBusHostServer.cpp src/DBus/Activation.cpp src/DBus/Auth.cpp src/DBus/Bus.cpp src/DBus/BusDriver.cpp src/DBus/Match.cpp src/DBus/Message.cpp src/DBus/Notifications.cpp
         src/DBus/Wire.cpp src/Kernel/Sync.cpp tests/KernelStubs.cpp src/FileSystem/FileSystemTypes.cpp src/FileSystem/Ram/*.cpp)
printf '%s\n' "${sources[@]}" | xargs -P "$(nproc)" -I{} sh -c 'g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I src -w -c "{}" -o "'"$work"'/obj/$(echo "{}" | tr / _).o"'
g++ -fsanitize=address,undefined "$work"/obj/*.o -o "$work/dbushost"
gcc -O1 -g -Wall tests/dbus/fdclient.c $(pkg-config --cflags --libs dbus-1) -o "$work/fdclient"
gcc -O1 -g -Wall tests/dbus/echoservice.c $(pkg-config --cflags --libs dbus-1) -o "$work/echoservice"
# services to start on demand: the echo service under two names, one that exits at once, one whose program is missing, one that never takes its name
mkdir -p "$work/services"
rm -f "$work/services"/*.service
service() { printf '[D-BUS Service]\nName=%s\nExec=%s\n' "$1" "$2" > "$work/services/$1.service"; }
service org.tcos.Echo "$work/echoservice org.tcos.Echo 20"
service org.tcos.Echo2 "$work/echoservice 'org.tcos.Echo2' 20"
service org.tcos.False "/bin/false"
service org.tcos.Missing "$work/no-such-program"
service org.tcos.Slow "/bin/sleep 31.5"
service org.tcos.Env "$work/echoservice org.tcos.Env 20"
# without systemd: the unit's ExecStart runs (found by its Alias=), not the Exec=/bin/false that distributions write; one with no unit runs its Exec
mkdir -p "$work/units"
rm -f "$work/units"/*.service
printf '[D-BUS Service]\nName=org.tcos.Unit\nExec=/bin/false\nUser=root\nSystemdService=dbus-org.tcos.Unit.service\n' > "$work/services/org.tcos.Unit.service"
printf '[D-BUS Service]\nName=org.tcos.NoUnit\nExec=/bin/false\nSystemdService=dbus-org.tcos.NoUnit.service\n' > "$work/services/org.tcos.NoUnit.service"
printf '[Unit]\nDescription=test\n[Service]\nType=dbus\nBusName=org.tcos.Unit\nExecStart=%s org.tcos.Unit 20\nEnvironment=FROM_UNIT=unit-env\n[Install]\nAlias=dbus-org.tcos.Unit.service\n' \
  "$work/echoservice" > "$work/units/tcos-unitd.service"

fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }
"$work/dbushost" "$sock/session" session 120 > "$work/session.out" 2> "$work/session.err" & session_pid=$!
"$work/dbushost" "$sock/system" system 120 > "$work/system.out" 2> "$work/system.err" & system_pid=$!
# a third bus starts services on demand, with short timeouts: 1.5 s for a reply, 2 s for a service to take its name
"$work/dbushost" "$sock/act" session 120 --services "$work/services" --units "$work/units" --reply-timeout-ms 1500 --activation-timeout-ms 2000 > "$work/act.out" 2> "$work/act.err" & act_pid=$!
for _ in $(seq 50); do [ -S "$sock/session" ] && [ -S "$sock/system" ] && [ -S "$sock/act" ] && break; sleep 0.1; done
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
# activation (the third bus)
A="env DBUS_SESSION_BUS_ADDRESS=unix:path=$sock/act timeout 10"
check "a call starts its service (dbus-send)" "$A dbus-send --session --print-reply --dest=org.tcos.Echo /org/tcos/Echo org.tcos.Echo.Echo string:started-on-demand | grep -q 'string \"started-on-demand\"'"
check "the activated service stays and answers again" "$A dbus-send --session --print-reply --dest=org.tcos.Echo /org/tcos/Echo org.tcos.Echo.Echo string:again | grep -q 'string \"again\"' && [ \$(grep -c 'activate: org.tcos.Echo ' $work/act.out) = 1 ]"
check "StartServiceByName (gdbus): 1, then 2" "$A gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.StartServiceByName org.tcos.Echo2 0 | grep -q '(uint32 1,)' && $A gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.StartServiceByName org.tcos.Echo2 0 | grep -q '(uint32 2,)'"
check "busctl lists the activatable names" "$A busctl --user list --activatable --no-pager | grep -q org.tcos.Slow"
check "a service that exits: Spawn.ChildExited" "{ $A dbus-send --session --print-reply --dest=org.tcos.False /x org.tcos.X.Y 2>&1 || true; } | grep -q 'Spawn.ChildExited'"
check "a program that is not there: Spawn.ExecFailed" "{ $A dbus-send --session --print-reply --dest=org.tcos.Missing /x org.tcos.X.Y 2>&1 || true; } | grep -q 'Spawn.ExecFailed'"
check "a service that never takes its name: TimedOut" "{ $A dbus-send --session --print-reply --dest=org.tcos.Slow /x org.tcos.X.Y 2>&1 || true; } | grep -q 'org.freedesktop.DBus.Error.TimedOut'"
sleep 0.3
stopped=$(sed -n 's/^activate: stopped //p' "$work/act.out" | head -1)
check "... and its program is stopped" "[ -n '$stopped' ] && { [ ! -d /proc/$stopped ] || grep -q '^State:.*Z' /proc/$stopped/status; }"
check "UpdateActivationEnvironment (dbus-send)" "$A dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.UpdateActivationEnvironment dict:string:string:TCOS_FROM_BUS,set-by-update > /dev/null"
check "UpdateActivationEnvironment (gdbus)" "$A gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.UpdateActivationEnvironment \"{'TCOS_GDBUS': 'g', 'TCOS_FROM_BUS': 'replaced'}\" > /dev/null"
check "a service started later has them" "$A dbus-send --session --print-reply --dest=org.tcos.Env /org/tcos/Echo org.tcos.Echo.Getenv string:TCOS_FROM_BUS | grep -q 'string \"replaced\"' && $A dbus-send --session --print-reply --dest=org.tcos.Env /org/tcos/Echo org.tcos.Echo.Getenv string:TCOS_GDBUS | grep -q 'string \"g\"'"
check "... and still the starter variables" "$A dbus-send --session --print-reply --dest=org.tcos.Env /org/tcos/Echo org.tcos.Echo.Getenv string:DBUS_STARTER_BUS_TYPE | grep -q 'string \"session\"'"
check "a SystemdService= unit runs without systemd" "$A dbus-send --session --print-reply --dest=org.tcos.Unit /org/tcos/Echo org.tcos.Echo.Echo string:from-the-unit | grep -q 'string \"from-the-unit\"' && grep -q 'activate: org.tcos.Unit -> $work/echoservice' $work/act.out"
check "... with the unit's Environment= and the bus's" "$A dbus-send --session --print-reply --dest=org.tcos.Unit /org/tcos/Echo org.tcos.Echo.Getenv string:FROM_UNIT | grep -q 'string \"unit-env\"' && $A dbus-send --session --print-reply --dest=org.tcos.Unit /org/tcos/Echo org.tcos.Echo.Getenv string:TCOS_GDBUS | grep -q 'string \"g\"'"
check "no unit: Exec=/bin/false fails as without systemd" "{ $A dbus-send --session --print-reply --dest=org.tcos.NoUnit /x org.tcos.X.Y 2>&1 || true; } | grep -q 'Spawn.ChildExited'"
check "sd-bus StartUnit-free activation (busctl call)" "$A busctl --user call org.tcos.Echo2 /org/tcos/Echo org.tcos.Echo Echo s via-busctl | grep -q 'via-busctl'"
# reply timeout: the bus answers a call nobody answers after 1.5 s (the client would wait 20 s)
start=$(date +%s%N)
$A dbus-send --session --print-reply --reply-timeout=20000 --dest=org.tcos.Echo /org/tcos/Echo org.tcos.Echo.Hang > "$work/hang.out" 2>&1 || true
elapsed=$(( ($(date +%s%N) - start) / 1000000 ))
check "an unanswered call gets NoReply from the bus (${elapsed} ms)" "grep -q 'org.freedesktop.DBus.Error.NoReply' $work/hang.out && [ $elapsed -ge 1000 ] && [ $elapsed -lt 6000 ]"
for n in Echo Env Unit; do $A dbus-send --session --print-reply --dest=org.tcos.$n /org/tcos/Echo org.tcos.Echo.Quit > /dev/null 2>&1 || true; done
check "no sanitizer report" "! grep -q 'ERROR: AddressSanitizer\|runtime error' $work/session.err $work/system.err $work/act.err"
[ $fail -eq 0 ] && echo "dbus interop: all passed" || echo "dbus interop: FAILED"
exit $fail
