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
"""A library's .def file (src/Windows/<dll>/<dll>.def: forwarders, aliases and the ordinals Windows gives the exports) for the i386 build (docs/Wow64.md).

The .def names functions as the 64-bit build has them. On i386 a stdcall function's symbol is _Name@bytes, which the linker does not find by the plain
name, so each line that names a function of the library is rewritten with the decorated name; the build's --kill-at takes the decoration off the
export again. The decorated names are read from the .def the first link of the library wrote (--output-def). Forwarders stay as they are.

Usage: gen-wow64-def.py <dll.def> <decorated.def> <out.def>
"""
import re
import sys


def main():
    source, decorated_path, out = sys.argv[1:4]
    decorated = {}
    with open(decorated_path, encoding="utf-8") as listing:
        for line in listing:
            match = re.match(r"^\s*([A-Za-z_][A-Za-z0-9_]*)@(\d+)\b", line)
            if match:
                decorated[match.group(1)] = "%s@%s" % (match.group(1), match.group(2))
    lines = []
    with open(source, encoding="utf-8") as text:
        for line in text:
            line = line.rstrip("\n")
            alias = re.match(r"^(\s*)([A-Za-z_][A-Za-z0-9_]*)(\s*=\s*)([A-Za-z_][A-Za-z0-9_]*)(\s.*|)$", line)      # Name = Internal [@ordinal]
            plain = re.match(r"^(\s*)([A-Za-z_][A-Za-z0-9_]*)(\s+@\d+.*|\s*)$", line)                                 # Name [@ordinal]
            if alias and alias.group(4) in decorated:
                line = alias.group(1) + alias.group(2) + alias.group(3) + decorated[alias.group(4)] + alias.group(5)
            elif plain and plain.group(2) in decorated and plain.group(2) not in ("EXPORTS", "LIBRARY"):
                line = plain.group(1) + decorated[plain.group(2)] + plain.group(3)
            lines.append(line)
    with open(out, "w", encoding="utf-8") as target:
        target.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
