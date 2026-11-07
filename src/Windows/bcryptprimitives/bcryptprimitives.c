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

/* bcryptprimitives.dll for TheoryCraftOS: ProcessPrng, the random generator of a process that Chrome (base::RandBytes), Rust's std and the newer CRTs call from their
 * first instructions on. The bytes come from the system's source, NtQuerySystemInformation class 0x1000 (the same one BCryptGenRandom and RtlGenRandom read). The
 * Get*Interface tables of the CNG providers are not here: they fail with STATUS_NOT_SUPPORTED and a NULL table. Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define ST_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

/* Random bytes, in pieces the system call takes. Windows' ProcessPrng never fails (it is documented to always return TRUE); if the source does, the process cannot go on
 * safely with predictable bytes, so it ends like Windows ends a process whose generator is broken. */
static void fill_random(BYTE *out, SIZE_T size) {
  while (size) {
    const ULONG n = size > 0x10000 ? 0x10000 : (ULONG)size;
    if (NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)0x1000, out, n, NULL) < 0) {
      OutputDebugStringA("bcryptprimitives: the system's random source failed\n");
      TerminateProcess(GetCurrentProcess(), 0xC0000001u);
    }
    out += n;
    size -= n;
  }
}

EXPORT BOOL WINAPI ProcessPrng(PBYTE data, SIZE_T size) {
  if (size) fill_random(data, size);
  return TRUE;
}

/* A random GUID (version 4) of this process's generator. */
EXPORT BOOL WINAPI ProcessPrngGuid(GUID *guid) {
  if (!guid) return FALSE;
  fill_random((BYTE *)guid, sizeof *guid);
  guid->Data3 = (WORD)((guid->Data3 & 0x0FFF) | 0x4000);
  guid->Data4[0] = (BYTE)((guid->Data4[0] & 0x3F) | 0x80);
  return TRUE;
}

/* The function tables of the primitive providers (used by bcrypt.dll's own providers on Windows): none here. */
#define NO_INTERFACE(name) \
  EXPORT NTSTATUS WINAPI name(LPCWSTR provider, LPCWSTR algorithm, void **table, ULONG flags) { \
    (void)provider; (void)algorithm; (void)flags; \
    if (table) *table = NULL; \
    return ST_NOT_SUPPORTED; \
  }
NO_INTERFACE(GetAsymmetricEncryptionInterface)
NO_INTERFACE(GetCipherInterface)
NO_INTERFACE(GetHashInterface)
NO_INTERFACE(GetKeyDerivationInterface)
NO_INTERFACE(GetRngInterface)
NO_INTERFACE(GetSecretAgreementInterface)
NO_INTERFACE(GetSignatureInterface)
