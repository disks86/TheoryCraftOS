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

/* userenv.dll for TheoryCraftOS: the profile folders. There is one user; the profile is C:\Users\Default (the USERPROFILE of the environment). */
#include "../tcos_win.h"

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

IMPORT void WINAPI SetLastError(DWORD error);
IMPORT DWORD WINAPI GetEnvironmentVariableW(LPCWSTR name, LPWSTR buffer, DWORD capacity);
IMPORT int WINAPI WideCharToMultiByte(UINT code_page, DWORD flags, LPCWSTR source, int length, LPSTR dest, int capacity, LPCSTR default_char, LPBOOL used_default);

static const WCHAR kVariable[] = {'U', 'S', 'E', 'R', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0};
static const WCHAR kDefault[] = {'C', ':', '\\', 'U', 's', 'e', 'r', 's', '\\', 'D', 'e', 'f', 'a', 'u', 'l', 't', 0};

/* The folder in `buffer` (size in characters, with the end mark); on a buffer too small the size needed is stored and the error is ERROR_INSUFFICIENT_BUFFER. */
EXPORT BOOL WINAPI GetUserProfileDirectoryW(HANDLE token, LPWSTR buffer, LPDWORD size) {
  WCHAR folder[260];
  if (!token) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  DWORD n = GetEnvironmentVariableW(kVariable, folder, 260);
  if (n == 0 || n >= 260) { for (n = 0; kDefault[n]; n++) folder[n] = kDefault[n]; folder[n] = 0; }
  if (!buffer || *size < n + 1) { *size = n + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  for (DWORD i = 0; i <= n; i++) buffer[i] = folder[i];
  *size = n + 1;
  return TRUE;
}
EXPORT BOOL WINAPI GetUserProfileDirectoryA(HANDLE token, LPSTR buffer, LPDWORD size) {
  WCHAR wide[260];
  DWORD n = 260;
  if (!size || !buffer) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!GetUserProfileDirectoryW(token, wide, &n)) return FALSE;
  if (*size < n) { *size = n; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  for (DWORD i = 0; i < n; i++) buffer[i] = (char)wide[i];      /* (ASCII) */
  *size = n;
  return TRUE;
}

/* ---- environment blocks ---------------------------------------------------------------------------------------------------------------------------------- */

IMPORT WCHAR *WINAPI GetEnvironmentStringsW(void);
IMPORT BOOL WINAPI FreeEnvironmentStringsW(WCHAR *block);
IMPORT HANDLE WINAPI GetProcessHeap(void);
IMPORT LPVOID WINAPI HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes);
IMPORT BOOL WINAPI HeapFree(HANDLE heap, DWORD flags, LPVOID memory);
IMPORT size_t_ wcslen(const WCHAR *);

#define W(...) (const WCHAR *)(const WCHAR[]){__VA_ARGS__, 0}
static const struct { const char *name, *value; } kSystemVariables[] = {
  {"ALLUSERSPROFILE", "C:\\ProgramData"}, {"APPDATA", "C:\\Users\\Default\\AppData\\Roaming"}, {"CommonProgramFiles", "C:\\Program Files\\Common Files"},
  {"CommonProgramFiles(x86)", "C:\\Program Files (x86)\\Common Files"}, {"CommonProgramW6432", "C:\\Program Files\\Common Files"}, {"COMPUTERNAME", "TCOS"},
  {"ComSpec", "C:\\Windows\\System32\\cmd.exe"}, {"LOCALAPPDATA", "C:\\Users\\Default\\AppData\\Local"}, {"OS", "Windows_NT"}, {"PATH", "C:\\Windows\\System32;C:\\Windows"},
  {"PROCESSOR_ARCHITECTURE", "AMD64"}, {"PROCESSOR_IDENTIFIER", "Intel64 Family 6 Model 1 Stepping 1, GenuineIntel"}, {"PROCESSOR_LEVEL", "6"}, {"PROCESSOR_REVISION", "0101"},
  {"ProgramData", "C:\\ProgramData"}, {"ProgramFiles", "C:\\Program Files"}, {"ProgramFiles(x86)", "C:\\Program Files (x86)"}, {"ProgramW6432", "C:\\Program Files"},
  {"SystemDrive", "C:"}, {"TEMP", "C:\\Temp"}, {"TMP", "C:\\Temp"}, {"USERNAME", "Default"}, {"USERPROFILE", "C:\\Users\\Default"}, {"windir", "C:\\Windows"},
};

static WCHAR upcase(WCHAR c) { return c >= 'a' && c <= 'z' ? (WCHAR)(c - 32) : c; }
/* Does the entry `entry` ("NAME=value") start with `name`= ? */
static BOOL has_name(const WCHAR *entry, const char *name) {
  size_t_ i = 0;
  for (; name[i]; i++) if (upcase(entry[i]) != upcase((WCHAR)name[i])) return FALSE;
  return entry[i] == '=';
}

/* A new environment block for a user: the system variables fresh, SystemRoot and the number of processors as this process has them and, with `inherit`, everything else of this
   process's environment. DestroyEnvironmentBlock gives it back. */
EXPORT BOOL WINAPI CreateEnvironmentBlock(LPVOID *environment, HANDLE token, BOOL inherit) {
  if (!environment) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }      /* (no token: the default user) */
  WCHAR *current = GetEnvironmentStringsW();
  if (!current) return FALSE;
  size_t_ total = 2;
  for (size_t_ k = 0; k < sizeof(kSystemVariables) / sizeof(kSystemVariables[0]); k++) {
    size_t_ n = 0;
    while (kSystemVariables[k].name[n]) n++;
    size_t_ v = 0;
    while (kSystemVariables[k].value[v]) v++;
    total += n + 1 + v + 1;
  }
  total += 2 * 512;      /* SystemRoot and NUMBER_OF_PROCESSORS from this process */
  if (inherit) for (const WCHAR *e = current; *e; e += wcslen(e) + 1) total += wcslen(e) + 1;
  WCHAR *block = HeapAlloc(GetProcessHeap(), 0, total * 2);
  if (!block) { FreeEnvironmentStringsW(current); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  size_t_ out = 0;
  for (size_t_ k = 0; k < sizeof(kSystemVariables) / sizeof(kSystemVariables[0]); k++) {
    for (const char *c = kSystemVariables[k].name; *c; c++) block[out++] = (unsigned char)*c;
    block[out++] = '=';
    for (const char *c = kSystemVariables[k].value; *c; c++) block[out++] = (unsigned char)*c;
    block[out++] = 0;
  }
  BOOL got_processors = FALSE;
  for (const WCHAR *e = current; *e; e += wcslen(e) + 1) {
    const size_t_ n = wcslen(e);
    const BOOL taken = has_name(e, "SystemRoot") || has_name(e, "NUMBER_OF_PROCESSORS");
    if (has_name(e, "NUMBER_OF_PROCESSORS")) got_processors = TRUE;
    BOOL known = FALSE;
    if (!taken) for (size_t_ k = 0; k < sizeof(kSystemVariables) / sizeof(kSystemVariables[0]); k++) if (has_name(e, kSystemVariables[k].name)) known = TRUE;
    if (!(taken || (inherit && !known))) continue;
    if (out + n + 1 > total - 2) break;
    for (size_t_ i = 0; i <= n; i++) block[out++] = e[i];
  }
  if (!got_processors) { const WCHAR *one = W('N','U','M','B','E','R','_','O','F','_','P','R','O','C','E','S','S','O','R','S','=','1'); size_t_ n = wcslen(one); for (size_t_ i = 0; i <= n; i++) block[out++] = one[i]; }
  block[out++] = 0;
  FreeEnvironmentStringsW(current);
  *environment = block;
  return TRUE;
}
EXPORT BOOL WINAPI DestroyEnvironmentBlock(LPVOID environment) {
  if (!environment) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return HeapFree(GetProcessHeap(), 0, environment);
}

/* ---- profile folders -------------------------------------------------------------------------------------------------------------------------------- */

/* The folder (from the environment, expanded: %SystemDrive%\Users) into `buffer`. A buffer too small gets the size needed in characters, with the end mark, and
   ERROR_INSUFFICIENT_BUFFER; GetProfilesDirectoryA's size on that error leaves out the end mark, and its result is the number of characters stored with the end mark. */
static BOOL folder_w(const WCHAR *text, LPWSTR buffer, LPDWORD size) {
  size_t_ n = wcslen(text);
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!buffer || *size < n + 1) { *size = (DWORD)n + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  for (size_t_ i = 0; i <= n; i++) buffer[i] = text[i];
  *size = (DWORD)n + 1;
  return TRUE;
}
EXPORT BOOL WINAPI GetProfilesDirectoryW(LPWSTR buffer, LPDWORD size) { return folder_w(W('C',':','\\','U','s','e','r','s'), buffer, size); }
EXPORT BOOL WINAPI GetDefaultUserProfileDirectoryW(LPWSTR buffer, LPDWORD size) { return folder_w(W('C',':','\\','U','s','e','r','s','\\','D','e','f','a','u','l','t'), buffer, size); }
EXPORT BOOL WINAPI GetAllUsersProfileDirectoryW(LPWSTR buffer, LPDWORD size) { return folder_w(W('C',':','\\','P','r','o','g','r','a','m','D','a','t','a'), buffer, size); }
EXPORT DWORD WINAPI GetProfilesDirectoryA(LPSTR buffer, LPDWORD size) {
  WCHAR wide[64];
  DWORD n = 64;
  if (!size || (!buffer && *size == 0 && 0)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  if (!GetProfilesDirectoryW(wide, &n)) return 0;
  if (!buffer) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
  if (*size < n) { *size = n; SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
  for (DWORD i = 0; i < n; i++) buffer[i] = (char)wide[i];
  *size = n;
  return n;
}
EXPORT BOOL WINAPI GetDefaultUserProfileDirectoryA(LPSTR buffer, LPDWORD size) {
  WCHAR wide[64];
  DWORD n = 64;
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!GetDefaultUserProfileDirectoryW(wide, &n)) return FALSE;
  if (!buffer || *size < n) { *size = n; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  for (DWORD i = 0; i < n; i++) buffer[i] = (char)wide[i];
  *size = n;
  return TRUE;
}
EXPORT BOOL WINAPI GetAllUsersProfileDirectoryA(LPSTR buffer, LPDWORD size) {
  WCHAR wide[64];
  DWORD n = 64;
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!GetAllUsersProfileDirectoryW(wide, &n)) return FALSE;
  if (!buffer || *size < n) { *size = n; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
  for (DWORD i = 0; i < n; i++) buffer[i] = (char)wide[i];
  *size = n;
  return TRUE;
}

/* ---- app containers ---------------------------------------------------------------------------------------------------------------------------------------- *
 * The SID of an app container is derived from its name as on Windows: S-1-15-2-r1-...-r7, the seven sub-authorities being the first 28 bytes of the SHA-256 of the
 * lower-cased name (UTF-16, little-endian), read as little-endian DWORDs. The SID is allocated like RtlAllocateAndInitializeSid's, so FreeSid frees it. */

IMPORT NTSTATUS NTAPI RtlAllocateAndInitializeSid(const void *authority, BYTE count, DWORD s0, DWORD s1, DWORD s2, DWORD s3, DWORD s4, DWORD s5, DWORD s6, DWORD s7,
                                                  void **sid);

static const DWORD kSha256K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
  0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
  0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
  0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static DWORD ror32(DWORD x, int n) { return (x >> n) | (x << (32 - n)); }
static void sha256_block(DWORD state[8], const BYTE block[64]) {
  DWORD w[64];
  for (int i = 0; i < 16; i++) w[i] = (DWORD)block[4 * i] << 24 | (DWORD)block[4 * i + 1] << 16 | (DWORD)block[4 * i + 2] << 8 | block[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    const DWORD s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  DWORD a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
  for (int i = 0; i < 64; i++) {
    const DWORD t1 = h + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
    const DWORD t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}
/* SHA-256 of `size` bytes (at most a few hundred here: the name is limited). */
static void sha256(const BYTE *data, DWORD size, BYTE out[32]) {
  DWORD state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  BYTE block[64];
  DWORD done = 0;
  for (; size - done >= 64; done += 64) sha256_block(state, data + done);
  DWORD n = size - done;
  for (DWORD i = 0; i < n; i++) block[i] = data[done + i];
  block[n++] = 0x80;
  if (n > 56) { while (n < 64) block[n++] = 0; sha256_block(state, block); n = 0; }
  while (n < 56) block[n++] = 0;
  const unsigned long long bits = (unsigned long long)size * 8;
  for (int i = 0; i < 8; i++) block[56 + i] = (BYTE)(bits >> (56 - 8 * i));
  sha256_block(state, block);
  for (int i = 0; i < 8; i++) { out[4 * i] = (BYTE)(state[i] >> 24); out[4 * i + 1] = (BYTE)(state[i] >> 16); out[4 * i + 2] = (BYTE)(state[i] >> 8); out[4 * i + 3] = (BYTE)state[i]; }
}

#define APP_CONTAINER_NAME_MAX 64      /* (the longest name Windows accepts) */
EXPORT LONG WINAPI DeriveAppContainerSidFromAppContainerName(LPCWSTR name, void **sid) {
  if (sid) *sid = NULL;
  if (!name || !sid) return (LONG)0x80070057;      /* E_INVALIDARG */
  BYTE text[APP_CONTAINER_NAME_MAX * 2];
  DWORD n = 0;
  for (; name[n]; n++) {
    if (n >= APP_CONTAINER_NAME_MAX) return (LONG)0x80070057;
    WCHAR c = name[n];
    if (c >= 'A' && c <= 'Z') c = (WCHAR)(c + 32);
    text[2 * n] = (BYTE)c;
    text[2 * n + 1] = (BYTE)(c >> 8);
  }
  if (n < 2) return (LONG)0x80070057;
  BYTE hash[32];
  sha256(text, 2 * n, hash);
  DWORD rid[7];
  for (int i = 0; i < 7; i++) rid[i] = (DWORD)hash[4 * i] | (DWORD)hash[4 * i + 1] << 8 | (DWORD)hash[4 * i + 2] << 16 | (DWORD)hash[4 * i + 3] << 24;
  static const BYTE kAppPackageAuthority[6] = {0, 0, 0, 0, 0, 15};      /* SECURITY_APP_PACKAGE_AUTHORITY */
  if (RtlAllocateAndInitializeSid(kAppPackageAuthority, 8, 2 /* SECURITY_APP_PACKAGE_BASE_RID */, rid[0], rid[1], rid[2], rid[3], rid[4], rid[5], rid[6], sid) < 0) {
    *sid = NULL;
    return (LONG)0x8007000E;      /* E_OUTOFMEMORY */
  }
  return 0;
}

/* ---- group policy -------------------------------------------------------------------------------------------------------------------------------------------- *
 * There is no group policy: the critical section is a token that only says so, notifications are accepted and never sent, and the profile is a local one. */
#define POLICY_SECTION_USER ((HANDLE)(ULONG_PTR)0x47500001u)
#define POLICY_SECTION_MACHINE ((HANDLE)(ULONG_PTR)0x47500002u)
EXPORT HANDLE WINAPI EnterCriticalPolicySection(BOOL machine) { return machine ? POLICY_SECTION_MACHINE : POLICY_SECTION_USER; }
EXPORT BOOL WINAPI LeaveCriticalPolicySection(HANDLE section) {
  if (section != POLICY_SECTION_USER && section != POLICY_SECTION_MACHINE) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI RegisterGPNotification(HANDLE event, BOOL machine) {
  (void)machine;
  if (!event) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI UnregisterGPNotification(HANDLE event) {
  if (!event) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return TRUE;
}
/* GetProfileType: 0 is a local profile (not PT_TEMPORARY, PT_ROAMING or PT_MANDATORY). */
EXPORT BOOL WINAPI GetProfileType(DWORD *flags) {
  if (!flags) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  *flags = 0;
  return TRUE;
}

/* The entry point (without one of its own, the default of stubs.c would lose to the DllMain kernel32.dll exports, which would then run as this library's entry). */
BOOL WINAPI DllMain(HANDLE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}
