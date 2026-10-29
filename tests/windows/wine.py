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
"""Wine's conformance tests (dlls/<dll>/tests) as Windows programs for this OS. See README.md.

  tests/windows/wine.py import <wine checkout> [dll ...]   copy the tests (and the headers they need) from a Wine source tree, keeping the ones that compile
  tests/windows/wine.py build <output directory> [dll ...] build one program per test, <dll>_<test>.exe, with the OS's mingw compiler
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wine")      # the tests; this script stays outside because a DLL directory named msvcrt would shadow Python's
CC = os.environ.get("CC", "x86_64-w64-mingw32-gcc")
WIDL = os.environ.get("WIDL", "widl")
ENV = dict(os.environ, LC_ALL="C")
# The DLLs this OS has files for (src/Windows): the tests of these are imported unless the command line names others.
OUR_DLLS = ("ntdll kernel32 kernelbase advapi32 ws2_32 msvcrt ucrtbase win32u user32 gdi32 shell32 ole32 oleaut32 comctl32 comdlg32 rpcrt4 shlwapi winmm version imm32 "
            "secur32 crypt32 bcrypt ncrypt userenv setupapi iphlpapi dbghelp psapi wininet winhttp wintrust netapi32 dwmapi uxtheme mswsock gdiplus msimg32 opengl32 glu32 "
            "dnsapi mpr wtsapi32 powrprof cfgmgr32 d3d9 d3d11 dxgi dwrite shcore avrt winspool.drv wevtapi pdh wldap32 urlmon msi propsys hid usp10 winscard odbc32 "
            "xinput1_4 dsound dinput8 d2d1 d3dcompiler_47 lz32 oleacc oledlg credui ntdsapi samlib activeds normaliz sensapi authz rasapi32").split()
# Libraries every test may need; a test names its own through the imports of its DLL (-l<dll>).
COMMON_LIBS = ["ntdll", "user32", "advapi32", "gdi32", "kernel32", "uuid", "ole32", "oleaut32", "shell32", "comctl32", "winmm", "shlwapi", "version", "imm32", "ws2_32"]


def compiler_include():
    return subprocess.run([CC, "-print-file-name=include"], capture_output=True, text=True).stdout.strip()


def flags(extra_include=(), dll_gen=None):
    # Wine's own headers replace the compiler's (they and mingw's disagree), the compiler keeps only its freestanding ones (stdarg.h ...).
    fl = ["-O1", "-w", "-nostdinc", "-isystem", compiler_include(), "-I", os.path.join(HERE, "include"), "-I", os.path.join(OUT, "gen") if OUT else "",
          "-I", os.path.join(HERE, "include", "msvcrt"), "-I", dll_gen or "", "-D__WINESRC__", "-D_WIN32_WINNT=0x0a00", "-ffunction-sections", "-fdata-sections"]
    return [f for f in fl if f] + list(extra_include)


OUT = None


def run(cmd, cwd=None):
    return subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, env=ENV)


def dll_dirs(names):
    found = sorted(d for d in os.listdir(HERE) if os.path.isdir(os.path.join(HERE, d)) and d != "include")
    return [d for d in found if not names or d in names]


# ---- import ------------------------------------------------------------------------------------------------------------------------------------------------

def do_import(wine, names):
    global OUT
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        OUT = tmp
        shutil.rmtree(os.path.join(HERE, "include"), ignore_errors=True)
        shutil.copytree(os.path.join(wine, "include"), os.path.join(HERE, "include"))
        os.makedirs(os.path.join(tmp, "gen"))
        generate_headers(os.path.join(tmp, "gen"))
        kept = {}
        for tests in sorted(glob.glob(os.path.join(wine, "dlls", "*", "tests"))):
            dll = tests.split(os.sep)[-2]
            if dll not in (names or OUR_DLLS):
                continue
            sources = sorted(glob.glob(os.path.join(tests, "*.c")))
            dll_gen = test_headers(tests, os.path.join(tmp, "gen_" + dll))
            with ThreadPoolExecutor(8) as ex:
                ok = list(ex.map(lambda s: compile_one(s, os.path.join(tmp, dll + "_" + os.path.basename(s) + ".o"), dll_gen).returncode == 0, sources))
            good = [s for s, o in zip(sources, ok) if o]
            if not good:
                continue
            dest = os.path.join(HERE, dll)
            shutil.rmtree(dest, ignore_errors=True)
            os.makedirs(dest)
            for name in sorted(os.listdir(tests)):      # the sources that compile, and every other file (headers, IDL, resources) the tests may need
                path = os.path.join(tests, name)
                if os.path.isfile(path) and (not name.endswith(".c") or path in good):
                    shutil.copy(path, dest)
            kept[dll] = (len(good), len(sources))
        # the Makefile.in of each DLL names the libraries its tests import
        for dll in kept:
            mk = os.path.join(wine, "dlls", dll, "tests", "Makefile.in")
            m = re.search(r"IMPORTS\s*=([^\n]*)", open(mk).read()) if os.path.exists(mk) else None
            with open(os.path.join(HERE, dll, "imports.txt"), "w") as f:
                f.write((m.group(1).strip() if m else "") + "\n")
        for dll, (g, n) in kept.items():
            print("%-12s %3d of %3d files compile" % (dll, g, n))


def generate_headers(gen):
    idls = sorted(glob.glob(os.path.join(HERE, "include", "*.idl")))

    def one(idl):
        name = os.path.basename(idl)[:-4]
        return run([WIDL, "-h", "-o", os.path.join(gen, name + ".h"), "-I", os.path.join(HERE, "include"), "-D__WINESRC__", idl])

    with ThreadPoolExecutor(8) as ex:
        list(ex.map(one, idls))


def compile_one(source, obj, dll_gen=None):
    return run([CC] + flags(dll_gen=dll_gen) + ["-c", "-o", obj, source])


def test_headers(tests, gen):
    """The headers that widl makes from the IDL files of a DLL's tests; returns their directory."""
    os.makedirs(gen, exist_ok=True)
    for idl in sorted(glob.glob(os.path.join(tests, "*.idl"))):
        run([WIDL, "-h", "-o", os.path.join(gen, os.path.basename(idl)[:-4] + ".h"), "-I", os.path.join(HERE, "include"), "-I", tests, "-D__WINESRC__", idl])
    return gen


def guid_library(out):
    """libwineguids.a: the GUIDs (IID_*, CLSID_*) of all of Wine's headers, which the tests expect to find in a library (Wine's libuuid). Every definition is weak
    because the headers define the GUIDs of the headers they include as well."""
    lib = os.path.join(out, "libwineguids.a")
    if os.path.exists(lib):
        return lib
    objdir = os.path.join(out, "guids")
    os.makedirs(objdir, exist_ok=True)
    headers = sorted(glob.glob(os.path.join(out, "gen", "*.h")))

    def one(h):
        name = os.path.basename(h)[:-2]
        c = os.path.join(objdir, name + ".c")
        with open(c, "w") as f:
            f.write('#include <stdarg.h>\n#define INITGUID\n#include <windef.h>\n#include <winbase.h>\n#include <guiddef.h>\n#include "%s.h"\n' % name)
        o = c[:-2] + ".o"
        if run([CC] + flags() + ["-c", "-o", o, c]).returncode:
            return None
        run([CC.replace("gcc", "objcopy"), "--weaken", o])
        return o

    with ThreadPoolExecutor(8) as ex:
        objs = [o for o in ex.map(one, headers) if o]
    run([CC.replace("gcc", "ar"), "rcs", lib] + objs)
    return lib


def available(libs):
    have = set(os.path.basename(p)[3:-2] for p in glob.glob("/usr/x86_64-w64-mingw32/lib/lib*.a"))
    return [l for l in libs if l[2:] in have]


# ---- build -------------------------------------------------------------------------------------------------------------------------------------------------

def do_build(out, names):
    global OUT
    OUT = os.path.abspath(out)
    os.makedirs(os.path.join(OUT, "gen"), exist_ok=True)
    os.makedirs(os.path.join(OUT, "obj"), exist_ok=True)
    if not glob.glob(os.path.join(OUT, "gen", "*.h")):
        generate_headers(os.path.join(OUT, "gen"))
    guids = guid_library(OUT)
    shim_obj = os.path.join(OUT, "obj", "shim.o")
    r = run([CC] + flags() + ["-c", "-o", shim_obj, os.path.join(HERE, "shim.c")])
    if r.returncode:
        sys.exit(r.stderr)
    report = {"built": [], "compile_failed": [], "link_failed": []}
    for dll in dll_dirs(names):
        src = os.path.join(HERE, dll)
        files = sorted(glob.glob(os.path.join(src, "*.c")))
        objdir = os.path.join(OUT, "obj", dll)
        os.makedirs(objdir, exist_ok=True)

        dll_gen = test_headers(src, os.path.join(OUT, "gen_" + dll))

        def comp(s):
            return s, compile_one(s, os.path.join(objdir, os.path.basename(s)[:-2] + ".o"), dll_gen)

        with ThreadPoolExecutor(8) as ex:
            results = list(ex.map(comp, files))
        objs, tests, helpers = [], [], []
        for s, r in results:
            if r.returncode:
                report["compile_failed"].append("%s/%s" % (dll, os.path.basename(s)))
                continue
            o = os.path.join(objdir, os.path.basename(s)[:-2] + ".o")
            names_in = re.findall(r"^START_TEST\(\s*(\w+)\s*\)", open(s, errors="replace").read(), re.M)
            if names_in:
                tests.append((s, o, names_in))
            else:
                helpers.append(o)
        imports = open(os.path.join(src, "imports.txt")).read().split() if os.path.exists(os.path.join(src, "imports.txt")) else []
        libs = ["-l" + i for i in imports + [dll] + COMMON_LIBS]
        libs = [l for i, l in enumerate(libs) if l not in libs[:i]]
        libs = available(libs) + [guids]
        # Wine links its tests against more libraries than a plain link would import (a test that looks for a library in its own process needs it there): force_imports.txt has
        # lines "<test>: <symbol> ..." whose symbols are kept as undefined so that the import is made.
        forced = {}
        fi = os.path.join(src, "force_imports.txt")
        if os.path.exists(fi):
            for line in open(fi):
                if ":" in line:
                    k, v = line.split(":", 1)
                    forced[k.strip()] = ["-Wl,-u," + x for x in v.split()]
        # lets the link of a test see its DLL's import library first
        for s, o, names_in in tests:
            others = [t[1] for t in tests if t[1] != o]
            for name in names_in:
                tl = os.path.join(objdir, "testlist_%s.c" % name)
                with open(tl, "w") as f:
                    f.write('#define STANDALONE\n#include <windef.h>\n#include <winbase.h>\n#include <wincon.h>\n#include "wine/test.h"\n'
                            'extern void func_%s(void);\nconst struct test winetest_testlist[] = { { "%s", func_%s }, { 0, 0 } };\n' % (name, name, name))
                tlo = tl[:-2] + ".o"
                r = run([CC] + flags(dll_gen=dll_gen) + ["-include", "stdarg.h", "-c", "-o", tlo, tl])
                if r.returncode:
                    report["link_failed"].append("%s/%s: testlist: %s" % (dll, name, (r.stderr.strip().splitlines() or ["?"])[0]))
                    continue
                exe = os.path.join(OUT, "%s_%s.exe" % (dll, name))
                # the test, the helper files, and (if it needs them) the other tests of the DLL, which are kept out of the link by --gc-sections
                for extra in ([], helpers, helpers + others):
                    r = run([CC, "-o", exe, tlo, o, shim_obj] + extra + ["-Wl,--gc-sections"] + forced.get(name, []) + libs)
                    if r.returncode == 0:
                        break
                if r.returncode:
                    err = [l for l in r.stderr.splitlines() if "undefined reference" in l or "cannot find" in l]
                    report["link_failed"].append("%s/%s: %s" % (dll, name, (err or r.stderr.strip().splitlines() or ["?"])[0][-160:]))
                else:
                    report["built"].append(os.path.basename(exe))
    with open(os.path.join(OUT, "manifest.json"), "w") as f:
        json.dump(report, f, indent=1)
    print("built %d programs, %d files did not compile, %d did not link (see %s/manifest.json)" % (len(report["built"]), len(report["compile_failed"]), len(report["link_failed"]), OUT))


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "import":
        do_import(sys.argv[2], sys.argv[3:])
    elif len(sys.argv) >= 3 and sys.argv[1] == "build":
        do_build(sys.argv[2], sys.argv[3:])
    else:
        sys.exit(__doc__)
