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
"""Builds Wine's oleaut32 typelib and tmarshal tests, which tests/windows/wine.py cannot link: they need the interface IDs and the type libraries (TYPELIB resources 1 to
4: tmarshal, test_tlb, test_reg, test_simple) that widl makes from the IDL files of the tests.
  scripts/oleaut32-typelib-tests.py <tests dir made by wine.py build> <output dir>       writes <output dir>/oleaut32_typelib.exe, oleaut32_tmarshal.exe and the other tests that need the type libraries (vartype)"""
import glob
import os
import subprocess
import sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(root, "tests", "windows"))
import wine  # noqa: E402

built, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
os.makedirs(out, exist_ok=True)
wine.OUT = built
src = os.path.join(wine.HERE, "oleaut32")
gen = os.path.join(built, "gen_oleaut32")
idls = [("tmarshal", 1), ("test_tlb", 2), ("test_reg", 3), ("test_simple", 4)]
cc = wine.CC
objs, rc = [], []
for name, res in idls:
    idl = os.path.join(src, name + ".idl")
    inc = ["-I", os.path.join(wine.HERE, "include"), "-I", src, "-D__WINESRC__"]
    for args, target in ((["-u"], name + "_i.c"), (["-t"], name + ".tlb")):
        r = wine.run([wine.WIDL] + args + inc + ["-o", os.path.join(out, target), idl])
        if r.returncode:
            sys.exit("widl %s: %s" % (name, r.stderr))
    rc.append('%d TYPELIB "%s"' % (res, name + ".tlb"))
    o = os.path.join(out, name + "_i.o")
    r = wine.run([cc] + wine.flags(dll_gen=gen) + ["-include", "stdarg.h", "-c", "-o", o, os.path.join(out, name + "_i.c")])
    if r.returncode:
        sys.exit(r.stderr)
    objs.append(o)
with open(os.path.join(out, "res.rc"), "w") as f:
    f.write("\n".join(rc) + "\n")
r = wine.run([cc.replace("gcc", "windres"), "-O", "coff", "-o", os.path.join(out, "res.o"), os.path.join(out, "res.rc")], cwd=out)
if r.returncode:
    sys.exit(r.stderr)
shim = os.path.join(built, "obj", "shim.o")
libs = wine.available(["-l" + i for i in "oleaut32 ole32 rpcrt4 user32 gdi32 advapi32".split() + wine.COMMON_LIBS])
libs = [l for i, l in enumerate(libs) if l not in libs[:i]] + [os.path.join(built, "libwineguids.a")]
for test in ("typelib", "tmarshal", "vartype", "safearray", "vartest", "dispatch"):
    o = os.path.join(out, test + ".o")
    r = wine.run([cc] + wine.flags(dll_gen=gen) + ["-fmacro-prefix-map=%s/=" % src, "-c", "-o", o, os.path.join(src, test + ".c")])
    if r.returncode:
        sys.exit(r.stderr)
    tl = os.path.join(out, "testlist_%s.c" % test)
    with open(tl, "w") as f:
        f.write('#define STANDALONE\n#include <windef.h>\n#include <winbase.h>\n#include <wincon.h>\n#include "wine/test.h"\nextern void func_%s(void);\n'
                'const struct test winetest_testlist[] = { { "%s", func_%s }, { 0, 0 } };\n' % (test, test, test))
    tlo = tl[:-2] + ".o"
    r = wine.run([cc] + wine.flags(dll_gen=gen) + ["-include", "stdarg.h", "-c", "-o", tlo, tl])
    if r.returncode:
        sys.exit(r.stderr)
    exe = os.path.join(out, "oleaut32_%s.exe" % test)
    r = wine.run([cc, "-o", exe, tlo, o, shim] + objs + [os.path.join(out, "res.o")] + ["-Wl,--gc-sections"] + libs)
    print(exe if r.returncode == 0 else "link failed: " + r.stderr[-1500:])
