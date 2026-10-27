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
"""The mingw-w64 import libraries leave out exports that the real DLLs have (internal ones, newer ones). Wine's tests look such functions up by name with GetProcAddress
and call them if they find them, so a missing name is a skipped test and a stub is a failed one. This adds the names the tests look up to the stub lists:
src/Windows/stubs/<dll>.inc for the DLL whose tests (tests/windows/wine/<dll>) mention them, or ntdll for Nt/Rtl/Ldr names.

Usage: scripts/gen-test-stubs.py <directory with the built DLLs>   (it skips every name the DLL already exports)
"""
import glob
import os
import re
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
built = sys.argv[1]
wine = os.path.join(root, "tests", "windows", "wine")
NTDLL = re.compile(r"^(Nt|Zw|Rtl|Ldr|Etw|Tp|Dbg|Ki|Csr|Pfx|Wow64|Wine)")
cache = {}


def exports(dll):
    if dll not in cache:
        path = os.path.join(built, dll + ".dll")
        names = set()
        if os.path.exists(path):
            out = subprocess.run(["x86_64-w64-mingw32-objdump", "-p", path], capture_output=True, text=True).stdout
            for line in out.split("[Ordinal/Name Pointer]", 1)[-1].splitlines():
                if "[" in line and line.split():
                    names.add(line.split()[-1])
        cache[dll] = names
    return cache[dll]


added = {}
for dll in sorted(os.listdir(wine)):
    d = os.path.join(wine, dll)
    if not os.path.isdir(d) or dll == "include":
        continue
    name = dll.replace(".drv", "")
    for src in glob.glob(os.path.join(d, "*.c")):
        text = open(src, errors="replace").read()
        found = set(re.findall(r'GetProcAddress\s*\([^,;]+,\s*"([A-Za-z_]\w*)"', text))
        if re.search(r"#define\s+(\w+)\(\w+\)[^\n]*GetProcAddress\([^\n]*#\w+", text):
            found |= set(re.findall(r"^\s*(?:#define\s+\w+\(\w+\)[^\n]*\n)?\s*[A-Z_]+\((\w+)\)\s*$", text, re.M))
        for n in found:
            target = "ntdll" if NTDLL.match(n) else name
            if target not in ("ntdll", "kernel32", "advapi32", "ws2_32", "msvcrt") and not os.path.exists(os.path.join(root, "src", "Windows", "stubs", target + ".inc")):
                continue
            if n in exports(target):
                continue
            added.setdefault(target, set()).add(n)
for dll, names in added.items():
    inc = os.path.join(root, "src", "Windows", "stubs", dll + ".inc")
    text = open(inc).read()
    have = set(re.findall(r"^X\((\w+)\)", text, re.M))
    new = sorted(n for n in names if n not in have)
    if new:
        open(inc, "w").write(text.rstrip("\n") + "\n" + "".join("X(%s)\n" % n for n in new))
        print("%s: +%d" % (dll, len(new)))
