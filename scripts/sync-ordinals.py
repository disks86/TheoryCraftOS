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
"""Gives the exports of this OS's DLLs the ordinals the same exports have in Windows, so that a program that imports by number (chrome.dll takes bind,
connect and recv from ws2_32.dll and SysAllocString from oleaut32.dll by ordinal) gets the function it means.

For every DLL in the build directory that a folder of Windows DLLs also has, each name both export gets Windows' ordinal in src/Windows/<dll>/<dll>.def
(the file is made if there is none; names it lists keep their forwarders and flags). Windows exports that have an ordinal but no name cannot be matched
this way: they are listed at the end, to be named by hand. The DLLs are linked again by the next build.

Usage: scripts/sync-ordinals.py <folder with the Windows DLLs> [build/windows] [dll ...]
"""
import importlib.util
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location("check_imports", os.path.join(ROOT, "scripts", "check-imports.py"))
check_imports = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check_imports)


def ordinals(path):
    """name -> ordinal, and the ordinals that have no name."""
    pe = check_imports.Pe(path)
    rva, _ = pe.dirs[0]
    if not rva:
        return {}, set()
    o = pe.off(rva)
    base, nfunc, nname, afunc, aname, aord = struct.unpack_from("<IIIIII", pe.data, o + 16)
    named, used = {}, set()
    for i in range(nname):
        name = pe.cstr(struct.unpack_from("<I", pe.data, pe.off(aname) + 4 * i)[0])
        index = struct.unpack_from("<H", pe.data, pe.off(aord) + 2 * i)[0]
        named[name] = base + index
        used.add(index)
    unnamed = {base + i for i in range(nfunc) if i not in used and struct.unpack_from("<I", pe.data, pe.off(afunc) + 4 * i)[0]}
    return named, unnamed


LINE = re.compile(r"^(\s*)([A-Za-z_?@$][^\s=]*)(\s*=\s*\S+)?(\s+@\d+)?(.*)$")


def sync(dll, windows_dir, built_dir):
    ours_path = os.path.join(built_dir, dll + ".dll")
    theirs_path = next((os.path.join(windows_dir, dll + ext) for ext in (".dll", ".drv", ".cpl") if os.path.exists(os.path.join(windows_dir, dll + ext))), None)
    if not theirs_path or not os.path.exists(ours_path):
        return None
    ours, _ = ordinals(ours_path)
    theirs, unnamed = ordinals(theirs_path)
    wanted = {name: theirs[name] for name in ours if name in theirs}
    wanted_ordinals = set(wanted.values())
    def_path = os.path.join(ROOT, "src", "Windows", dll, dll + ".def")
    lines = open(def_path).read().split("\n") if os.path.exists(def_path) else ["LIBRARY %s.dll" % dll, "EXPORTS"]
    listed, dropped, out = set(), [], []
    for line in lines:
        m = LINE.match(line)
        if not m or m.group(2) in ("LIBRARY", "EXPORTS") or line.strip().startswith(";"):
            out.append(line)
            continue
        name = m.group(2)
        old = re.search(r"@(\d+)", m.group(4) or "")
        ordinal = wanted.get(name)
        if ordinal is None:
            if old and int(old.group(1)) in wanted_ordinals:      # a placeholder on a number Windows gives a name we export: the name takes it
                dropped.append(name)
                continue
            out.append(line)
            continue
        listed.add(name)
        rest = m.group(5)
        if "NONAME" in rest:      # (it has a name in Windows: it keeps it)
            rest = rest.replace("NONAME", "").rstrip()
        out.append("%s%s%s @%d%s" % (m.group(1), name, m.group(3) or "", ordinal, rest))
    added = 0
    marker = "; The ordinals Windows gives these exports (scripts/sync-ordinals.py)"
    for name, ordinal in sorted(wanted.items(), key=lambda kv: kv[1]):
        if name in listed:
            continue
        if marker not in out:
            while out and out[-1] == "":
                out.pop()
            out.append(marker)
        out.append("%s @%d" % (name, ordinal))
        added += 1
    os.makedirs(os.path.dirname(def_path), exist_ok=True)
    open(def_path, "w").write("\n".join(out).rstrip("\n") + "\n")
    return added, len(wanted), sorted(unnamed), dropped


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    windows_dir = sys.argv[1]
    built_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "windows")
    # (ntdll, kernel32, msvcrt and win32u are imported by name only; their exports are many and their ordinals change between versions of Windows)
    dlls = sys.argv[3:] or sorted(f[:-4] for f in os.listdir(built_dir) if f.endswith(".dll") and f[:-4] not in ("ntdll", "kernel32", "msvcrt", "win32u"))
    for dll in dlls:
        result = sync(dll, windows_dir, built_dir)
        if result is None:
            continue
        added, matched, unnamed, dropped = result
        print("%-16s %4d named exports matched, %4d added to the .def%s%s" % (dll, matched, added, ", unnamed in Windows: " + " ".join(map(str, unnamed[:30])) + (" ..." if len(unnamed) > 30 else "") if unnamed else "",
                                                                           ", placeholders dropped: " + " ".join(dropped) if dropped else ""))


if __name__ == "__main__":
    main()
