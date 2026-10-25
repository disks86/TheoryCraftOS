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
PORT = int(os.environ.get("QEMU_GDB_PORT", "2159"))


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


def gpu_arguments():
    """The display adapter and display backend, from QEMU_GPU like scripts/qemu-debug.sh: auto (the default: gl when this QEMU has
    virtio-gpu-gl-pci, else std), gl (virgl 3D), virtio (virtio-gpu, 2D) or std (Bochs VGA with 64 MiB of video memory)."""
    choice = os.environ.get("QEMU_GPU", "auto")
    if choice == "auto":
        devices = subprocess.run(["qemu-system-x86_64", "-device", "help"], capture_output=True, text=True).stdout
        choice = "gl" if "virtio-gpu-gl-pci" in devices else "std"
    if choice == "gl":
        return ["-vga", "none", "-device", "virtio-gpu-gl-pci"], "gtk,gl=on,show-cursor=off,show-menubar=off"
    if choice == "virtio":
        return ["-vga", "none", "-device", "virtio-gpu-pci"], "gtk,show-cursor=off,show-menubar=off"
    if choice == "std":
        return ["-vga", "none", "-device", "VGA,vgamem_mb=" + os.environ.get("QEMU_VGAMEM", "64")], "gtk,show-cursor=off,show-menubar=off"
    sys.exit("unknown QEMU_GPU=%s (auto, std, virtio, gl)" % choice)


def audio_arguments(build):
    """The HD Audio card and its host backend, from QEMU_AUDIO like scripts/qemu-debug.sh: auto (the default: PipeWire, else PulseAudio,
    ALSA, SDL), any -audiodev driver name, wav (records to audio.wav in the build directory) or none (discards the sound)."""
    choice = os.environ.get("QEMU_AUDIO", "auto")
    if choice == "auto":
        drivers = subprocess.run(["qemu-system-x86_64", "-audiodev", "help"], capture_output=True, text=True).stdout.split()
        choice = next((d for d in ("pipewire", "pa", "alsa", "sdl") if d in drivers), "none")
    backend = f"{choice},id=snd0" + (f",path={os.path.join(build, 'audio.wav')}" if choice == "wav" else "")
    sys.stderr.write(f"[gdb-proxy] QEMU audio backend: {choice}\n")
    return ["-audiodev", backend, "-device", "ich9-intel-hda", "-device", "hda-duplex,audiodev=snd0"]


def start_qemu(build):
    esp = os.path.join(build, "esp", "EFI", "BOOT")
    os.makedirs(esp, exist_ok=True)
    shutil.copy(os.path.join(build, "BOOTX64.efi"), os.path.join(esp, "BOOTX64.EFI"))
    vars_fd = os.path.join(build, "OVMF_VARS.fd")
    if not os.path.exists(vars_fd):
        shutil.copy(cache(build, "OVMF_VARS"), vars_fd)
        os.chmod(vars_fd, 0o644)
    log = os.path.join(build, "debugcon.log")
    gpu, display = gpu_arguments()
    cmd = [
        "qemu-system-x86_64", "-machine", "q35", "-smp", os.environ.get("QEMU_SMP", "4"), "-m", "8G", "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0", "-rtc", "base=localtime",
        # The tablet gives the desktop an absolute pointer: no click-to-grab, and the host pointer maps
        # straight onto the guest's (relative PS/2 grabbing is unreliable under Wayland).
        "-device", "virtio-tablet-pci",
        "-display", os.environ.get("QEMU_DISPLAY", display),
        "-gdb", f"tcp:127.0.0.1:{PORT}", "-S",
        # `scripts/qemu-status.py` asks this socket why the VM is paused.
        "-qmp", f"unix:{os.path.join(build, 'qmp.sock')},server=on,wait=off",
        "-d", "guest_errors,unimp", "-D", os.path.join(build, "qemu-guest.log"),
        "-drive", f"if=pflash,format=raw,readonly=on,file={cache(build, 'OVMF_CODE')}",
        "-drive", f"if=pflash,format=raw,file={vars_fd}",
        "-drive", f"format=raw,file=fat:rw:{os.path.join(build, 'esp')}",
        "-chardev", f"stdio,id=dbg,logfile={log},signal=off",
        "-device", "isa-debugcon,iobase=0x402,chardev=dbg", "-serial", "null",
    ] + gpu + audio_arguments(build) + os.environ.get("QEMU_EXTRA", "").split()
    # QEMU's stdout would corrupt the gdb channel: send it to stderr instead.
    return subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=sys.stderr.fileno())


SIGNALS = {2: "SIGINT (pause requested)", 4: "SIGILL (invalid instruction)", 5: "SIGTRAP (breakpoint, watchpoint or step)",
           8: "SIGFPE (divide error)", 11: "SIGSEGV (page fault / memory fault)", 6: "SIGABRT"}


def report_stops(buf):
    """Says on stderr (the IDE's debugger console) whenever QEMU stops the virtual machine: a stopped VM shows
    as [Paused] in QEMU's window and looks like a hang. Returns the unparsed tail of the stream."""
    while True:
        start = max(buf.find(b"$T"), -1)
        s_start = buf.find(b"$S")
        if start < 0 or (0 <= s_start < start):
            start = s_start
        if start < 0:
            return buf[-8:]
        end = buf.find(b"#", start)
        if end < 0 or len(buf) < end + 3:
            return buf[start:]
        body = buf[start + 2:end].decode("ascii", "replace")
        sig = int(body[:2], 16) if body[:2].strip() else 0
        details = [f for f in body[2:].split(";") if f]
        sys.stderr.write(f"[gdb-proxy] the VM was stopped by the debugger: {SIGNALS.get(sig, 'signal %d' % sig)}"
                         f"{' [' + ', '.join(details) + ']' if details else ''}\n")
        sys.stderr.flush()
        buf = buf[end + 3:]


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
        pending = b""
        while True:
            data = sock.recv(65536)
            if not data:
                break
            os.write(1, data)
            pending = (pending + data)[-4096:]
            pending = report_stops(pending)
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
