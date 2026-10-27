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
"""Removes from src/Windows/stubs/<dll>.inc the exports that a source file now defines (`EXPORT <type> NTAPI|WINAPI Name(` at the start of a line, or a name given with -n).

Usage: scripts/unstub.py <dll> <source file> ...      (the source files of the DLL: .c and .inc)
"""
import os
import re
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
dll = sys.argv[1]
names = set()
for path in sys.argv[2:]:
    for line in open(path, errors="replace"):
        m = re.match(r"^\s*(?:EXPORT|__declspec\(dllexport\))[^()=;]*?\bFN\((\w+)\)\s*\(", line)
        if m:      # a function written once for narrow and wide characters (FN(Name) is NameA and NameW)
            names.add(m.group(1) + "A")
            names.add(m.group(1) + "W")
            continue
        m = re.match(r"^\s*(?:EXPORT|__declspec\(dllexport\))[^()=;]*?\b(\w+)\s*\(", line)
        if m:
            names.add(m.group(1))
inc = os.path.join(root, "src", "Windows", "stubs", dll + ".inc")
lines = open(inc).read().split("\n")
kept = [l for l in lines if not (l.startswith("X(") and l[2:-1] in names)]
open(inc, "w").write("\n".join(kept))
print("removed %d stubs from %s" % (len(lines) - len(kept), dll))
