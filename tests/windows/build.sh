#!/usr/bin/env bash
# Builds the Windows test programs with the mingw compiler of the OS's toolchain (they run on TheoryCraftOS's kernel32.dll, ntdll.dll and msvcrt.dll; see src/Windows).
#   k32test.exe: freestanding, imports kernel32 only
#   crttest.exe: an ordinary C program with the toolchain's C runtime (the Universal CRT with this mingw; the OS maps its api-ms-win-crt-* imports to msvcrt.dll)
#   ipctest.exe: run twice at the same time ("server"/"client", cmake -DTCOS_USERMODE_DEMO_PAIR=ON): named events, mutexes and semaphores between processes
#   tlscbtest.exe: TLS callbacks for the process and its threads
#   threadtest.exe: CreateThread and friends, critical sections, events, mutexes, TLS (clang)
#   sehtest.exe: __try/__except/__finally and RaiseException (clang, which has structured exception handling in C)
#   imm32test.exe: imm32.dll (input contexts, no IME)
#   user32test.exe: user32.dll windows (classes, messages, the message loop) over win32k
#   shell32test.exe: shell32.dll and ole32.dll (folders, item id lists, file operations, ShellExecute, shortcuts)
# Usage: tests/windows/build.sh [output-dir]   (default: build/windows-tests)
set -euo pipefail
cd "$(dirname "$0")/../.."
out=${1:-build/windows-tests}
mkdir -p "$out"
cc=${CC:-x86_64-w64-mingw32-gcc}
$cc -nostdlib -ffreestanding -fno-builtin -O1 -Wall -Wextra -Wno-unused-parameter -Wl,-e,start -Wl,--subsystem,console -o "$out/k32test.exe" tests/windows/k32test.c -lkernel32
echo "built $out/k32test.exe"
$cc -O1 -Wall -Wextra -o "$out/crttest.exe" tests/windows/crttest.c
echo "built $out/crttest.exe"
$cc -O1 -Wall -Wextra -o "$out/ipctest.exe" tests/windows/ipctest.c
echo "built $out/ipctest.exe (two processes sharing named events, mutexes, semaphores)"
$cc -O1 -Wall -Wextra -o "$out/tlscbtest.exe" tests/windows/tlscbtest.c
echo "built $out/tlscbtest.exe (TLS callbacks)"
$cc -O1 -Wall -Wextra -o "$out/procchild.exe" tests/windows/procchild.c
$cc -O1 -Wall -Wextra -o "$out/processtest.exe" tests/windows/processtest.c
echo "built $out/processtest.exe and procchild.exe (CreateProcess; put procchild.exe in System32: -DTCOS_USERMODE_DEMO_FILES=...)"
$cc -O1 -Wall -Wextra -o "$out/systest.exe" tests/windows/systest.c
echo "built $out/systest.exe (processors, memory, affinity, priorities, times)"
$cc -O1 -Wall -Wextra -shared -o "$out/loadme.dll" tests/windows/loadme.c -Wl,--out-implib,"$out/libloadme.a"
$cc -O1 -Wall -Wextra -shared -o "$out/loaddep.dll" tests/windows/loaddep.c "$out/libloadme.a"
$cc -O1 -Wall -Wextra -shared -o "$out/failinit.dll" tests/windows/failinit.c
$cc -O1 -Wall -Wextra -o "$out/loadtest.exe" tests/windows/loadtest.c
echo "built $out/loadtest.exe (LoadLibrary/FreeLibrary; put loadme.dll loaddep.dll failinit.dll tlsdll.dll in System32: -DTCOS_USERMODE_DEMO_FILES=...)"
$cc -O1 -Wall -Wextra -o "$out/metatest.exe" tests/windows/metatest.c
echo "built $out/metatest.exe (file attributes and times)"
$cc -O1 -Wall -Wextra -o "$out/pipetest.exe" tests/windows/pipetest.c
echo "built $out/pipetest.exe (pipes, handle inheritance, redirected standard handles; procchild.exe in System32)"
$cc -O1 -Wall -Wextra -o "$out/advapitest.exe" tests/windows/advapitest.c -ladvapi32
echo "built $out/advapitest.exe (advapi32: hive files, notification, CryptoAPI, event log, ETW, credentials, services, LSA, audit; it starts itself as a service)"
$cc -O1 -Wall -Wextra -o "$out/dwritetest.exe" tests/windows/dwritetest.c -ldwrite
echo "built $out/dwritetest.exe (dwrite.dll: fonts, layout, analysis; scripts/winetest.sh dwrite own runs it)"
$cc -O1 -Wall -Wextra -o "$out/apctest.exe" tests/windows/apctest.c
echo "built $out/apctest.exe (APCs, alertable waits, waitable timers, OpenProcess; procchild.exe in System32)"
$cc -O1 -Wall -Wextra -o "$out/mmaptest.exe" tests/windows/mmaptest.c
echo "built $out/mmaptest.exe (file mappings; procchild.exe in System32)"
x86_64-w64-mingw32-windres tests/windows/batchtest.rc -O coff -o "$out/batchtest_rc.o"
$cc -O1 -Wall -Wextra -Wno-unused-parameter -o "$out/batchtest.exe" tests/windows/batchtest.c "$out/batchtest_rc.o" -lpsapi -lsynchronization -lmincore
echo "built $out/batchtest.exe (WaitOnAddress, resources, VirtualAlloc2, settings, packages, aliases, file names, backup streams, namespaces, durations)"
$cc -O1 -Wall -Wextra -o "$out/extratest.exe" tests/windows/extratest.c
echo "built $out/extratest.exe (fibers, thread pool, timer queues, volumes, ini files, small things)"
$cc -O1 -Wall -Wextra -o "$out/nlstest.exe" tests/windows/nlstest.c
echo "built $out/nlstest.exe (locale, string functions, atoms, completion ports, versions)"
$cc -O1 -Wall -Wextra -o "$out/consoletest.exe" tests/windows/consoletest.c
echo "built $out/consoletest.exe (screen buffers, cursor, modes, ANSI sequences, key events)"
$cc -O1 -Wall -Wextra -o "$out/jobtest.exe" tests/windows/jobtest.c
echo "built $out/jobtest.exe (job objects; procchild.exe in System32)"
$cc -O1 -Wall -Wextra -o "$out/toolhelptest.exe" tests/windows/toolhelptest.c
echo "built $out/toolhelptest.exe (Toolhelp32 lists, HeapWalk, psapi; procchild.exe in System32)"
$cc -O1 -Wall -Wextra -o "$out/remotetest.exe" tests/windows/remotetest.c
echo "built $out/remotetest.exe (memory and threads of another process; procchild.exe in System32)"
$cc -O1 -Wall -Wextra -o "$out/linktest.exe" tests/windows/linktest.c
echo "built $out/linktest.exe (hard and symbolic links)"
$cc -O1 -Wall -Wextra -o "$out/priotest.exe" tests/windows/priotest.c
echo "built $out/priotest.exe (thread and process priority affects scheduling)"
$cc -O1 -Wall -Wextra -o "$out/regtest.exe" tests/windows/regtest.c -ladvapi32
echo "built $out/regtest.exe (the registry, over advapi32)"
$cc -O1 -Wall -Wextra -o "$out/bcrypttest.exe" tests/windows/bcrypttest.c -lbcrypt
echo "built $out/bcrypttest.exe (bcrypt: hashes, KDFs, AES, RC4, ECDH/ECDSA, key blobs)"
$cc -O1 -Wall -Wextra -o "$out/locktest.exe" tests/windows/locktest.c
echo "built $out/locktest.exe (byte-range locks; procchild.exe in System32)"
$cc -O1 -Wall -Wextra -o "$out/notifytest.exe" tests/windows/notifytest.c
echo "built $out/notifytest.exe (FindFirstChangeNotification/ReadDirectoryChangesW's NtCreateChangeNotification)"
$cc -O1 -Wall -Wextra -o "$out/mailslottest.exe" tests/windows/mailslottest.c
echo "built $out/mailslottest.exe (CreateMailslot/GetMailslotInfo/SetMailslotInfo)"
$cc -O1 -Wall -Wextra -o "$out/socktest.exe" tests/windows/socktest.c -lws2_32
echo "built $out/socktest.exe (ws2_32: TCP/UDP loopback, non-blocking connect, select)"
$cc -O1 -Wall -Wextra -o "$out/imm32test.exe" tests/windows/imm32test.c -limm32
echo "built $out/imm32test.exe (imm32.dll without an IME: input contexts, their status, HIMCC blocks, composition string / candidate list / guide line getters)"
$cc -O1 -Wall -Wextra -o "$out/user32test.exe" tests/windows/user32test.c -luser32 -lgdi32
echo "built $out/user32test.exe (user32 windows over win32k: classes, creation messages, tree, text, posted/sent messages, loop, timers, enumeration)"
$cc -O1 -Wall -Wextra -Wno-unused-parameter -o "$out/shell32test.exe" tests/windows/shell32test.c -lshell32 -lole32 -lshlwapi -ladvapi32 -luuid
echo "built $out/shell32test.exe (shell32 and ole32: command lines, special folders, item id lists, shell items, file operations, ShellExecute, shortcuts; procchild.exe in System32)"
x86_64-w64-mingw32-windres tests/windows/chromedlltest.rc -O coff -o "$out/chromedlltest_rc.o"
$cc -O1 -Wall -Wextra -o "$out/chromedlltest.exe" tests/windows/chromedlltest.c "$out/chromedlltest_rc.o" -lversion -lruntimeobject -lnetapi32 -lwinspool -lbcryptprimitives -luuid
echo "built $out/chromedlltest.exe (Chrome's startup libraries: ProcessPrng, version resources, HSTRINGs, netapi32, winspool, interrupt time, imports by ordinal)"
$cc -O1 -Wall -o "$out/ws2eventtest.exe" tests/windows/ws2eventtest.c -lws2_32
echo "built $out/ws2eventtest.exe (ws2_32: WSA events, WSAEventSelect, overlapped WSARecv/WSASend, WSAIoctl, protocol catalogue, GetAddrInfoExW)"
$cc -O1 -Wall -o "$out/crypt32test.exe" tests/windows/crypt32test.c -lcrypt32 -lsecur32
echo "built $out/crypt32test.exe (crypt32: DPAPI round trip, CryptProtectMemory, certificate stores; secur32 GetUserNameExW)"
$cc -O1 -Wall -o "$out/shmregiontest.exe" tests/windows/shmregiontest.c
echo "built $out/shmregiontest.exe (sections as Chromium hands them on: NtQuerySection, DuplicateHandle through a process handle, read-only handles, 64 KiB-aligned views)"
x86_64-w64-mingw32-windres tests/windows/user32misctest.rc -O coff -o "$out/user32misctest_rc.o"
$cc -O1 -Wall -o "$out/user32misctest.exe" tests/windows/user32misctest.c "$out/user32misctest_rc.o" -luser32 -lgdi32
echo "built $out/user32misctest.exe (user32: rectangles, LoadString, window stations and desktops, notifications)"
$cc -O1 -Wall -o "$out/winhttptest.exe" tests/windows/winhttptest.c -lwinhttp
echo "built $out/winhttptest.exe (winhttp: WinHttpCrackUrl, no proxy, handles)"
$cc -O0 -Wall -Wextra -o "$out/suspendtest.exe" tests/windows/suspendtest.c
echo "built $out/suspendtest.exe (SuspendThread, thread contexts, TerminateThread)"
if command -v clang >/dev/null; then
  clang --target=x86_64-w64-mingw32 -fms-extensions -O1 -Wall -I/usr/x86_64-w64-mingw32/include -L/usr/x86_64-w64-mingw32/lib -fuse-ld=bfd -o "$out/sehtest.exe" tests/windows/sehtest.c
  echo "built $out/sehtest.exe (SEH: __try/__except/__finally, clang)"
  clang --target=x86_64-w64-mingw32 -fms-extensions -O1 -Wall -Wno-unused-function -I/usr/x86_64-w64-mingw32/include -L/usr/x86_64-w64-mingw32/lib -fuse-ld=bfd -o "$out/threadtest.exe" tests/windows/threadtest.c
  clang --target=x86_64-w64-mingw32 -fms-extensions -O1 -I/usr/x86_64-w64-mingw32/include -L/usr/x86_64-w64-mingw32/lib -fuse-ld=bfd -shared -o "$out/tlsdll.dll" tests/windows/tlsdll.c -Wl,--out-implib,"$out/libtlsdll.a"
  clang --target=x86_64-w64-mingw32 -fms-extensions -O1 -I/usr/x86_64-w64-mingw32/include -L/usr/x86_64-w64-mingw32/lib -fuse-ld=bfd -o "$out/tlsdlltest.exe" tests/windows/tlsdlltest.c "$out/libtlsdll.a"
  echo "built $out/tlsdlltest.exe and tlsdll.dll (a DLL's static TLS; put tlsdll.dll next to it: -DTCOS_USERMODE_DEMO_FILES=...)"
  echo "built $out/threadtest.exe (threads, clang for __declspec(thread))"
  clang --target=x86_64-w64-mingw32 -fms-extensions -O1 -Wall -I/usr/x86_64-w64-mingw32/include -L/usr/x86_64-w64-mingw32/lib -fuse-ld=bfd -o "$out/smptest.exe" tests/windows/smptest.c
  echo "built $out/smptest.exe (threads on different cores, page tables shared between cores)"
fi
${CXX:-x86_64-w64-mingw32-g++} -std=c++17 -O1 -Wall -static -static-libgcc -static-libstdc++ -Wno-unused-result -o "$out/cxxtest.exe" tests/windows/cxxtest.cpp 2>/dev/null && echo "built $out/cxxtest.exe (C++ exceptions, libstdc++ and libgcc linked statically)" || echo "(no C++ build: $(basename "${CXX:-x86_64-w64-mingw32-g++}") failed)"
$cc -O1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -o "$out/vulkantest.exe" tests/windows/vulkantest.c
echo "built $out/vulkantest.exe (vulkan-1.dll: the physical devices and a render on each; scripts/winetest.sh does not run it, see docs/WindowsVulkan.md)"
