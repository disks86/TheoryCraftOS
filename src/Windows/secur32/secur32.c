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

/* secur32.dll for TheoryCraftOS. GetUserNameEx gives the names of the logged-on user that a computer outside a domain has (the SAM-compatible "COMPUTER\user" and
 * the display name; the domain forms are ERROR_NONE_MAPPED, as on such a computer). There are no security packages (no NTLM, Kerberos, Negotiate or Schannel): the
 * SSPI functions report SEC_E_SECPKG_NOT_FOUND, or SEC_E_INVALID_HANDLE for handles, which can only be made up as none is ever handed out, and the LSA logon functions
 * report that there is no LSA to talk to. Every out-parameter is cleared, so that no caller goes on with garbage. The other exports stay stubs
 * (src/Windows/stubs/secur32.inc). Built freestanding over kernel32.dll and advapi32.dll with the OS's mingw compiler, against mingw-w64's headers. */
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define SECURITY_WIN32
#define _NO_KSECDD_IMPORT_
#include <windows.h>
#include <winternl.h>
#include <ntsecapi.h>
#include <sspi.h>
#include <secext.h>
#include <lmcons.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#define ST_SUCCESS ((NTSTATUS)0)
#define ST_INVALID_HANDLE ((NTSTATUS)0xC0000008)
#define ST_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define ST_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002)
#define ST_NO_SUCH_PACKAGE ((NTSTATUS)0xC00000FE)

static void clear(void *p, SIZE_T n) { BYTE *b = p; if (b) while (n--) *b++ = 0; }

/* ----------------------------------------------------------------------------------------------------------------------------- user names */

/* The name for a format into buf (capacity chars with the NUL); its length without the NUL, 0 with *error set if the format has no name here. */
static DWORD user_name(EXTENDED_NAME_FORMAT format, WCHAR *buf, DWORD capacity, DWORD *error) {
  WCHAR user[UNLEN + 1], computer[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD user_size = UNLEN + 1, computer_size = MAX_COMPUTERNAME_LENGTH + 1;
  *error = ERROR_SUCCESS;
  if (!GetUserNameW(user, &user_size)) { *error = GetLastError(); return 0; }
  DWORD n = 0;
  switch (format) {
    case NameSamCompatible:
      if (!GetComputerNameW(computer, &computer_size)) { *error = GetLastError(); return 0; }
      for (DWORD i = 0; computer[i] && n + 1 < capacity; ++i) buf[n++] = computer[i];
      if (n + 1 < capacity) buf[n++] = L'\\';
      /* fall through */
    case NameDisplay:
      for (DWORD i = 0; user[i] && n + 1 < capacity; ++i) buf[n++] = user[i];
      buf[n] = 0;
      return n;
    default:      /* the distinguished, canonical, principal and GUID names exist only in a domain */
      *error = format == NameUnknown || format > NameSurname ? ERROR_INVALID_PARAMETER : ERROR_NONE_MAPPED;
      return 0;
  }
}

EXPORT BOOLEAN SEC_ENTRY GetUserNameExW(EXTENDED_NAME_FORMAT format, LPWSTR out, PULONG size) {
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR name[UNLEN + MAX_COMPUTERNAME_LENGTH + 4];
  DWORD error;
  const DWORD n = user_name(format, name, sizeof name / sizeof name[0], &error);
  if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
  if (!out || *size < n + 1) { *size = n + 1; SetLastError(ERROR_MORE_DATA); return FALSE; }      /* (the size needed, with the NUL) */
  for (DWORD i = 0; i <= n; ++i) out[i] = name[i];
  *size = n;      /* (what was written, without the NUL) */
  return TRUE;
}

EXPORT BOOLEAN SEC_ENTRY GetUserNameExA(EXTENDED_NAME_FORMAT format, LPSTR out, PULONG size) {
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR name[UNLEN + MAX_COMPUTERNAME_LENGTH + 4];
  DWORD error;
  const DWORD n = user_name(format, name, sizeof name / sizeof name[0], &error);
  if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
  char narrow[(UNLEN + MAX_COMPUTERNAME_LENGTH + 4) * 3];
  const int bytes = WideCharToMultiByte(CP_ACP, 0, name, (int)n + 1, narrow, sizeof narrow, NULL, NULL);
  if (bytes <= 0) { SetLastError(ERROR_NONE_MAPPED); return FALSE; }
  if (!out || *size < (ULONG)bytes) { *size = (ULONG)bytes; SetLastError(ERROR_MORE_DATA); return FALSE; }
  for (int i = 0; i < bytes; ++i) out[i] = narrow[i];
  *size = (ULONG)bytes - 1;
  return TRUE;
}

/* ----------------------------------------------------------------------------------------------------------------------------- SSPI: no packages */

static SECURITY_STATUS no_credentials(PCredHandle credential, PTimeStamp expiry) {
  if (credential) { credential->dwLower = 0; credential->dwUpper = 0; }
  if (expiry) expiry->QuadPart = 0;
  return SEC_E_SECPKG_NOT_FOUND;
}
EXPORT SECURITY_STATUS SEC_ENTRY AcquireCredentialsHandleW(LPWSTR principal, LPWSTR package, ULONG use, void *logon_id, void *auth_data, SEC_GET_KEY_FN get_key,
                                                           void *get_key_argument, PCredHandle credential, PTimeStamp expiry) {
  return no_credentials(credential, expiry);
}
EXPORT SECURITY_STATUS SEC_ENTRY AcquireCredentialsHandleA(LPSTR principal, LPSTR package, ULONG use, void *logon_id, void *auth_data, SEC_GET_KEY_FN get_key,
                                                           void *get_key_argument, PCredHandle credential, PTimeStamp expiry) {
  return no_credentials(credential, expiry);
}

static SECURITY_STATUS no_context(PCtxtHandle context, PSecBufferDesc output, ULONG *attributes, PTimeStamp expiry) {
  if (context) { context->dwLower = 0; context->dwUpper = 0; }
  if (attributes) *attributes = 0;
  if (expiry) expiry->QuadPart = 0;
  if (output && output->pBuffers) for (ULONG i = 0; i < output->cBuffers; ++i) if (output->pBuffers[i].BufferType == SECBUFFER_TOKEN) output->pBuffers[i].cbBuffer = 0;
  return SEC_E_INVALID_HANDLE;      /* (no credential handle is valid) */
}
EXPORT SECURITY_STATUS SEC_ENTRY InitializeSecurityContextW(PCredHandle credential, PCtxtHandle old_context, SEC_WCHAR *target, ULONG requirements, ULONG reserved,
                                                            ULONG representation, PSecBufferDesc input, ULONG reserved2, PCtxtHandle new_context, PSecBufferDesc output,
                                                            ULONG *attributes, PTimeStamp expiry) {
  return no_context(new_context, output, attributes, expiry);
}
EXPORT SECURITY_STATUS SEC_ENTRY InitializeSecurityContextA(PCredHandle credential, PCtxtHandle old_context, SEC_CHAR *target, ULONG requirements, ULONG reserved,
                                                            ULONG representation, PSecBufferDesc input, ULONG reserved2, PCtxtHandle new_context, PSecBufferDesc output,
                                                            ULONG *attributes, PTimeStamp expiry) {
  return no_context(new_context, output, attributes, expiry);
}
EXPORT SECURITY_STATUS SEC_ENTRY AcceptSecurityContext(PCredHandle credential, PCtxtHandle old_context, PSecBufferDesc input, ULONG requirements, ULONG representation,
                                                       PCtxtHandle new_context, PSecBufferDesc output, ULONG *attributes, PTimeStamp expiry) {
  return no_context(new_context, output, attributes, expiry);
}

EXPORT SECURITY_STATUS SEC_ENTRY QuerySecurityPackageInfoW(SEC_WCHAR *package, PSecPkgInfoW *info) {
  if (info) *info = NULL;
  return SEC_E_SECPKG_NOT_FOUND;
}
EXPORT SECURITY_STATUS SEC_ENTRY QuerySecurityPackageInfoA(SEC_CHAR *package, PSecPkgInfoA *info) {
  if (info) *info = NULL;
  return SEC_E_SECPKG_NOT_FOUND;
}

/* No packages: an empty list (the pointer stays NULL, which FreeContextBuffer accepts). */
EXPORT SECURITY_STATUS SEC_ENTRY EnumerateSecurityPackagesW(ULONG *count, PSecPkgInfoW *packages) {
  if (count) *count = 0;
  if (packages) *packages = NULL;
  return count && packages ? SEC_E_OK : SEC_E_INVALID_TOKEN;
}
EXPORT SECURITY_STATUS SEC_ENTRY EnumerateSecurityPackagesA(ULONG *count, PSecPkgInfoA *packages) {
  if (count) *count = 0;
  if (packages) *packages = NULL;
  return count && packages ? SEC_E_OK : SEC_E_INVALID_TOKEN;
}

/* Nothing here allocates a context buffer, so there is nothing to free. */
EXPORT SECURITY_STATUS SEC_ENTRY FreeContextBuffer(void *buffer) { return SEC_E_OK; }

EXPORT SECURITY_STATUS SEC_ENTRY FreeCredentialsHandle(PCredHandle credential) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY DeleteSecurityContext(PCtxtHandle context) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY QueryContextAttributesW(PCtxtHandle context, ULONG attribute, void *buffer) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY QueryContextAttributesA(PCtxtHandle context, ULONG attribute, void *buffer) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY SetContextAttributesW(PCtxtHandle context, ULONG attribute, void *buffer, ULONG size) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY SetContextAttributesA(PCtxtHandle context, ULONG attribute, void *buffer, ULONG size) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY QueryCredentialsAttributesW(PCredHandle credential, ULONG attribute, void *buffer) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY QueryCredentialsAttributesA(PCredHandle credential, ULONG attribute, void *buffer) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY CompleteAuthToken(PCtxtHandle context, PSecBufferDesc token) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY ApplyControlToken(PCtxtHandle context, PSecBufferDesc input) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY ImpersonateSecurityContext(PCtxtHandle context) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY RevertSecurityContext(PCtxtHandle context) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY MakeSignature(PCtxtHandle context, ULONG quality, PSecBufferDesc message, ULONG sequence) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY VerifySignature(PCtxtHandle context, PSecBufferDesc message, ULONG sequence, ULONG *quality) {
  if (quality) *quality = 0;
  return SEC_E_INVALID_HANDLE;
}
EXPORT SECURITY_STATUS SEC_ENTRY EncryptMessage(PCtxtHandle context, ULONG quality, PSecBufferDesc message, ULONG sequence) { return SEC_E_INVALID_HANDLE; }
EXPORT SECURITY_STATUS SEC_ENTRY DecryptMessage(PCtxtHandle context, PSecBufferDesc message, ULONG sequence, ULONG *quality) {
  if (quality) *quality = 0;
  return SEC_E_INVALID_HANDLE;
}
EXPORT SECURITY_STATUS SEC_ENTRY QuerySecurityContextToken(PCtxtHandle context, void **token) {
  if (token) *token = NULL;
  return SEC_E_INVALID_HANDLE;
}
EXPORT SECURITY_STATUS SEC_ENTRY ExportSecurityContext(PCtxtHandle context, ULONG flags, PSecBuffer packed, void **token) {
  if (packed) { packed->cbBuffer = 0; packed->pvBuffer = NULL; }
  if (token) *token = NULL;
  return SEC_E_INVALID_HANDLE;
}
EXPORT SECURITY_STATUS SEC_ENTRY ImportSecurityContextW(SEC_WCHAR *package, PSecBuffer packed, void *token, PCtxtHandle context) {
  if (context) { context->dwLower = 0; context->dwUpper = 0; }
  return SEC_E_SECPKG_NOT_FOUND;
}
EXPORT SECURITY_STATUS SEC_ENTRY ImportSecurityContextA(SEC_CHAR *package, PSecBuffer packed, void *token, PCtxtHandle context) {
  if (context) { context->dwLower = 0; context->dwUpper = 0; }
  return SEC_E_SECPKG_NOT_FOUND;
}

/* The function table of InitSecurityInterface: no provider to describe, so none (callers treat NULL as "SSPI is not available"). */
EXPORT PSecurityFunctionTableW SEC_ENTRY InitSecurityInterfaceW(void) { SetLastError(ERROR_NOT_SUPPORTED); return NULL; }
EXPORT PSecurityFunctionTableA SEC_ENTRY InitSecurityInterfaceA(void) { SetLastError(ERROR_NOT_SUPPORTED); return NULL; }

/* ----------------------------------------------------------------------------------------------------------------------------- LSA logon: no LSA */

EXPORT NTSTATUS NTAPI LsaConnectUntrusted(PHANDLE handle) {
  if (!handle) return ST_INVALID_PARAMETER;
  *handle = NULL;
  return ST_NOT_IMPLEMENTED;
}
EXPORT NTSTATUS NTAPI LsaRegisterLogonProcess(PLSA_STRING name, PHANDLE handle, PLSA_OPERATIONAL_MODE mode) {
  if (handle) *handle = NULL;
  if (mode) *mode = 0;
  return ST_NOT_IMPLEMENTED;
}
EXPORT NTSTATUS NTAPI LsaDeregisterLogonProcess(HANDLE handle) { return ST_INVALID_HANDLE; }
EXPORT NTSTATUS NTAPI LsaLookupAuthenticationPackage(HANDLE handle, PLSA_STRING name, PULONG package) {
  if (package) *package = 0;
  return handle ? ST_NO_SUCH_PACKAGE : ST_INVALID_HANDLE;
}
EXPORT NTSTATUS NTAPI LsaLogonUser(HANDLE handle, PLSA_STRING origin, SECURITY_LOGON_TYPE type, ULONG package, PVOID information, ULONG information_size,
                                   PTOKEN_GROUPS groups, PTOKEN_SOURCE source, PVOID *profile, PULONG profile_size, PLUID logon_id, PHANDLE token,
                                   PQUOTA_LIMITS quotas, PNTSTATUS sub_status) {
  if (profile) *profile = NULL;
  if (profile_size) *profile_size = 0;
  if (logon_id) { logon_id->LowPart = 0; logon_id->HighPart = 0; }
  if (token) *token = NULL;
  clear(quotas, sizeof *quotas);
  if (sub_status) *sub_status = ST_SUCCESS;
  return ST_INVALID_HANDLE;
}
EXPORT NTSTATUS NTAPI LsaCallAuthenticationPackage(HANDLE handle, ULONG package, PVOID buffer, ULONG size, PVOID *result, PULONG result_size, PNTSTATUS status) {
  if (result) *result = NULL;
  if (result_size) *result_size = 0;
  if (status) *status = ST_SUCCESS;
  return ST_INVALID_HANDLE;
}
EXPORT NTSTATUS NTAPI LsaEnumerateLogonSessions(PULONG count, PLUID *list) {
  if (count) *count = 0;
  if (list) *list = NULL;
  return ST_NOT_IMPLEMENTED;
}
EXPORT NTSTATUS NTAPI LsaGetLogonSessionData(PLUID logon_id, PSECURITY_LOGON_SESSION_DATA *data) {
  if (data) *data = NULL;
  return ST_NOT_IMPLEMENTED;
}
/* Nothing here hands out an LSA buffer. */
EXPORT NTSTATUS NTAPI LsaFreeReturnBuffer(PVOID buffer) { return ST_SUCCESS; }

/* An entry point of its own: the weak default of stubs.c would lose to the DllMain export of advapi32.dll, which this library links against. */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) { return TRUE; }
