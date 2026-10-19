#!/usr/bin/env python3
# TheoryCraftOS - a minimal UEFI x86_64 operating system.
# Copyright (C) 2026 Christopher Schaefer
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""Says why QEMU is paused: `scripts/qemu-status.py [--resume]`. Needs the QMP socket that qemu-debug.sh and gdb-proxy.py open.

query-status gives the reason: "debug" = a debugger (gdb) stopped it, "io-error" = a disk write failed, "internal-error" =
the accelerator gave up, "guest-panicked", "shutdown" (-no-shutdown), "prelaunch" (-S, waiting for Resume)."""
import json, os, socket, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
build = os.environ.get("BUILD_DIR") or next(os.path.join(ROOT, d) for d in ("build", "cmake-build-debug") if os.path.isfile(os.path.join(ROOT, d, "CMakeCache.txt")))
s = socket.socket(socket.AF_UNIX)
s.connect(os.path.join(build, "qmp.sock"))
f = s.makefile("rw")
f.readline()


def call(command, **arguments):
    f.write(json.dumps({"execute": command, "arguments": arguments}) + "\n")
    f.flush()
    while True:
        reply = json.loads(f.readline())
        if "event" not in reply:
            return reply.get("return", reply)


call("qmp_capabilities")
status = call("query-status")
print("status:", status["status"], "(running)" if status["running"] else "(STOPPED)")
if not status["running"]:
    print(call("human-monitor-command", **{"command-line": "info registers -a"}))
    print(call("human-monitor-command", **{"command-line": "info block"}))
    if "--resume" in sys.argv:
        call("cont")
        print("resumed")
