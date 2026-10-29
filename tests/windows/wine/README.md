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

## Running them, and where the implementations stand

`scripts/winetest.sh <dll> [test ...]` builds the tests of a DLL, boots the OS headless under KVM with `tests/windows/winerun.exe` as the program, runs each test as a process
of its own and prints the failed checks per test (a test that stops the machine is recorded as HUNG and the rest go on). The debug console is slow, so each test stops
printing after 30 failures: the number is a lower bound. `WINETEST_ENV=WINETEST_DEBUG=2` or `scripts/winetest.sh ntdll "rtl@VAR=value"` set environment variables for a test.
`scripts/gen-test-stubs.py` adds the names the tests look up with GetProcAddress to the stub lists; `scripts/unstub.py` removes the stubs a source file now defines.

Checks that failed (Wine commit f343ca6), before the implementation of the library started and now:

| DLL / test | before | now |
|---|---|---|
| ntdll rtlbitmap | 2 | 0 |
| ntdll large_int | 432 | 5 (the NUL after a number that fills MaximumLength exactly) |
| ntdll rtlstr | 8+, hung | 4 + RtlIsTextUnicode (not done) |
| ntdll error (RtlNtStatusToDosError) | 633 | 155 |
| ntdll string (C library in ntdll) | crash | 14 |
| shlwapi string | 16751 | about 150 (needs ordinal exports and StrRetTo*/StrFromTimeInterval) |
| lz32 lzexpand | 117 | 17 |
| kernel32 format_msg | 591 | 270 |
| kernel32 time | 786 | 1 |
| kernel32 locale | 150 (stopped early) | about 4600, runs to the end (en-US and the invariant locale only; no normalization, locale enumeration, geo info or code pages) |
| ntdll reg (NT registry services) | 133 | about 90 (needs an in-kernel registry: done; registry links, RtlQueryRegistryValues not) |
| advapi32 registry | not run | 166, runs to the end |
| kernel32 atom | 9422 (hung) | 0 (needed user32 wsprintf, now in src/Windows/user32) |
| kernel32 virtual | 10983 | 0 (passes; the child-process phase hung or lost the child once in four runs) |
| kernel32 heap | 3463 (hung) | about 285 before test_heap_sizes (Global/Local memory done; HeapWalk regions, HeapQueryInformation, heap flags/debug heap and Windows' internal sub-heap sizes are not) |
| kernel32 file | 2754 | the first 2,400 lines of the test (create/delete/read-only/copy/overlapped/share modes) pass apart from mapped-file cases; FindFirstFile wildcards now match all 88 rows of Wine's table (src/Process/DosWildcard.cpp); LockFile/LockFileEx pass; case-insensitive names, DOS device names, the drive root and GetFileType now pass; still failing: handle access of the reopen tests, mapped-file rules (ERROR_USER_MAPPED_FILE). Counts vary from run to run: the run stops printing (the console is slow) and often hits the 120 s limit |
| kernel32 loader | 16957 | 2 (was 31): exclusive data-file loads hold the file (share modes), a library is known by its file across MoveFile and CreateHardLink, a failed image mapping gives its pages back, LdrLoadDll's search path stops at bad headers / failed DllMain and falls through for images this process cannot run, 32-bit data-only libraries map, ImageContainsCode follows Windows' rule, non-dynamic-base image views are not relocated. Still failing: loader.c:3010 (a mapped-only library stays loaded after its FreeLibrary in the export-forwarder chain test) and loader.c:3293 (RtlDllShutdownInProgress in phase 7: Wine marks it todo). The ExitProcess child phases and kernel32 virtual's child-process phase occasionally fail (a child crashes at start; page tables rebuilt by another thread are now recovered from, the rest is not understood) |
| kernel32 module | ~3500 | 0 (was 50 with a crash): LOAD_LIBRARY_SEARCH_* flags, AddDllDirectory / SetDefaultDllDirectories / SetDllDirectory (ntdll keeps the folders), LdrLoadDll with coded search flags and relative names, LdrGetDllPath, LdrGetDllHandle(Ex), LdrGetDllFullName, API set presence (small built-in table), GetModuleHandleEx rules, authz and rasapi32 stub libraries. The test programs now run from C:\\Windows\\Tests (winerun.c, UserModeDemo.cpp), cwd C:\\Windows\\Tests, so System32 is not the application folder |
| kernel32 environ / module / file | crashed or hung | run to the end |

Everything else was measured once and has not been worked on: ntdll alpc 1684, directory 784, file 354, sync 246, threadpool 244, reg 133 (the NT registry services), path 279;
comm 763, version 92 (needs manifest-based version emulation); debugger, sync and thread tests hang or crash the machine; shlwapi url/istream/shreg/assoc; mpr 211, psapi 123, userenv 68, secur32 53. Most of those are
missing system services (the NT registry, object namespace, ALPC, thread pools, `NtQuerySystemInformation` classes) rather than missing library code.
