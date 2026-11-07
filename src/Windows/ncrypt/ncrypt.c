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

/* ncrypt.dll for TheoryCraftOS: the CNG key storage router. There is no key storage provider here (neither the software KSP nor the TPM's "Platform Crypto Provider",
 * nor smart cards), so NCryptOpenStorageProvider fails with NTE_PROV_DLL_NOT_FOUND - what Windows says for a provider that is not installed - and, since no provider
 * or key handle can exist, every call on a handle gives NTE_INVALID_HANDLE with its outputs zeroed (programs such as Chrome then go without hardware-bound keys).
 * NCryptEnumStorageProviders lists none. The BCrypt* names that ncrypt.dll exports on Windows are forwarders to bcrypt.dll (ncrypt.def); the protection descriptor,
 * stream and Ssl* exports stay stubs (src/Windows/stubs/ncrypt.inc). Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ncrypt.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

#define ST_OK ((SECURITY_STATUS)0)
#define ST_INVALID_HANDLE ((SECURITY_STATUS)NTE_INVALID_HANDLE)
#define ST_INVALID_PARAMETER ((SECURITY_STATUS)NTE_INVALID_PARAMETER)

/* ------------------------------------------------------------------------------------------------------------------------- providers */

EXPORT SECURITY_STATUS WINAPI NCryptOpenStorageProvider(NCRYPT_PROV_HANDLE *provider, LPCWSTR name, DWORD flags) {
  (void)name;
  (void)flags;
  if (!provider) return ST_INVALID_PARAMETER;
  *provider = 0;
  return (SECURITY_STATUS)NTE_PROV_DLL_NOT_FOUND;
}

EXPORT SECURITY_STATUS WINAPI NCryptEnumStorageProviders(DWORD *count, NCryptProviderName **list, DWORD flags) {
  (void)flags;
  if (!count || !list) return ST_INVALID_PARAMETER;
  *count = 0;
  *list = NULL;
  return ST_OK;
}

/* (Nothing this DLL gives out is a buffer, except NULL lists.) */
EXPORT SECURITY_STATUS WINAPI NCryptFreeBuffer(PVOID buffer) { return buffer ? ST_INVALID_PARAMETER : ST_OK; }

EXPORT SECURITY_STATUS WINAPI NCryptFreeObject(NCRYPT_HANDLE object) { (void)object; return ST_INVALID_HANDLE; }
EXPORT WINBOOL WINAPI NCryptIsKeyHandle(NCRYPT_KEY_HANDLE key) { (void)key; return FALSE; }

EXPORT SECURITY_STATUS WINAPI NCryptIsAlgSupported(NCRYPT_PROV_HANDLE provider, LPCWSTR algorithm, DWORD flags) {
  (void)provider; (void)algorithm; (void)flags;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptEnumAlgorithms(NCRYPT_PROV_HANDLE provider, DWORD operations, DWORD *count, NCryptAlgorithmName **list, DWORD flags) {
  (void)provider; (void)operations; (void)flags;
  if (count) *count = 0;
  if (list) *list = NULL;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptEnumKeys(NCRYPT_PROV_HANDLE provider, LPCWSTR scope, NCryptKeyName **name, PVOID *state, DWORD flags) {
  (void)provider; (void)scope; (void)state; (void)flags;
  if (name) *name = NULL;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptNotifyChangeKey(NCRYPT_PROV_HANDLE provider, HANDLE *event, DWORD flags) {
  (void)provider; (void)event; (void)flags;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptTranslateHandle(NCRYPT_PROV_HANDLE *provider, NCRYPT_KEY_HANDLE *key, HCRYPTPROV legacy_provider, HCRYPTKEY legacy_key,
                                                   DWORD key_spec, DWORD flags) {
  (void)legacy_provider; (void)legacy_key; (void)key_spec; (void)flags;
  if (provider) *provider = 0;
  if (key) *key = 0;
  return (SECURITY_STATUS)NTE_NOT_SUPPORTED;
}

/* ------------------------------------------------------------------------------------------------------------------------- keys */

EXPORT SECURITY_STATUS WINAPI NCryptOpenKey(NCRYPT_PROV_HANDLE provider, NCRYPT_KEY_HANDLE *key, LPCWSTR name, DWORD key_spec, DWORD flags) {
  (void)provider; (void)name; (void)key_spec; (void)flags;
  if (key) *key = 0;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptCreatePersistedKey(NCRYPT_PROV_HANDLE provider, NCRYPT_KEY_HANDLE *key, LPCWSTR algorithm, LPCWSTR name, DWORD key_spec,
                                                      DWORD flags) {
  (void)provider; (void)algorithm; (void)name; (void)key_spec; (void)flags;
  if (key) *key = 0;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptImportKey(NCRYPT_PROV_HANDLE provider, NCRYPT_KEY_HANDLE import_key, LPCWSTR blob_type, NCryptBufferDesc *parameters,
                                             NCRYPT_KEY_HANDLE *key, PBYTE data, DWORD size, DWORD flags) {
  (void)provider; (void)import_key; (void)blob_type; (void)parameters; (void)data; (void)size; (void)flags;
  if (key) *key = 0;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptFinalizeKey(NCRYPT_KEY_HANDLE key, DWORD flags) { (void)key; (void)flags; return ST_INVALID_HANDLE; }
EXPORT SECURITY_STATUS WINAPI NCryptDeleteKey(NCRYPT_KEY_HANDLE key, DWORD flags) { (void)key; (void)flags; return ST_INVALID_HANDLE; }

EXPORT SECURITY_STATUS WINAPI NCryptGetProperty(NCRYPT_HANDLE object, LPCWSTR property, PBYTE output, DWORD capacity, DWORD *result, DWORD flags) {
  (void)object; (void)property; (void)output; (void)capacity; (void)flags;
  if (result) *result = 0;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptSetProperty(NCRYPT_HANDLE object, LPCWSTR property, PBYTE input, DWORD size, DWORD flags) {
  (void)object; (void)property; (void)input; (void)size; (void)flags;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptExportKey(NCRYPT_KEY_HANDLE key, NCRYPT_KEY_HANDLE export_key, LPCWSTR blob_type, NCryptBufferDesc *parameters, PBYTE output,
                                             DWORD capacity, DWORD *result, DWORD flags) {
  (void)key; (void)export_key; (void)blob_type; (void)parameters; (void)output; (void)capacity; (void)flags;
  if (result) *result = 0;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptSignHash(NCRYPT_KEY_HANDLE key, VOID *padding, PBYTE hash, DWORD hash_size, PBYTE signature, DWORD capacity, DWORD *result,
                                            DWORD flags) {
  (void)key; (void)padding; (void)hash; (void)hash_size; (void)signature; (void)capacity; (void)flags;
  if (result) *result = 0;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptVerifySignature(NCRYPT_KEY_HANDLE key, VOID *padding, PBYTE hash, DWORD hash_size, PBYTE signature, DWORD size, DWORD flags) {
  (void)key; (void)padding; (void)hash; (void)hash_size; (void)signature; (void)size; (void)flags;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptEncrypt(NCRYPT_KEY_HANDLE key, PBYTE input, DWORD size, VOID *padding, PBYTE output, DWORD capacity, DWORD *result, DWORD flags) {
  (void)key; (void)input; (void)size; (void)padding; (void)output; (void)capacity; (void)flags;
  if (result) *result = 0;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptDecrypt(NCRYPT_KEY_HANDLE key, PBYTE input, DWORD size, VOID *padding, PBYTE output, DWORD capacity, DWORD *result, DWORD flags) {
  (void)key; (void)input; (void)size; (void)padding; (void)output; (void)capacity; (void)flags;
  if (result) *result = 0;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptSecretAgreement(NCRYPT_KEY_HANDLE private_key, NCRYPT_KEY_HANDLE public_key, NCRYPT_SECRET_HANDLE *secret, DWORD flags) {
  (void)private_key; (void)public_key; (void)flags;
  if (secret) *secret = 0;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptDeriveKey(NCRYPT_SECRET_HANDLE secret, LPCWSTR kdf, NCryptBufferDesc *parameters, PBYTE output, DWORD capacity, DWORD *result,
                                             ULONG flags) {
  (void)secret; (void)kdf; (void)parameters; (void)output; (void)capacity; (void)flags;
  if (result) *result = 0;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptKeyDerivation(NCRYPT_KEY_HANDLE key, NCryptBufferDesc *parameters, PUCHAR output, DWORD capacity, DWORD *result, ULONG flags) {
  (void)key; (void)parameters; (void)output; (void)capacity; (void)flags;
  if (result) *result = 0;
  return ST_INVALID_HANDLE;
}

/* Key attestation claims (of TPM keys). */
EXPORT SECURITY_STATUS WINAPI NCryptCreateClaim(NCRYPT_KEY_HANDLE subject, NCRYPT_KEY_HANDLE authority, DWORD type, NCryptBufferDesc *parameters, PBYTE claim,
                                               DWORD capacity, DWORD *result, DWORD flags) {
  (void)subject; (void)authority; (void)type; (void)parameters; (void)claim; (void)capacity; (void)flags;
  if (result) *result = 0;
  return ST_INVALID_HANDLE;
}

EXPORT SECURITY_STATUS WINAPI NCryptVerifyClaim(NCRYPT_KEY_HANDLE subject, NCRYPT_KEY_HANDLE authority, DWORD type, NCryptBufferDesc *parameters, PBYTE claim,
                                               DWORD size, NCryptBufferDesc *output, DWORD flags) {
  (void)subject; (void)authority; (void)type; (void)parameters; (void)claim; (void)size; (void)output; (void)flags;
  return ST_INVALID_HANDLE;
}
