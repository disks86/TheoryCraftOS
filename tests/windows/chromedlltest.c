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

/* What Chrome needs at startup from the small system libraries: ProcessPrng (bcryptprimitives.dll), the version resource of its own file (version.dll), HSTRINGs and
 * the Ro* calls (combase.dll, linked through the api-ms-win-core-winrt-* names that the loader folds), the join state (netapi32.dll), the spooler with no printers
 * (winspool.drv, folded to winspool.dll), CompareObjectHandles and the interrupt-time clocks (kernel32), and the exports Chrome imports by ordinal (shell32 #2, #4,
 * #680, d3d12 #101, winspool #203). Build:
 *   x86_64-w64-mingw32-windres tests/windows/chromedlltest.rc -O coff -o chromedlltest_rc.o
 *   x86_64-w64-mingw32-gcc -O1 -o chromedlltest.exe tests/windows/chromedlltest.c chromedlltest_rc.o -lversion -lruntimeobject -lnetapi32 -lwinspool -lbcryptprimitives -luuid -lws2_32 -ladvapi32
 * Prints "chromedlltest: N checks, M failed"; the exit code is the number of failures. */
#define COBJMACROS
#include <winsock2.h>      /* (before windows.h) */
#include <ws2tcpip.h>
#include <windows.h>
#include <winstring.h>
#include <roapi.h>
#include <lm.h>
#include <dsrole.h>
#include <winspool.h>
#include <nspapi.h>
#include <evntprov.h>
#include <stdio.h>
#include <string.h>

BOOL WINAPI ProcessPrng(PBYTE data, SIZE_T size);

static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s (error %lu)\n", what, GetLastError());
}

static void test_prng(void) {
  BYTE a[64], b[64];
  memset(a, 0, sizeof a);
  memset(b, 0, sizeof b);
  check(ProcessPrng(a, sizeof a) == TRUE, "ProcessPrng");
  check(ProcessPrng(b, sizeof b) == TRUE, "ProcessPrng again");
  check(memcmp(a, b, sizeof a) != 0, "ProcessPrng gives new bytes");
  int zeros = 0;
  for (int i = 0; i < 64; ++i) zeros += a[i] == 0;
  check(zeros < 16, "ProcessPrng bytes look random");
  check(ProcessPrng(NULL, 0) == TRUE, "ProcessPrng of nothing");
  HMODULE m = LoadLibraryW(L"bcryptprimitives.dll");      /* (what Chrome's base::RandBytes does) */
  check(m && GetProcAddress(m, "ProcessPrng"), "LoadLibrary bcryptprimitives + ProcessPrng");
}

static void test_version(void) {
  WCHAR self[MAX_PATH];
  GetModuleFileNameW(NULL, self, MAX_PATH);
  DWORD handle = 7;
  const DWORD size = GetFileVersionInfoSizeW(self, &handle);
  check(size > 0 && handle == 0, "GetFileVersionInfoSizeW of the program");
  static BYTE block[8192];
  check(size <= sizeof block && GetFileVersionInfoW(self, 0, size, block), "GetFileVersionInfoW");
  VS_FIXEDFILEINFO *fixed = NULL;
  UINT length = 0;
  check(VerQueryValueW(block, L"\\", (void **)&fixed, &length) && length == sizeof *fixed, "VerQueryValueW root");
  check(fixed && fixed->dwSignature == 0xFEEF04BD && fixed->dwFileVersionMS == 0x00010002 && fixed->dwFileVersionLS == 0x00030004 &&
            fixed->dwProductVersionMS == 0x00050006 && fixed->dwProductVersionLS == 0x00070008, "VS_FIXEDFILEINFO values");
  DWORD *translation = NULL;
  check(VerQueryValueW(block, L"\\VarFileInfo\\Translation", (void **)&translation, &length) && length == 4 && translation && *translation == 0x04B00409,
        "Translation");
  WCHAR *text = NULL;
  check(VerQueryValueW(block, L"\\StringFileInfo\\040904b0\\ProductVersion", (void **)&text, &length) && text && wcscmp(text, L"5.6.7.8") == 0 && length == 8,
        "ProductVersion (W)");
  check(VerQueryValueW(block, L"\\stringfileinfo\\040904B0\\companyname", (void **)&text, &length) && text && wcscmp(text, L"TheoryCraftOS Test") == 0,
        "CompanyName, keys case-insensitive");
  char *ansi = NULL;
  check(VerQueryValueA(block, "\\StringFileInfo\\040904b0\\ProductVersion", (void **)&ansi, &length) && ansi && strcmp(ansi, "5.6.7.8") == 0, "ProductVersion (A)");
  check(!VerQueryValueW(block, L"\\StringFileInfo\\040904b0\\NoSuchValue", (void **)&text, &length), "a missing value");
  check(!VerQueryValueW(block, L"\\NoSuchBlock", (void **)&text, &length), "a missing block");
  char self_a[MAX_PATH];
  GetModuleFileNameA(NULL, self_a, MAX_PATH);
  check(GetFileVersionInfoSizeA(self_a, NULL) == size, "GetFileVersionInfoSizeA");
  /* A cut block stays inside its bounds. */
  static BYTE small[64];
  check(GetFileVersionInfoW(self, 0, sizeof small, small), "GetFileVersionInfoW into a small buffer");
  check(*(WORD *)small <= sizeof small, "a cut block says its new length");
  VerQueryValueW(small, L"\\StringFileInfo\\040904b0\\ProductVersion", (void **)&text, &length);
  /* Files without a version resource, and no file. */
  SetLastError(0);
  WCHAR k32[MAX_PATH];
  GetSystemDirectoryW(k32, MAX_PATH);
  wcscat(k32, L"\\ntdll.dll");
  check(GetFileVersionInfoSizeW(k32, NULL) == 0 || GetLastError() == 0, "a DLL with no version resource (or with one)");
  SetLastError(0);
  check(GetFileVersionInfoSizeW(L"C:\\no\\such\\file.dll", NULL) == 0 && GetLastError() != 0, "a file that does not exist");
}

static void test_hstring(void) {
  HSTRING a = NULL, b = NULL, c = NULL;
  check(WindowsCreateString(L"hello", 5, &a) == S_OK && a, "WindowsCreateString");
  UINT32 n = 0;
  const WCHAR *raw = WindowsGetStringRawBuffer(a, &n);
  check(n == 5 && wcscmp(raw, L"hello") == 0 && WindowsGetStringLen(a) == 5 && !WindowsIsStringEmpty(a), "raw buffer and length");
  check(WindowsCreateString(L"", 0, &b) == S_OK && b == NULL && WindowsIsStringEmpty(b), "the empty string is NULL");
  check(WindowsCreateString(NULL, 3, &b) == E_POINTER, "NULL source");
  check(WindowsDuplicateString(a, &b) == S_OK && b == a, "duplicate shares");
  check(WindowsDeleteString(b) == S_OK, "delete the duplicate");
  HSTRING_HEADER header;
  HSTRING r = NULL;
  check(WindowsCreateStringReference(L"help", 4, &header, &r) == S_OK && r, "WindowsCreateStringReference");
  check(WindowsCreateStringReference(L"help", 3, &header, &c) == E_INVALIDARG, "a reference must be terminated at its length");
  INT32 order = 9;
  check(WindowsCompareStringOrdinal(a, r, &order) == S_OK && order == -1, "hello < help");
  check(WindowsCompareStringOrdinal(r, a, &order) == S_OK && order == 1, "help > hello");
  check(WindowsCompareStringOrdinal(NULL, NULL, &order) == S_OK && order == 0, "empty == empty");
  check(WindowsDuplicateString(r, &c) == S_OK && c && c != r, "duplicating a reference copies it");
  check(WindowsCompareStringOrdinal(c, r, &order) == S_OK && order == 0, "the copy is equal");
  WindowsDeleteString(c);
  check(WindowsConcatString(a, r, &c) == S_OK && wcscmp(WindowsGetStringRawBuffer(c, NULL), L"hellohelp") == 0, "WindowsConcatString");
  WindowsDeleteString(c);
  check(WindowsSubstringWithSpecifiedLength(a, 1, 3, &c) == S_OK && wcscmp(WindowsGetStringRawBuffer(c, NULL), L"ell") == 0, "substring");
  WindowsDeleteString(c);
  check(WindowsSubstring(a, 6, &c) == E_BOUNDS, "substring out of bounds");
  check(WindowsDeleteString(r) == S_OK && WindowsDeleteString(a) == S_OK && WindowsDeleteString(NULL) == S_OK, "delete");
  check(wcscmp(WindowsGetStringRawBuffer(NULL, &n), L"") == 0 && n == 0, "NULL's raw buffer");
  /* WinRT activation: no classes. */
  HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
  check(SUCCEEDED(hr), "RoInitialize");
  void *factory = (void *)1;
  WindowsCreateString(L"Windows.Foundation.Uri", 22, &a);
  check(RoGetActivationFactory(a, &IID_IUnknown, &factory) == REGDB_E_CLASSNOTREG && factory == NULL, "RoGetActivationFactory fails cleanly");
  IInspectable *instance = (IInspectable *)1;
  check(RoActivateInstance(a, &instance) == REGDB_E_CLASSNOTREG && instance == NULL, "RoActivateInstance fails cleanly");
  WindowsDeleteString(a);
  if (SUCCEEDED(hr)) RoUninitialize();
}

static void test_netapi(void) {
  LPWSTR name = NULL;
  NETSETUP_JOIN_STATUS status = NetSetupUnknownStatus;
  const DWORD joined = NetGetJoinInformation(NULL, &name, &status);
  check(joined == NERR_Success && name && wcscmp(name, L"WORKGROUP") == 0 && status == NetSetupWorkgroupName, "NetGetJoinInformation");
  if (joined != NERR_Success || !name) printf("  NetGetJoinInformation: %lu, name %p, status %d\n", joined, (void *)name, (int)status);
  NetApiBufferFree(name);
  typedef HRESULT (WINAPI *AadFn)(LPCWSTR, void **);
  AadFn aad = (AadFn)(void *)GetProcAddress(LoadLibraryW(L"netapi32.dll"), "NetGetAadJoinInformation");
  void *info = (void *)1;
  check(aad && aad(NULL, &info) == S_OK && info == NULL, "NetGetAadJoinInformation: not joined");
  PDSROLE_PRIMARY_DOMAIN_INFO_BASIC role = NULL;
  check(DsRoleGetPrimaryDomainInformation(NULL, DsRolePrimaryDomainInfoBasic, (PBYTE *)&role) == ERROR_SUCCESS && role &&
            role->MachineRole == DsRole_RoleStandaloneWorkstation && role->DomainNameFlat && wcscmp(role->DomainNameFlat, L"WORKGROUP") == 0,
        "DsRoleGetPrimaryDomainInformation");
  DsRoleFreeMemory(role);
  LPBYTE user = (LPBYTE)1;
  check(NetUserGetInfo(NULL, L"nobody", 0, &user) != NERR_Success && user == NULL, "NetUserGetInfo fails cleanly");
}

static void test_spooler(void) {
  WCHAR printer[260];
  DWORD size = 260;
  SetLastError(0);
  check(!GetDefaultPrinterW(printer, &size) && GetLastError() == ERROR_FILE_NOT_FOUND, "no default printer");
  DWORD needed = 9, returned = 9;
  check(EnumPrintersW(PRINTER_ENUM_LOCAL, NULL, 2, NULL, 0, &needed, &returned) && needed == 0 && returned == 0, "no printers");
  HANDLE h = (HANDLE)1;
  check(!OpenPrinterW(L"Nothing", &h, NULL) && h == NULL, "OpenPrinterW fails");
  HMODULE spool = LoadLibraryW(L"winspool.drv");
  check(spool && GetProcAddress(spool, (LPCSTR)203) == GetProcAddress(spool, "GetDefaultPrinterW"), "winspool.drv #203 is GetDefaultPrinterW");
}

static void test_kernel32(void) {
  HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
  BOOL (WINAPI *compare)(HANDLE, HANDLE) = (void *)GetProcAddress(k32, "CompareObjectHandles");
  void (WINAPI *interrupt_precise)(PULONGLONG) = (void *)GetProcAddress(k32, "QueryInterruptTimePrecise");
  void (WINAPI *unbiased_precise)(PULONGLONG) = (void *)GetProcAddress(k32, "QueryUnbiasedInterruptTimePrecise");
  check(compare && interrupt_precise && unbiased_precise, "the kernel32 exports exist");
  if (!compare || !interrupt_precise || !unbiased_precise) return;
  HANDLE e1 = CreateEventW(NULL, TRUE, FALSE, NULL), e2 = CreateEventW(NULL, TRUE, FALSE, NULL), dup = NULL;
  DuplicateHandle(GetCurrentProcess(), e1, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
  check(compare(e1, dup), "CompareObjectHandles: a duplicate is the same object");
  check(!compare(e1, e2), "CompareObjectHandles: two events differ");
  CloseHandle(e1);
  CloseHandle(e2);
  CloseHandle(dup);
  ULONGLONG t0 = 0, t1 = 0, u = 0;
  interrupt_precise(&t0);
  Sleep(20);
  interrupt_precise(&t1);
  unbiased_precise(&u);
  check(t0 > 0 && t1 > t0 && t1 - t0 >= 100000 && t1 - t0 < 10000000, "QueryInterruptTimePrecise advances in 100 ns units");
  check(u > 0 && u <= t1 + 10000000, "QueryUnbiasedInterruptTimePrecise");
  ULONGLONG plain = 0;
  check(QueryUnbiasedInterruptTime(&plain) && plain >= u, "QueryUnbiasedInterruptTime");
}

static void test_ordinals(void) {
  HMODULE shell32 = LoadLibraryW(L"shell32.dll");
  check(shell32 && GetProcAddress(shell32, (LPCSTR)680) == GetProcAddress(shell32, "IsUserAnAdmin"), "shell32 #680 is IsUserAnAdmin");
  check(shell32 && GetProcAddress(shell32, (LPCSTR)2) == GetProcAddress(shell32, "SHChangeNotifyRegister"), "shell32 #2 is SHChangeNotifyRegister");
  check(shell32 && GetProcAddress(shell32, (LPCSTR)4) == GetProcAddress(shell32, "SHChangeNotifyDeregister"), "shell32 #4 is SHChangeNotifyDeregister");
  HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  check(d3d12 && GetProcAddress(d3d12, (LPCSTR)101) == GetProcAddress(d3d12, "D3D12CreateDevice"), "d3d12 #101 is D3D12CreateDevice");
  HMODULE power = LoadLibraryW(L"powrprof.dll");
  check(power != NULL, "powrprof.dll loads");
  check(power && LoadLibraryW(L"api-ms-win-power-base-l1-1-0.dll") == power, "api-ms-win-power-* is powrprof.dll");
  check(LoadLibraryW(L"bthprops.dll") != NULL, "bthprops.dll loads");
  check(LoadLibraryW(L"bthprops.cpl") != NULL, "bthprops.cpl loads");
}

/* What Chromium's logging and network code ask at startup: FormatMessage of common system errors (else "Error (0x13D) while retrieving error"), the TraceLogging
 * provider traits (EventSetInformation class 2, which mingw's header does not name), the NLA name space (WSALookupServiceBegin(NS_NLA)) and the Tcpip key the
 * DNS watcher opens. */
static void test_startup_queries(void) {
  static const DWORD codes[] = {ERROR_BROKEN_PIPE, ERROR_SHARING_VIOLATION, ERROR_PIPE_NOT_CONNECTED, ERROR_NO_DATA, ERROR_IO_PENDING, ERROR_NOT_FOUND, WSAECONNREFUSED,
                                0x80040154u /* REGDB_E_CLASSNOTREG */, 0x8007006Du /* HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE) */};
  for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; ++i) {
    char text[512], what[64];
    const DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, codes[i], 0, text, sizeof text, NULL);
    snprintf(what, sizeof what, "FormatMessage of system message 0x%lX", codes[i]);
    check(n > 2 && text[n - 1] == '\n', what);
  }
  char text[64];
  SetLastError(0);
  check(FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, 0x0FFFFFFF, 0, text, sizeof text, NULL) == 0 &&
            GetLastError() == ERROR_MR_MID_NOT_FOUND, "FormatMessage of an unknown code: ERROR_MR_MID_NOT_FOUND");

  static const GUID provider = {0x1d0e7b8c, 0x5a4f, 0x4c3b, {0x9e, 0x2d, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66}};
  REGHANDLE reg = 0;
  check(EventRegister(&provider, NULL, NULL, &reg) == ERROR_SUCCESS, "EventRegister");
  BYTE traits[16] = {9, 0, 't', 'c', 'o', 's', '.', 't', 0};
  check(EventSetInformation(reg, (EVENT_INFO_CLASS)2 /* EventProviderSetTraits */, traits, 9) == ERROR_SUCCESS, "EventSetInformation(EventProviderSetTraits)");
  check(EventUnregister(reg) == ERROR_SUCCESS, "EventUnregister");

  WSADATA wsa;
  check(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "WSAStartup");
  WSAQUERYSETW query;
  memset(&query, 0, sizeof query);
  query.dwSize = sizeof query;
  query.dwNameSpace = NS_NLA;
  HANDLE lookup = NULL;
  check(WSALookupServiceBeginW(&query, LUP_RETURN_ALL, &lookup) == 0 && lookup != NULL, "WSALookupServiceBegin(NS_NLA)");
  if (lookup) {
    union { WSAQUERYSETW set; char bytes[sizeof(WSAQUERYSETW) + 256]; } result;
    DWORD length = 0;
    const int first = WSALookupServiceNextW(lookup, LUP_RETURN_NAME, &length, &result.set);      /* (no buffer: the size needed, or no network at all) */
    const int error = first ? WSAGetLastError() : 0;
    check(first == SOCKET_ERROR && (error == WSAEFAULT || error == WSA_E_NO_MORE), "WSALookupServiceNext without room");
    if (error == WSAEFAULT) {
      check(length > sizeof(WSAQUERYSETW) && length <= sizeof result, "WSALookupServiceNext says what it needs");
      length = sizeof result;
      check(WSALookupServiceNextW(lookup, LUP_RETURN_NAME, &length, &result.set) == 0 && result.set.lpszServiceInstanceName && result.set.lpszServiceInstanceName[0],
            "WSALookupServiceNext names a network");
    }
    check(WSALookupServiceEnd(lookup) == 0, "WSALookupServiceEnd");
  }
  SOCKET v6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  check(v6 != INVALID_SOCKET, "an IPv6 UDP socket");
  if (v6 != INVALID_SOCKET) {      /* (what Chromium's address sorter does with every DNS answer: IPv4 as mapped IPv6) */
    SOCKADDR_STORAGE addresses[2];
    memset(addresses, 0, sizeof addresses);
    static const BYTE ipv4[4] = {93, 184, 216, 34};
    SOCKADDR_IN6 *a = (SOCKADDR_IN6 *)&addresses[0];
    a->sin6_family = AF_INET6;
    a->sin6_addr.s6_addr[10] = a->sin6_addr.s6_addr[11] = 0xFF;
    memcpy(&a->sin6_addr.s6_addr[12], ipv4, 4);
    SOCKADDR_IN6 *b = (SOCKADDR_IN6 *)&addresses[1];
    b->sin6_family = AF_INET6;
    b->sin6_addr.s6_addr[0] = 0x20;
    b->sin6_addr.s6_addr[1] = 0x01;
    b->sin6_addr.s6_addr[2] = 0x0d;
    b->sin6_addr.s6_addr[3] = 0xb8;
    b->sin6_addr.s6_addr[15] = 1;
    union { SOCKET_ADDRESS_LIST list; BYTE bytes[sizeof(SOCKET_ADDRESS_LIST) + sizeof(SOCKET_ADDRESS)]; } input, output;
    input.list.iAddressCount = 2;
    input.list.Address[0].lpSockaddr = (LPSOCKADDR)a;
    input.list.Address[0].iSockaddrLength = sizeof *a;
    input.list.Address[1].lpSockaddr = (LPSOCKADDR)b;
    input.list.Address[1].iSockaddrLength = sizeof *b;
    DWORD returned = 0;
    check(WSAIoctl(v6, SIO_ADDRESS_LIST_SORT, &input, sizeof input, &output, sizeof output, &returned, NULL, NULL) == 0, "SIO_ADDRESS_LIST_SORT");
    check(output.list.iAddressCount == 2 && ((output.list.Address[0].lpSockaddr == (LPSOCKADDR)a && output.list.Address[1].lpSockaddr == (LPSOCKADDR)b) ||
                                             (output.list.Address[0].lpSockaddr == (LPSOCKADDR)b && output.list.Address[1].lpSockaddr == (LPSOCKADDR)a)),
          "SIO_ADDRESS_LIST_SORT keeps both addresses");
    closesocket(v6);
  }
  query.dwNameSpace = NS_DNS;
  lookup = NULL;
  check(WSALookupServiceBeginW(&query, LUP_RETURN_ALL, &lookup) == SOCKET_ERROR && WSAGetLastError() == WSASERVICE_NOT_FOUND, "no DNS name-space provider");
  WSACleanup();

  HKEY key = NULL;
  check(RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters", 0, KEY_NOTIFY, &key) == ERROR_SUCCESS, "the Tcpip parameters key exists");
  if (key) {
    HANDLE changed = CreateEventW(NULL, TRUE, FALSE, NULL);
    check(RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_CHANGE_NAME, changed, TRUE) == ERROR_SUCCESS, "watch the Tcpip key");
    RegCloseKey(key);
    CloseHandle(changed);
  }
}

int main(void) {
  test_prng();
  test_version();
  test_hstring();
  test_netapi();
  test_spooler();
  test_kernel32();
  test_ordinals();
  test_startup_queries();
  printf("chromedlltest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
