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
"""Lists the Windows exports and system services that this OS does not implement yet, from the import libraries of mingw-w64 (which name every export of the real DLLs).

Writes, under src/Windows:
  stubs/<dll>.inc          one X(Name) per export the DLL does not implement; stubs/stubs.c turns the list into exported functions that do nothing
  ntdll/services.inc       NT_SERVICE(NtName) for every service that ntdll.dll has no stub for yet
  win32u/services.inc      NT_SERVICE(NtUserName), NT_SERVICE(NtGdiName), ... for the services of win32k (the names are scripts/win32k-services.txt)
  ntdll/zw.inc             ZW_SERVICE(NtName, ZwName): the Zw twin of every service
  tcos_syscalls.h          the numbers of the new services (also added to WindowsSyscallNumber in src/Process/WindowsSyscalls.hpp)

It is meant to be run once from a build with the current DLLs, and again only to pick up new names: numbers of services never change once given. To implement an
export, write it in the DLL and delete its X(...) line from stubs/<dll>.inc (the linker reports the duplicate otherwise).

Usage: scripts/gen-windows-stubs.py <directory with the built ntdll.dll kernel32.dll msvcrt.dll advapi32.dll ws2_32.dll>
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIN = os.path.join(ROOT, "src", "Windows")
LIB = "/usr/x86_64-w64-mingw32/lib"
NM = "x86_64-w64-mingw32-nm"
OBJDUMP = "x86_64-w64-mingw32-objdump"

# The DLLs that already exist, with the import libraries that name their exports. (msvcrt.dll is one library for all the C runtimes: see canonical_library_name().)
EXISTING = {
    "ntdll": ["ntdll"],
    "kernel32": ["kernel32"],
    "msvcrt": ["msvcrt", "ucrt", "ucrtbase"],
    "advapi32": ["advapi32"],
    "ws2_32": ["ws2_32", "wsock32"],
}
# The other system libraries of Windows that a program may import from. Each becomes a DLL of the same name with every export in place.
NEW = """user32 gdi32 shell32 ole32 oleaut32 comctl32 comdlg32 rpcrt4 shlwapi winmm version imm32 secur32 crypt32 bcrypt ncrypt userenv setupapi iphlpapi dbghelp psapi
wininet winhttp wintrust netapi32 dwmapi uxtheme mswsock gdiplus msimg32 opengl32 glu32 dnsapi mpr wtsapi32 powrprof cfgmgr32 d3d9 d3d11 dxgi dwrite shcore avrt winspool
wevtapi pdh wldap32 urlmon msi propsys hid usp10 winscard odbc32 xinput1_4 dsound dinput8 d2d1 d3dcompiler_47 lz32 oleacc oledlg credui ntdsapi samlib activeds
normaliz sensapi""".split()
# Exports of ntdll.dll that are ordinary functions although their names look like system services.
NOT_SERVICES = {"NtGetTickCount"}
WIN32K_BASE = 0x1000      # where the win32k services start (the second service table of Windows has index 1: 0x1000)
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def library_exports(libs):
    names = set()
    for lib in libs:
        path = os.path.join(LIB, "lib%s.a" % lib)
        if not os.path.exists(path):
            continue
        out = subprocess.run([NM, "-g", "--defined-only", path], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] == "T" and not parts[2].startswith("__imp_") and IDENT.match(parts[2]):
                names.add(parts[2])
    return names


def dll_exports(path):
    out = subprocess.run([OBJDUMP, "-p", path], capture_output=True, text=True, check=True).stdout
    names = set()
    table = out.split("[Ordinal/Name Pointer]", 1)[-1]
    for line in table.splitlines():
        if "[" in line and line.split():
            names.add(line.split()[-1])
    return names


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


HEADER = """/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
"""


def main():
    built = sys.argv[1]
    existing = {dll: dll_exports(os.path.join(built, dll + ".dll")) for dll in EXISTING}
    for dll in ("kernel32", "msvcrt", "advapi32", "ws2_32"):          # the forwarders of a .def are exports too
        with open(os.path.join(WIN, dll, dll + ".def")) as f:
            for line in f:
                if "=" in line:
                    existing[dll].add(line.split("=")[0].strip())

    # ---- the system services -------------------------------------------------------------------------------------------------------------------------------
    # Two ranges, as on Windows: the services of the executive from 0, those of win32k (called through win32u.dll) from WIN32K_BASE.
    nt_names = sorted(n for n in library_exports(["ntdll"]) if re.match(r"^Nt[A-Z]", n) and n not in NOT_SERVICES)
    with open(os.path.join(ROOT, "scripts", "win32k-services.txt")) as f:
        k_names = [line.strip() for line in f if line.strip() and not line.startswith("#")]
    k_names = [n for n in dict.fromkeys(k_names) if n not in nt_names]
    header_path = os.path.join(WIN, "tcos_syscalls.h")
    with open(header_path) as f:
        header = f.read()
    numbered = {m.group(1): int(m.group(2)) for m in re.finditer(r"#define NT_SERVICE_(\w+) (\d+)", header) if not re.match(r"(COUNT|WIN32K_)", m.group(1))}
    next_number = max(v for v in numbered.values() if v < WIN32K_BASE) + 1
    next_k = max([v for v in numbered.values() if v >= WIN32K_BASE] + [WIN32K_BASE - 1]) + 1
    added = []
    for name in nt_names:
        if name not in numbered:
            numbered[name] = next_number
            added.append((name, next_number))
            next_number += 1
    for name in k_names:
        if name not in numbered:
            numbered[name] = next_k
            added.append((name, next_k))
            next_k += 1
    if added:
        text = re.sub(r"#define NT_SERVICE_(COUNT|WIN32K_BASE|WIN32K_END) \d+\n", "", header)
        text = text.replace("#endif\n", "".join("#define NT_SERVICE_%s %d\n" % a for a in added) + "#endif\n")
        text = text.replace("#endif\n", "#define NT_SERVICE_COUNT %d\n#define NT_SERVICE_WIN32K_BASE %d\n#define NT_SERVICE_WIN32K_END %d\n#endif\n" % (next_number, WIN32K_BASE, next_k))
        write(header_path, text)
        hpp_path = os.path.join(ROOT, "src", "Process", "WindowsSyscalls.hpp")
        with open(hpp_path) as f:
            hpp = f.read()
        hpp = re.sub(r"constexpr uint32_t kWindowsServiceCount = \d+;[^\n]*\n", "", hpp)
        hpp = re.sub(r"constexpr uint32_t kWin32kService(Base|End) = \w+;[^\n]*\n", "", hpp)
        hpp = hpp.replace("};\n\nconstexpr uint32_t kStatusInvalidSystemService", "".join("  %s = %d,\n" % a for a in added) + "};\n\nconstexpr uint32_t kStatusInvalidSystemService", 1)
        hpp = hpp.replace("constexpr uint32_t kStatusInvalidSystemService",
                          "constexpr uint32_t kWindowsServiceCount = %d;      // numbers below this are services of the executive (the ones without code answer STATUS_NOT_IMPLEMENTED)\n"
                          "constexpr uint32_t kWin32kServiceBase = %d, kWin32kServiceEnd = %d;      // the services of win32k (NtUser*, NtGdi*, ...) are these numbers; same answer\n"
                          "constexpr uint32_t kStatusInvalidSystemService" % (next_number, WIN32K_BASE, next_k), 1)
        write(hpp_path, hpp)
    have_stub = existing["ntdll"]
    write(os.path.join(WIN, "win32u", "services.inc"), HEADER + "/* The services of win32k, as win32u.dll exports them: generated by scripts/gen-windows-stubs.py from scripts/win32k-services.txt. They end in\n * STATUS_NOT_IMPLEMENTED until the kernel implements them. */\n" +
          "".join("NT_SERVICE(%s);\n" % n for n in k_names))
    write(os.path.join(WIN, "ntdll", "services.inc"), HEADER + "/* The services that ntdll.dll has no stub for in ntdll.c: generated by scripts/gen-windows-stubs.py. They end in STATUS_NOT_IMPLEMENTED until the kernel\n * implements them. */\n" +
          "".join("NT_SERVICE(%s);\n" % n for n in nt_names if n not in have_stub and not re.search(r"\bNT_SERVICE(_RAW)?\(%s\)" % n, open(os.path.join(WIN, "ntdll", "ntdll.c")).read())))
    write(os.path.join(WIN, "ntdll", "zw.inc"), HEADER + "/* Zw twins of the services (in user mode they are the same calls): generated by scripts/gen-windows-stubs.py. */\n" +
          "".join("ZW_SERVICE(%s, Zw%s);\n" % (n, n[2:]) for n in nt_names if "Zw" + n[2:] not in have_stub))

    # ---- the exports ---------------------------------------------------------------------------------------------------------------------------------------
    counts = {}
    for dll, libs in list(EXISTING.items()) + [(n, [n]) for n in NEW]:
        have = existing.get(dll, set())
        wanted = library_exports(libs)
        if dll == "ntdll":
            wanted = {n for n in wanted if not re.match(r"^(Nt|Zw)[A-Z]", n)}      # the services are the NT_SERVICE lists above
        missing = sorted(wanted - have)
        counts[dll] = len(missing)
        write(os.path.join(WIN, "stubs", dll + ".inc"), HEADER + "/* The exports of %s.dll that are not implemented yet: generated by scripts/gen-windows-stubs.py.\n * To implement one, define it in the DLL and delete its line here. */\n" % dll +
              "".join("X(%s)\n" % n for n in missing))
    print("services added: %d (total %d)" % (len(added), next_number))
    for dll, n in counts.items():
        print("%-14s %5d stubs" % (dll, n))


main()
