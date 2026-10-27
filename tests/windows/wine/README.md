# Wine's conformance tests

The user-mode conformance tests of the [Wine](https://www.winehq.org/) project (`dlls/<dll>/tests`), for the DLLs this OS has files for (`src/Windows`). They check the
behaviour that real Windows has, which is what Wine (and this OS) must match. Almost none of them pass yet: most DLLs are stubs (see `docs/WindowsUserMode.md`), so the
list of failing tests is the to-do list for the implementation.

- **Source:** Wine commit `f343ca6085bfac807c33e9c1a8a43105071f1416` (2026-10-26), unmodified. The files keep their copyright notices; Wine is licensed under the
  GNU LGPL 2.1 or later (`COPYING.LIB`), which allows its use in this program under the AGPL. `include/` is Wine's header directory (`wine/test.h`, `winternl.h`, the
  IDL files of the COM interfaces, ...); the tests are written against it, not against mingw-w64's headers.
- **Layout:** `<dll>/*.c` are the tests (and the helper headers/IDL files next to them), `<dll>/imports.txt` the import libraries Wine links them with, `shim.c` the
  debug-channel functions that Wine's `ntdll.dll` provides and this OS's does not.
- **Build:** `tests/windows/wine.py build <output directory> [dll ...]` makes one program per test, `<dll>_<test>.exe` (the test is the name in `START_TEST(...)`),
  with the OS's mingw compiler, plus `manifest.json` with what did not build. It needs `widl` (part of mingw-w64) to make the COM headers. Each program runs its test
  and prints Wine's usual output (`file.c:line: Test failed: ...`, `Test marked todo: ...`, `skipping ...`) to its standard output.
- **Run:** like any other Windows program of the OS (the programs import `ntdll`, `kernel32`, `user32`, `advapi32`, ... by name, and the stub DLLs provide every name).
  A test that needs an export that is only a stub fails its checks; it does not stop the program.
- **Update:** `tests/windows/wine.py import <Wine checkout>` copies the tests again, keeping the files that compile.

## State

343 programs build (kernel32 33, comctl32 29, ntdll 28, user32 23, shell32 19, crypt32 15, gdi32 14, advapi32 12, gdiplus 12, ole32 11, shlwapi 11, winmm 10, ... 60 DLLs).
26 tests are imported but do not link here, because they need something mingw-w64 lacks: the import libraries of `kernelbase`, `win32u`, `winspool.drv` and `d3d10_1`,
the private entry points `__STRINGTOLD`, `GetDiskInfoA` and `D3D10CreateDevice1`, or GUIDs and helpers that Wine's build makes from IDL files of its own tree (msi, dsound,
oleaut32 `tmarshal`, rpcrt4 `cstub`, ole32 `CLSID_ManualResetEvent`, ...). Wine tests of DLLs this OS has no file for (windowscodecs, jscript, vbscript, ...) are not imported.
