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
#
# A sampling profiler for the OS running under QEMU: reads every virtual CPU's instruction pointer through the human monitor
# (-monitor unix:<socket>,server=on,wait=off) `info registers -a` as often as it can for a while, then names the places by the
# kernel's symbols (BOOTX64.efi, loaded at the TCOS_IMAGE_BASE the boot prints) and by the symbols of the user-mode modules the
# kernel's program report lists ("  module <name> at <base>", found in the given folders by name). Idle CPUs (HLT) are counted apart.
# Usage: scripts/qemu-profile.py <monitor socket> <seconds> <BOOTX64.efi> <debugcon.log> [dll folder...]
import collections
import os
import re
import socket
import subprocess
import sys
import time


def symbols(path, base):
    """Sorted (address, name) of the text symbols of a PE file, rebased to `base` (None: as linked)."""
    try:
        out = subprocess.run(["nm", "--defined-only", "-C", path], capture_output=True, text=True, check=False).stdout
    except OSError:
        return []
    image_base = None
    try:
        head = subprocess.run(["objdump", "-p", path], capture_output=True, text=True, check=False).stdout
        m = re.search(r"^ImageBase\s+([0-9a-fA-F]+)", head, re.M)
        if m:
            image_base = int(m.group(1), 16)
    except OSError:
        pass
    result = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) < 3 or parts[1] not in "tTwW":
            continue
        address = int(parts[0], 16)
        if base is not None and image_base is not None:
            address = address - image_base + base
        result.append((address, parts[2]))
    result.sort()
    return result


def lookup(table, address):
    lo, hi = 0, len(table)
    while lo < hi:
        mid = (lo + hi) // 2
        if table[mid][0] <= address:
            lo = mid + 1
        else:
            hi = mid
    return table[lo - 1][1] if lo else None


def main():
    if len(sys.argv) < 5:
        sys.exit("usage: qemu-profile.py <monitor socket> <seconds> <BOOTX64.efi> <debugcon.log> [dll folder...]")
    sock_path, seconds, kernel_file, log_file = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4]
    folders = sys.argv[5:]
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.settimeout(5)

    def command(text):
        s.sendall(text.encode() + b"\n")
        data = b""
        while not data.endswith(b"(qemu) "):
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            data += chunk
        return data.decode(errors="replace")

    command("")
    samples = collections.Counter()
    idle = 0
    total = 0
    end = time.time() + seconds
    while time.time() < end:
        text = command("info registers -a")
        for m in re.finditer(r"(?:RIP|EIP)=([0-9a-fA-F]+)[^\n]*?HLT=(\d)", text):
            total += 1
            if m.group(2) == "1":
                idle += 1
                continue
            samples[int(m.group(1), 16)] += 1
    log = open(log_file, "rb").read().decode(errors="replace")
    m = re.search(r"TCOS_IMAGE_BASE=0x([0-9a-fA-F]+)", log)
    kernel = symbols(kernel_file, int(m.group(1), 16) if m else None)
    modules = {}
    for name, base in re.findall(r"^  module (\S+) at 0x([0-9a-fA-F]+)", log, re.M):
        modules[name.lower()] = int(base, 16)
    tables = []
    for name, base in modules.items():
        for folder in folders:
            for candidate in (name, name.replace(".dll", "") + ".dll"):
                path = os.path.join(folder, candidate)
                if os.path.exists(path):
                    tables.append((base, name, symbols(path, base)))
                    break
    by_name = collections.Counter()
    for address, count in samples.items():
        name = lookup(kernel, address) if kernel and address >= kernel[0][0] and address < kernel[-1][0] + 0x100000 else None
        where = "kernel" if name else None
        if not name:
            best = None
            for base, module, table in tables:
                if address >= base and (best is None or base > best[0]):
                    best = (base, module, table)
            if best:
                sym = lookup(best[2], address)
                name = best[1] + "!" + (sym or hex(address - best[0]))
            elif modules:
                below = [(b, n) for n, b in modules.items() if b <= address]
                name = (max(below)[1] + "+" + hex(address - max(below)[0])) if below else hex(address)
            else:
                name = hex(address)
        by_name[(where or "user") + " " + name] += count
    busy = total - idle
    print(f"{total} samples, {idle} idle ({100.0 * idle / max(total, 1):.1f}%), {busy} busy")
    for name, count in by_name.most_common(60):
        print(f"{100.0 * count / max(busy, 1):6.2f}%  {name}")


if __name__ == "__main__":
    main()
