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

/* wintrust.dll for TheoryCraftOS: Authenticode. Signatures are not checked here (there is no PKCS #7 / certificate chain engine and no catalog database), so
 * WinVerifyTrust answers every verification with TRUST_E_NOSIGNATURE - "the file is not signed", the answer a caller treats as untrusted without being an error of
 * the call - and keeps no state: the WTD_STATEACTION_CLOSE that follows a verification succeeds, hWVTStateData stays NULL and WTHelperProvDataFromStateData gives
 * NULL. The catalog administrator functions fail (no catalog database): a caller that cannot look a file up in a catalog falls back to the embedded signature.
 * Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <windows.h>
#include <wintrust.h>
#include <mscat.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

/* ------------------------------------------------------------------------------------------------------------------------- WinVerifyTrust */

/* The state action of a WINTRUST_DATA (only read when the structure is large enough to have it). */
static DWORD state_action(WINTRUST_DATA *data) {
  if (data->cbStruct < offsetof(WINTRUST_DATA, dwStateAction) + sizeof(DWORD)) return WTD_STATEACTION_IGNORE;
  return data->dwStateAction;
}

static LONG verify(GUID *action, WINTRUST_DATA *data) {
  if (!action || !data || data->cbStruct < offsetof(WINTRUST_DATA, dwUnionChoice) + sizeof(DWORD)) return ERROR_INVALID_PARAMETER;
  const DWORD state = state_action(data);
  if (data->cbStruct >= offsetof(WINTRUST_DATA, hWVTStateData) + sizeof(HANDLE)) data->hWVTStateData = NULL;
  if (state == WTD_STATEACTION_CLOSE || state == WTD_STATEACTION_AUTO_CACHE_FLUSH) return ERROR_SUCCESS;
  return (LONG)TRUST_E_NOSIGNATURE;
}

EXPORT LONG WINAPI WinVerifyTrust(HWND window, GUID *action, LPVOID data) {
  (void)window;
  const LONG r = verify(action, data);
  SetLastError((DWORD)r);
  return r;
}

EXPORT HRESULT WINAPI WinVerifyTrustEx(HWND window, GUID *action, WINTRUST_DATA *data) {
  (void)window;
  const LONG r = verify(action, data);
  SetLastError((DWORD)r);
  return (HRESULT)r;
}

/* No provider data is ever made. */
EXPORT CRYPT_PROVIDER_DATA *WINAPI WTHelperProvDataFromStateData(HANDLE state) {
  (void)state;
  SetLastError(ERROR_INVALID_PARAMETER);
  return NULL;
}
EXPORT CRYPT_PROVIDER_SGNR *WINAPI WTHelperGetProvSignerFromChain(CRYPT_PROVIDER_DATA *data, DWORD signer, WINBOOL counter_signer, DWORD counter_index) {
  (void)data; (void)signer; (void)counter_signer; (void)counter_index;
  SetLastError(ERROR_INVALID_PARAMETER);
  return NULL;
}
EXPORT CRYPT_PROVIDER_CERT *WINAPI WTHelperGetProvCertFromChain(CRYPT_PROVIDER_SGNR *signer, DWORD index) {
  (void)signer; (void)index;
  SetLastError(ERROR_INVALID_PARAMETER);
  return NULL;
}

/* ------------------------------------------------------------------------------------------------------------------------- catalogs */

EXPORT WINBOOL WINAPI CryptCATAdminAcquireContext(HCATADMIN *admin, const GUID *subsystem, DWORD flags) {
  (void)subsystem; (void)flags;
  if (admin) *admin = NULL;
  SetLastError(admin ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER);
  return FALSE;
}

EXPORT WINBOOL WINAPI CryptCATAdminAcquireContext2(HCATADMIN *admin, const GUID *subsystem, PCWSTR hash_algorithm, PCCERT_STRONG_SIGN_PARA policy, DWORD flags) {
  (void)hash_algorithm; (void)policy;
  return CryptCATAdminAcquireContext(admin, subsystem, flags);
}

EXPORT WINBOOL WINAPI CryptCATAdminReleaseContext(HCATADMIN admin, DWORD flags) {
  (void)admin; (void)flags;
  SetLastError(ERROR_INVALID_PARAMETER);
  return FALSE;
}

EXPORT WINBOOL WINAPI CryptCATAdminCalcHashFromFileHandle(HANDLE file, DWORD *size, BYTE *hash, DWORD flags) {
  (void)file; (void)hash; (void)flags;
  if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  SetLastError(ERROR_NOT_SUPPORTED);
  return FALSE;
}

EXPORT WINBOOL WINAPI CryptCATAdminCalcHashFromFileHandle2(HCATADMIN admin, HANDLE file, DWORD *size, BYTE *hash, DWORD flags) {
  (void)admin;
  return CryptCATAdminCalcHashFromFileHandle(file, size, hash, flags);
}

EXPORT HCATINFO WINAPI CryptCATAdminEnumCatalogFromHash(HCATADMIN admin, BYTE *hash, DWORD size, DWORD flags, HCATINFO *previous) {
  (void)admin; (void)hash; (void)size; (void)flags;
  if (previous) *previous = NULL;
  SetLastError(ERROR_NOT_FOUND);
  return NULL;
}

EXPORT WINBOOL WINAPI CryptCATAdminReleaseCatalogContext(HCATADMIN admin, HCATINFO info, DWORD flags) {
  (void)admin; (void)info; (void)flags;
  SetLastError(ERROR_INVALID_PARAMETER);
  return FALSE;
}

EXPORT WINBOOL WINAPI CryptCATCatalogInfoFromContext(HCATINFO info, CATALOG_INFO *catalog, DWORD flags) {
  (void)info; (void)flags;
  if (catalog && catalog->cbStruct >= sizeof(DWORD) + sizeof(WCHAR)) catalog->wszCatalogFile[0] = 0;
  SetLastError(ERROR_INVALID_PARAMETER);
  return FALSE;
}
