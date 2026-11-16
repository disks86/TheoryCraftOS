/*
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

/* A 32-bit Windows program that uses the libraries a 32-bit program finds beyond the window system, for tests/wow64_qemu.sh (docs/Wow64.md): each of
 * them has something of its own that depends on the width of a pointer - a calling convention, a structure the program fills in, a request the
 * kernel reads. Winsock (ws2_32), the common dialogs' checks of their structures (comdlg32), the multimedia timer and the wave devices (winmm), the
 * input method manager (imm32), a walk of the program's own stack (dbghelp), hashing and random numbers (bcrypt), and what ntdll says of an object's name. Ends with exit code 42 if every
 * check passed.
 *   i686-w64-mingw32-gcc -O1 -fno-omit-frame-pointer -o lib32.exe tests/windows/lib32.c -lws2_32 -lcomdlg32 -lwinmm -limm32 -ldbghelp -lbcrypt -luser32 -lntdll */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <imm.h>
#include <dbghelp.h>
#include <bcrypt.h>

static int g_passed, g_failed;
static void check(int ok, const char *what) {
  printf(ok ? "lib32: ok: %s\n" : "lib32: FAILED: %s\n", what);
  fflush(stdout);
  if (ok) ++g_passed; else ++g_failed;
}

/* The stack pointer, to see that a call left it where it was (a function with the wrong calling convention does not). */
static __attribute__((noinline)) unsigned stack_now(void) { return (unsigned)(ULONG_PTR)__builtin_frame_address(0); }

/* ---- Winsock ---- */
static void sockets(void) {
  WSADATA data;
  memset(&data, 0xEE, sizeof(data));
  const unsigned before = stack_now();
  const int started = WSAStartup(MAKEWORD(2, 2), &data);
  check(stack_now() == before, "WSAStartup takes its arguments off the stack (stdcall)");
  check(started == 0 && data.wVersion == MAKEWORD(2, 2), "WSAStartup: version 2.2");
  check(memchr(data.szDescription, 0, sizeof(data.szDescription)) != NULL && memchr(data.szSystemStatus, 0, sizeof(data.szSystemStatus)) != NULL &&
        (unsigned char)data.szDescription[0] != 0xEE, "WSADATA: the description and status are text where a 32-bit program reads them");
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  check(s != INVALID_SOCKET, "socket(AF_INET, SOCK_DGRAM)");
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(40123);
  check(bind(s, (struct sockaddr *)&address, sizeof(address)) == 0, "bind");
  struct sockaddr_in bound;
  int length = sizeof(bound);
  memset(&bound, 0, sizeof(bound));
  check(getsockname(s, (struct sockaddr *)&bound, &length) == 0 && bound.sin_family == AF_INET && ntohs(bound.sin_port) == 40123, "getsockname: the port it was bound to");
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(s, &readable);
  struct timeval none = {0, 0};
  check(select(0, &readable, NULL, NULL, &none) == 0 && !FD_ISSET(s, &readable), "select with no time to wait: nothing to read");
  u_long nonblocking = 1;
  char byte;
  check(ioctlsocket(s, FIONBIO, &nonblocking) == 0 && recv(s, &byte, 1, 0) == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK,
        "a non-blocking recv with nothing there: WSAEWOULDBLOCK");
  struct addrinfo hints, *found = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  const int resolved = getaddrinfo("127.0.0.1", "80", &hints, &found);
  check(resolved == 0 && found && found->ai_family == AF_INET && found->ai_addrlen == sizeof(struct sockaddr_in) &&
        ((struct sockaddr_in *)found->ai_addr)->sin_addr.s_addr == htonl(0x7F000001) && ((struct sockaddr_in *)found->ai_addr)->sin_port == htons(80),
        "getaddrinfo of a numeric address: an addrinfo a 32-bit program can read");
  if (found) freeaddrinfo(found);
  check(closesocket(s) == 0, "closesocket");
  check(stack_now() == before, "the stack is where it was after the Winsock calls");
  WSACleanup();
}

/* ---- the common dialogs: the structures are checked before anything is shown ---- */
static void dialogs(void) {
  OPENFILENAMEW ofn;
  memset(&ofn, 0, sizeof(ofn));
  ofn.lStructSize = 152;      /* (the 64-bit structure's size) */
  check(!GetOpenFileNameW(&ofn) && CommDlgExtendedError() == CDERR_STRUCTSIZE, "GetOpenFileName refuses the 64-bit structure's size");
  ofn.lStructSize = sizeof(ofn);
  check(sizeof(ofn) == 88, "OPENFILENAME is 88 bytes in a 32-bit program");
  ofn.lpstrFile = NULL;
  ofn.nMaxFile = 0;
  ofn.Flags = OFN_ENABLEHOOK;      /* (a hook that is not there: refused after the size was accepted, before a dialog is shown) */
  const BOOL opened = GetOpenFileNameW(&ofn);
  const DWORD open_error = CommDlgExtendedError();
  if (opened || open_error != CDERR_NOHOOK) printf("lib32: (GetOpenFileName answered %d, error 0x%lX)\n", (int)opened, (unsigned long)open_error);
  check(!opened && open_error == CDERR_NOHOOK, "GetOpenFileName accepts the 32-bit structure's size");
  CHOOSECOLORW colour;
  memset(&colour, 0, sizeof(colour));
  colour.lStructSize = sizeof(colour);
  check(sizeof(colour) == 36 && !ChooseColorW(&colour) && CommDlgExtendedError() == CDERR_INITIALIZATION, "ChooseColor accepts its 36 bytes and misses the custom colours");
  CHOOSEFONTW font;
  memset(&font, 0, sizeof(font));
  font.lStructSize = sizeof(font) + 4;
  check(!ChooseFontW(&font) && CommDlgExtendedError() == CDERR_STRUCTSIZE, "ChooseFont refuses a wrong size");
  PRINTDLGEXW print;
  memset(&print, 0, sizeof(print));
  print.lStructSize = sizeof(print);
  print.hwndOwner = GetDesktopWindow();
  print.Flags = PD_NOPAGENUMS;
  print.nStartPage = START_PAGE_GENERAL;
  print.dwResultAction = 77;
  const HRESULT printed = PrintDlgExW(&print);
  check(sizeof(print) == 84 && printed == E_FAIL && print.dwResultAction == 0, "PrintDlgEx reads the 84-byte structure: no printer, nothing chosen");
}

/* ---- winmm ---- */
static volatile LONG g_ticks;
static DWORD_PTR g_tick_user;
static void CALLBACK tick(UINT id, UINT message, DWORD_PTR user, DWORD_PTR one, DWORD_PTR two) {
  (void)id, (void)message, (void)one, (void)two;
  g_tick_user = user;
  InterlockedIncrement(&g_ticks);
}
static void multimedia(void) {
  const DWORD t0 = timeGetTime();
  Sleep(30);
  const DWORD t1 = timeGetTime();
  check(t1 - t0 >= 20 && t1 - t0 < 2000, "timeGetTime moves with Sleep");
  const MMRESULT event = timeSetEvent(10, 1, tick, 0x1234ABCD, TIME_PERIODIC | TIME_CALLBACK_FUNCTION);
  check(event != 0, "timeSetEvent");
  for (int i = 0; i < 100 && g_ticks < 3; ++i) Sleep(10);
  check(g_ticks >= 3 && g_tick_user == 0x1234ABCD, "a periodic multimedia timer calls back with its user value");
  check(timeKillEvent(event) == TIMERR_NOERROR, "timeKillEvent");
  const LONG at = g_ticks;
  Sleep(60);
  check(g_ticks <= at + 1, "a killed timer stops");
  const UINT outputs = waveOutGetNumDevs();
  WAVEOUTCAPSW caps;
  memset(&caps, 0, sizeof(caps));
  check(outputs <= 1 && (outputs == 0 || waveOutGetDevCapsW(0, &caps, sizeof(caps)) == MMSYSERR_NOERROR), "waveOutGetNumDevs, and the device's capabilities when there is one");
  if (outputs) {
    WAVEFORMATEX format = {WAVE_FORMAT_PCM, 2, 44100, 44100 * 4, 4, 16, 0};
    HWAVEOUT out = NULL;
    const MMRESULT opened = waveOutOpen(&out, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
    static short samples[4410 * 2];
    WAVEHDR header;
    memset(&header, 0, sizeof(header));
    header.lpData = (LPSTR)samples;
    header.dwBufferLength = sizeof(samples);
    int played = opened == MMSYSERR_NOERROR && waveOutPrepareHeader(out, &header, sizeof(header)) == MMSYSERR_NOERROR &&
                 waveOutWrite(out, &header, sizeof(header)) == MMSYSERR_NOERROR;
    for (int i = 0; played && i < 300 && !(header.dwFlags & WHDR_DONE); ++i) Sleep(10);
    played = played && (header.dwFlags & WHDR_DONE) != 0;
    if (out) { waveOutReset(out); waveOutUnprepareHeader(out, &header, sizeof(header)); waveOutClose(out); }
    check(played, "waveOut plays a tenth of a second of silence to its end");
  }
}

/* ---- imm32 ---- */
static void input_methods(void) {
  const HKL layout = GetKeyboardLayout(0);
  check(layout != NULL && !ImmIsIME(layout), "the keyboard layout is not an input method");
  wchar_t text[16];
  check(ImmGetDescriptionW(layout, text, 16) == 0, "it has no description as one");
  const HIMC context = ImmCreateContext();
  check(context != NULL, "ImmCreateContext");
  DWORD conversion = 0xFFFF, sentence = 0xFFFF;
  check(context && ImmGetConversionStatus(context, &conversion, &sentence) && conversion != 0xFFFF, "ImmGetConversionStatus of the new context");
  check(context && ImmSetOpenStatus(context, TRUE) && ImmGetOpenStatus(context), "ImmSetOpenStatus is read back");
  check(context && ImmDestroyContext(context), "ImmDestroyContext");
}

/* ---- dbghelp: the program's own stack ---- */
/* (RtlCaptureContext describes its caller's caller in a 32-bit program, as on Windows: the context is that of walk_here's return into walk_middle) */
static __attribute__((noinline)) int walk_here(DWORD *addresses, int capacity) {
  CONTEXT context;
  RtlCaptureContext(&context);
  STACKFRAME64 frame;
  memset(&frame, 0, sizeof(frame));
  frame.AddrPC.Offset = context.Eip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Ebp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Esp;
  frame.AddrStack.Mode = AddrModeFlat;
  int n = 0;
  while (n < capacity && StackWalk64(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), GetCurrentThread(), &frame, &context, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
    addresses[n++] = (DWORD)frame.AddrPC.Offset;
  return n;
}
static __attribute__((noinline)) int walk_middle(DWORD *addresses, int capacity) {
  const int n = walk_here(addresses, capacity);
  return n + (addresses[0] == 0xFFFFFFFFu);      /* (something after the call, so that it is a call and not a jump) */
}
static __attribute__((noinline)) int walk_outer(DWORD *addresses, int capacity) {
  const int n = walk_middle(addresses, capacity);
  return n + (addresses[1] == 0xFFFFFFFFu);
}
static void stack(void) {
  check(SymInitialize(GetCurrentProcess(), NULL, FALSE), "SymInitialize");
  DWORD addresses[16];
  memset(addresses, 0, sizeof(addresses));
  const int n = walk_outer(addresses, 16);
  const DWORD middle = (DWORD)(ULONG_PTR)walk_middle, outer = (DWORD)(ULONG_PTR)walk_outer, top = (DWORD)(ULONG_PTR)stack;
  const int in_order = n >= 3 && addresses[0] - middle < 0x100 && addresses[1] - outer < 0x100 && addresses[2] - top < 0x400;
  if (!in_order)
    printf("lib32: (%d frames: %08lX %08lX %08lX %08lX; the functions are at %08lX %08lX %08lX)\n", n, (unsigned long)addresses[0], (unsigned long)addresses[1],
           (unsigned long)addresses[2], (unsigned long)addresses[3], (unsigned long)middle, (unsigned long)outer, (unsigned long)top);
  check(n >= 3, "StackWalk64 finds three frames or more");
  check(in_order, "the frames are the functions that were called, innermost first");
  CONTEXT wrong;
  STACKFRAME64 frame;
  memset(&wrong, 0, sizeof(wrong));
  memset(&frame, 0, sizeof(frame));
  check(!StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, &wrong, NULL, NULL, NULL, NULL), "a walk for another machine is refused");
  SymCleanup(GetCurrentProcess());
}

/* ---- bcrypt ---- */
static void crypto(void) {
  unsigned char random[32];
  memset(random, 0, sizeof(random));
  int nonzero = 0;
  const NTSTATUS filled = BCryptGenRandom(NULL, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  for (unsigned i = 0; i < sizeof(random); ++i) nonzero += random[i] != 0;
  check(filled == 0 && nonzero > 8, "BCryptGenRandom fills the buffer");
  BCRYPT_ALG_HANDLE algorithm = NULL;
  BCRYPT_HASH_HANDLE hash = NULL;
  unsigned char digest[32];
  memset(digest, 0, sizeof(digest));
  static const unsigned char expected[4] = {0xBA, 0x78, 0x16, 0xBF};      /* SHA-256("abc") begins so */
  const int hashed = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 && BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) == 0 &&
                     BCryptHashData(hash, (PUCHAR)"abc", 3, 0) == 0 && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
  check(hashed && memcmp(digest, expected, 4) == 0, "SHA-256 of \"abc\" through an algorithm provider and a hash object");
  if (hash) BCryptDestroyHash(hash);
  if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
}

/* ---- ntdll: an object's name and type, structures with a pointer in them ---- */
NTSTATUS NTAPI NtQueryObject(HANDLE, ULONG, PVOID, ULONG, PULONG);
typedef struct { USHORT Length, MaximumLength; WCHAR *Buffer; } NAME_STRING;
static void objects(void) {
  const HANDLE event = CreateEventW(NULL, TRUE, FALSE, L"lib32-event");
  union { NAME_STRING name; WCHAR space[200]; } info;
  ULONG returned = 0;
  memset(&info, 0xEE, sizeof(info));
  const NTSTATUS named = event ? NtQueryObject(event, 1 /* ObjectNameInformation */, &info, sizeof(info), &returned) : -1;
  const unsigned units = named == 0 ? info.name.Length / 2 : 0;
  check(named == 0 && units > 11 && info.name.Buffer == (WCHAR *)((char *)&info + sizeof(NAME_STRING)) && returned == sizeof(NAME_STRING) + info.name.Length + 2 &&
        memcmp(info.name.Buffer + units - 11, L"lib32-event", 22) == 0, "NtQueryObject: the name of an event, the characters right after an 8-byte string");
  memset(&info, 0xEE, sizeof(info));
  const NTSTATUS typed = event ? NtQueryObject(event, 2 /* ObjectTypeInformation */, &info, sizeof(info), &returned) : -1;
  check(typed == 0 && info.name.Length == 10 && info.name.Buffer == (WCHAR *)((char *)&info + 96) && memcmp(info.name.Buffer, L"Event", 10) == 0,
        "NtQueryObject: its type is \"Event\", after a structure of 96 bytes");
  ULONG need = 0;
  check(event && NtQueryObject(event, 1, &info, 4, &need) == (NTSTATUS)0xC0000004 && need == sizeof(NAME_STRING) + 2 * units + 2,
        "NtQueryObject with too small a buffer says how many bytes a 32-bit program needs");
  if (event) CloseHandle(event);
}

int main(void) {
  objects();
  sockets();
  dialogs();
  multimedia();
  input_methods();
  stack();
  crypto();
  if (g_failed == 0) printf("lib32: all %d checks passed\n", g_passed);
  else printf("lib32: %d of %d checks FAILED\n", g_failed, g_passed + g_failed);
  fflush(stdout);
  return g_failed == 0 ? 42 : 1;
}
