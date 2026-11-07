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
"""Lists what the Windows programs and DLLs given need from the system libraries of this OS and what is not there.

For every import (normal and delay-loaded) of each PE file it says whether the system DLL that would provide it (after the same folding as
canonical_library_name() in src/Process/ModuleLoader.cpp) exports it, and if so whether the export is only a stub of stubs/stubs.c (which returns
0 or STATUS_NOT_IMPLEMENTED and does nothing). Imports between the files given (chrome.exe -> chrome_elf.dll) are resolved against those files.

Usage: scripts/check-imports.py [--dlls build/windows] [--all] program.exe [library.dll ...]
  --all   list every missing or stubbed import (otherwise a count per DLL and the names, up to 40 a DLL)
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Pe:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError(path + ": not a PE file")
        sections = struct.unpack_from("<H", d, pe + 6)[0]
        optional_size = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", d, opt)[0]
        self.pe64 = magic == 0x20B
        dirs = opt + (112 if self.pe64 else 96)
        self.dirs = [struct.unpack_from("<II", d, dirs + 8 * i) for i in range(16)]
        self.sections = []
        s = opt + optional_size
        for i in range(sections):
            va_size, va, raw_size, raw = struct.unpack_from("<IIII", d, s + 40 * i + 8)
            self.sections.append((va, max(va_size, raw_size), raw))

    def off(self, rva):
        for va, size, raw in self.sections:
            if va <= rva < va + size:
                return rva - va + raw
        return None

    def cstr(self, rva):
        o = self.off(rva)
        e = self.data.index(b"\0", o)
        return self.data[o:e].decode("latin-1")

    def exports(self):
        """name -> forwarder or None, and ordinal -> same."""
        rva, size = self.dirs[0]
        names, ordinals = {}, {}
        if not rva:
            return names, ordinals
        o = self.off(rva)
        base, nfunc, nname, afunc, aname, aord = struct.unpack_from("<IIIIII", self.data, o + 16)
        funcs = [struct.unpack_from("<I", self.data, self.off(afunc) + 4 * i)[0] for i in range(nfunc)]
        def target(f):
            return self.cstr(f) if rva <= f < rva + size else None
        for i, f in enumerate(funcs):
            if f:
                ordinals[base + i] = target(f)
        for i in range(nname):
            n = self.cstr(struct.unpack_from("<I", self.data, self.off(aname) + 4 * i)[0])
            idx = struct.unpack_from("<H", self.data, self.off(aord) + 2 * i)[0]
            names[n] = target(funcs[idx])
        return names, ordinals

    def _thunks(self, rva):
        out = []
        o = self.off(rva)
        step = 8 if self.pe64 else 4
        flag = 1 << (63 if self.pe64 else 31)
        while True:
            v = struct.unpack_from("<Q" if self.pe64 else "<I", self.data, o)[0]
            if not v:
                return out
            out.append(("#%d" % (v & 0xFFFF)) if v & flag else self.cstr((v & 0x7FFFFFFF) + 2))
            o += step

    def imports(self):
        """[(dll, [names]), ...], normal then delay-loaded (marked delay=True)."""
        out = []
        rva, _ = self.dirs[1]
        if rva:
            o = self.off(rva)
            while True:
                lookup, _, _, name, first = struct.unpack_from("<IIIII", self.data, o)
                if not name:
                    break
                out.append((self.cstr(name), self._thunks(lookup or first), False))
                o += 20
        rva, _ = self.dirs[13]
        if rva:
            o = self.off(rva)
            while True:
                attrs, name, _, _, names, _, _, _ = struct.unpack_from("<IIIIIIII", self.data, o)
                if not name:
                    break
                out.append((self.cstr(name), self._thunks(names), True))
                o += 32
        return out


def canonical(name):
    n = name.lower().replace("/", "\\").split("\\")[-1]
    if "." not in n:
        n += ".dll"
    if n.startswith("api-ms-win-core-winrt-"):
        return "combase.dll"
    if n.startswith("api-ms-win-power-"):
        return "powrprof.dll"
    if n.startswith("api-ms-win-shcore-"):
        return "shcore.dll"
    if n == "winspool.drv":
        return "winspool.dll"
    if n == "bthprops.cpl":
        return "bthprops.dll"
    if n.startswith("api-ms-win-core-") or n.startswith("api-ms-win-eventing-") or n == "kernelbase.dll":
        return "kernel32.dll"
    if n.startswith("api-ms-win-security-"):
        return "advapi32.dll"
    if n in ("wsock32.dll", "winsock.dll"):
        return "ws2_32.dll"
    if n.startswith("xinput1_") or n in ("xinput9_1_0.dll", "xinputuap.dll"):
        return "xinput1_4.dll"
    if n.startswith("api-ms-win-crt-") or n in ("ucrtbase.dll", "ucrtbased.dll", "crtdll.dll") or n.startswith("msvcr") or n.startswith("vcruntime") or n.startswith("msvcp"):
        return "msvcrt.dll"
    return n


def stub_names(dll):
    path = os.path.join(ROOT, "src", "Windows", "stubs", dll[:-4] + ".inc")
    out = set()
    if os.path.exists(path):
        for line in open(path):
            line = line.strip()
            if line.startswith("X(") and line.endswith(")"):
                out.add(line[2:-1])
    return out


def main():
    args = sys.argv[1:]
    dlls = os.path.join(ROOT, "build", "windows")
    show_all = False
    while args and args[0].startswith("--"):
        if args[0] == "--dlls":
            dlls = args[1]
            args = args[2:]
        elif args[0] == "--all":
            show_all = True
            args = args[1:]
        else:
            sys.exit(__doc__)
    if not args:
        sys.exit(__doc__)
    local = {os.path.basename(p).lower(): p for p in args}
    cache = {}

    def library(dll):
        if dll not in cache:
            path = local.get(dll) or os.path.join(dlls, dll)
            cache[dll] = (Pe(path).exports(), stub_names(dll) if dll not in local else set()) if os.path.exists(path) else None
        return cache[dll]

    def resolve(dll, name, depth=0):
        """'ok', 'stub', 'missing' or 'nodll'."""
        lib = library(dll)
        if lib is None:
            return "nodll"
        (names, ordinals), stubs = lib
        if name.startswith("#"):
            key = int(name[1:])
            if key not in ordinals:
                return "missing"
            fwd = ordinals[key]
        else:
            if name not in names:
                return "missing"
            fwd = names[name]
            if name in stubs:
                return "stub"
        if fwd and depth < 8:
            target, _, sym = fwd.partition(".")
            return resolve(canonical(target), sym, depth + 1)
        return "ok"

    for path in args:
        pe = Pe(path)
        report = {}
        for dll, names, delay in pe.imports():
            c = canonical(dll) if dll.lower() not in local else dll.lower()
            for n in names:
                r = resolve(c, n)
                if r != "ok":
                    report.setdefault((c, r), []).append(n + (" (delay)" if delay else ""))
        print("== %s" % os.path.basename(path))
        for (dll, kind), names in sorted(report.items()):
            shown = names if show_all else names[:40]
            print("  %-22s %-8s %4d: %s%s" % (dll, kind, len(names), " ".join(shown), " ..." if len(shown) < len(names) else ""))


if __name__ == "__main__":
    main()
