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

"""Start QEMU and bridge gdb to its gdbstub: use as `target remote | scripts/gdb-proxy.py`.

An IDE can attach before OVMF has loaded the OS image, and software
breakpoints (int3 patched into RAM) would then be overwritten by the loader.
So this proxy rewrites software breakpoint packets (Z0/z0) into hardware ones
(Z1/z1), which QEMU implements without touching guest memory. QEMU starts
frozen (-S); press Resume once in the debugger. OS output goes to stderr,
which the IDE shows in its debugger console. QEMU exits with the debugger.
"""
import os, queue, shutil, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 1234


def build_dir():
    env = os.environ.get("BUILD_DIR")
    if env:
        return os.path.abspath(env)
    for d in ("build", "cmake-build-debug"):
        if os.path.isfile(os.path.join(ROOT, d, "CMakeCache.txt")):
            return os.path.join(ROOT, d)
    sys.exit("no CMake build directory found")


def cache(build, key):
    with open(os.path.join(build, "CMakeCache.txt")) as f:
        for line in f:
            if line.startswith(key + ":"):
                return line.split("=", 1)[1].strip()
    sys.exit(f"{key} missing from CMakeCache.txt")


def start_qemu(build):
    esp = os.path.join(build, "esp", "EFI", "BOOT")
    os.makedirs(esp, exist_ok=True)
    shutil.copy(os.path.join(build, "BOOTX64.efi"), os.path.join(esp, "BOOTX64.EFI"))
    vars_fd = os.path.join(build, "OVMF_VARS.fd")
    if not os.path.exists(vars_fd):
        shutil.copy(cache(build, "OVMF_VARS"), vars_fd)
        os.chmod(vars_fd, 0o644)
    log = os.path.join(build, "debugcon.log")
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-smp", os.environ.get("QEMU_SMP", "4"), "-m", "256M", "-net", "none",
        "-display", os.environ.get("QEMU_DISPLAY", "gtk"),
        "-gdb", f"tcp:127.0.0.1:{PORT}", "-S",
        "-drive", f"if=pflash,format=raw,readonly=on,file={cache(build, 'OVMF_CODE')}",
        "-drive", f"if=pflash,format=raw,file={vars_fd}",
        "-drive", f"format=raw,file=fat:rw:{os.path.join(build, 'esp')}",
        "-chardev", f"stdio,id=dbg,logfile={log},signal=off",
        "-device", "isa-debugcon,iobase=0x402,chardev=dbg", "-serial", "null",
    ]
    # QEMU's stdout would corrupt the gdb channel: send it to stderr instead.
    return subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=sys.stderr.fileno())


def checksum(payload):
    return b"%02x" % (sum(payload) & 0xFF)


class Rewriter:
    """Incrementally rewrites Z0/z0 packets in the gdb -> QEMU byte stream."""

    def __init__(self):
        self.buf = b""

    def feed(self, data):
        self.buf += data
        out = b""
        while self.buf:
            if self.buf[:1] != b"$":
                # acks, interrupt (0x03) etc. pass through one byte at a time
                out += self.buf[:1]
                self.buf = self.buf[1:]
                continue
            end = self.buf.find(b"#")
            if end < 0 or len(self.buf) < end + 3:
                break  # incomplete packet
            payload = self.buf[1:end]
            if payload[:2] in (b"Z0", b"z0"):
                payload = payload[:1] + b"1" + payload[2:]
            out += b"$" + payload + b"#" + checksum(payload)
            self.buf = self.buf[end + 3:]
        return out


def newest_packet(chunks):
    """gdb times out after ~2s and moves on to its next handshake packet, so
    only the newest packet sent while we were busy is still being waited for."""
    data = b"".join(chunks)
    last = data.rfind(b"$")
    return data[last:] if last >= 0 else b""


def main():
    build = build_dir()
    inbox = queue.Queue()  # gdb -> us, fed by a thread so we can build meanwhile

    def read_gdb():
        while True:
            data = os.read(0, 65536)
            inbox.put(data)
            if not data:
                return

    threading.Thread(target=read_gdb, daemon=True).start()

    cmake = cache(build, "CMAKE_COMMAND")
    rc = subprocess.call([cmake, "--build", build, "--target", "BOOTX64"],
                         stdin=subprocess.DEVNULL, stdout=sys.stderr.fileno())
    if rc:
        sys.exit("build failed")
    held = []
    while not inbox.empty():
        held.append(inbox.get())
    if held and held[-1] == b"":
        return

    qemu = start_qemu(build)
    sock = None
    for _ in range(100):
        try:
            sock = socket.create_connection(("127.0.0.1", PORT))
            break
        except OSError:
            time.sleep(0.1)
    if sock is None:
        qemu.kill()
        sys.exit("could not connect to QEMU's gdbstub")

    def qemu_to_gdb():
        while True:
            data = sock.recv(65536)
            if not data:
                break
            os.write(1, data)
        os._exit(0)

    threading.Thread(target=qemu_to_gdb, daemon=True).start()
    rw = Rewriter()
    try:
        first = newest_packet(held)
        if first:
            sock.sendall(rw.feed(first))
        while True:
            data = inbox.get()
            if not data:
                break
            sock.sendall(rw.feed(data))
    finally:
        qemu.kill()


if __name__ == "__main__":
    main()
