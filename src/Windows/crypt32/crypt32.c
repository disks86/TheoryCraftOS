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

/* crypt32.dll for TheoryCraftOS. The parts:
 *   DPAPI         CryptProtectData / CryptUnprotectData: a real round trip. Each user has a random 256-bit master key, made on first use and kept in the registry
 *                 (HKCU\Software\TheoryCraftOS\DPAPI, value MasterKey; HKLM for CRYPTPROTECT_LOCAL_MACHINE). A blob is encrypted with AES-256-CBC and authenticated with
 *                 HMAC-SHA-256 (encrypt then MAC), both through bcrypt.dll, with keys derived from the master key, a random salt and the caller's optional entropy, so a
 *                 changed byte or the wrong entropy fails with ERROR_INVALID_DATA. Unlike Windows the master key is not protected by the user's password: whoever can read
 *                 the user's registry can decrypt the blobs. The blob format is below (blob_*); it is this OS's own, not Windows'.
 *   memory        CryptProtectMemory / CryptUnprotectMemory over RtlEncryptMemory / RtlDecryptMemory (advapi32 SystemFunction040 / 041), in 16-byte blocks.
 *   certificates  stores (memory, system, collection; nothing is persisted: the system stores ROOT, MY, CA ... open empty), certificate contexts parsed from DER into a
 *                 CERT_INFO (cert.inc), finding, names (CertGetNameStringW, CertNameToStrW), hash properties, key usages, time validity.
 * Not here (they fail cleanly): private keys (CryptAcquireCertificatePrivateKey), chains (CertFindChainInStore), signature checks (CryptVerifyCertificateSignatureEx),
 * messages (CryptMsg*) and CryptQueryObject; the rest of the exports stay stubs (src/Windows/stubs/crypt32.inc).
 * Built freestanding over kernel32.dll, advapi32.dll and bcrypt.dll with the OS's mingw compiler, against mingw-w64's headers. */
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#undef DECLSPEC_IMPORT
#define DECLSPEC_IMPORT
#include <wincrypt.h>
#include <bcrypt.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

/* RtlEncryptMemory / RtlDecryptMemory of advapi32.dll */
__declspec(dllimport) NTSTATUS WINAPI SystemFunction040(PVOID memory, ULONG size, ULONG option);
__declspec(dllimport) NTSTATUS WINAPI SystemFunction041(PVOID memory, ULONG size, ULONG option);

static void mem_copy(void *d, const void *s, SIZE_T n) { BYTE *p = d; const BYTE *q = s; while (n--) *p++ = *q++; }
static void mem_zero(void *d, SIZE_T n) { volatile BYTE *p = d; while (n--) *p++ = 0; }
static int mem_equal(const void *a, const void *b, SIZE_T n) { const BYTE *p = a, *q = b; for (SIZE_T i = 0; i < n; ++i) if (p[i] != q[i]) return 0; return 1; }
static SIZE_T wide_length(LPCWSTR s) { SIZE_T n = 0; while (s[n]) ++n; return n; }

static void *heap_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), 0, n ? n : 1); }
static void *heap_zalloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n ? n : 1); }
static void heap_free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* ----------------------------------------------------------------------------------------------------------------------------- bcrypt helpers */

/* HMAC-SHA-256 of the concatenation of up to three parts. */
static BOOL hmac_sha256(const BYTE *key, ULONG key_size, const void *a, ULONG a_size, const void *b, ULONG b_size, const void *c, ULONG c_size, BYTE out[32]) {
  BCRYPT_ALG_HANDLE alg;
  BCRYPT_HASH_HANDLE hash;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return FALSE;
  BOOL ok = FALSE;
  if (BCryptCreateHash(alg, &hash, NULL, 0, (PUCHAR)key, key_size, 0) >= 0) {
    ok = (!a_size || BCryptHashData(hash, (PUCHAR)a, a_size, 0) >= 0) && (!b_size || BCryptHashData(hash, (PUCHAR)b, b_size, 0) >= 0) &&
         (!c_size || BCryptHashData(hash, (PUCHAR)c, c_size, 0) >= 0) && BCryptFinishHash(hash, out, 32, 0) >= 0;
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

/* A plain digest (SHA-1, MD5, SHA-256) of a buffer. */
static BOOL digest(LPCWSTR algorithm, const BYTE *data, ULONG size, BYTE *out, ULONG out_size) {
  BCRYPT_ALG_HANDLE alg;
  BCRYPT_HASH_HANDLE hash;
  if (BCryptOpenAlgorithmProvider(&alg, algorithm, NULL, 0) < 0) return FALSE;
  BOOL ok = FALSE;
  if (BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) >= 0) {
    ok = (!size || BCryptHashData(hash, (PUCHAR)data, size, 0) >= 0) && BCryptFinishHash(hash, out, out_size, 0) >= 0;
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

/* AES-256-CBC with PKCS #7 padding. `out` has room for size + 16 bytes; *result is the size written. */
static BOOL aes_cbc(BOOL encrypt, const BYTE key[32], const BYTE iv[16], const BYTE *in, ULONG size, BYTE *out, ULONG capacity, ULONG *result) {
  BCRYPT_ALG_HANDLE alg;
  BCRYPT_KEY_HANDLE k;
  BYTE chain[16];
  *result = 0;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, NULL, 0) < 0) return FALSE;
  BOOL ok = FALSE;
  if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof BCRYPT_CHAIN_MODE_CBC, 0) >= 0 &&
      BCryptGenerateSymmetricKey(alg, &k, NULL, 0, (PUCHAR)key, 32, 0) >= 0) {
    mem_copy(chain, iv, 16);      /* (bcrypt updates the IV it is given) */
    const NTSTATUS s = encrypt ? BCryptEncrypt(k, (PUCHAR)in, size, NULL, chain, 16, out, capacity, result, BCRYPT_BLOCK_PADDING)
                               : BCryptDecrypt(k, (PUCHAR)in, size, NULL, chain, 16, out, capacity, result, BCRYPT_BLOCK_PADDING);
    ok = s >= 0;
    BCryptDestroyKey(k);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

static BOOL random_bytes(void *out, ULONG size) { return BCryptGenRandom(NULL, out, size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0; }

/* ----------------------------------------------------------------------------------------------------------------------------- DPAPI */

/* The master keys: [0] the user's, [1] the machine's. They are read (or made) once per process; a named mutex keeps two processes from making two different ones. */
static BYTE g_master[2][32];
static BOOL g_master_ready[2];
static SRWLOCK g_master_lock = SRWLOCK_INIT;
static const WCHAR kDpapiKey[] = L"Software\\TheoryCraftOS\\DPAPI";
static const WCHAR kDpapiValue[] = L"MasterKey";

static DWORD master_key(BOOL machine, BYTE out[32]) {
  const int which = machine ? 1 : 0;
  AcquireSRWLockExclusive(&g_master_lock);
  if (g_master_ready[which]) {
    mem_copy(out, g_master[which], 32);
    ReleaseSRWLockExclusive(&g_master_lock);
    return ERROR_SUCCESS;
  }
  HANDLE mutex = CreateMutexW(NULL, FALSE, machine ? L"TheoryCraftOS.DPAPI.Machine" : L"TheoryCraftOS.DPAPI.User");
  if (mutex) WaitForSingleObject(mutex, INFINITE);
  HKEY key;
  DWORD error = RegCreateKeyExW(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, kDpapiKey, 0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &key, NULL);
  if (error == ERROR_SUCCESS) {
    BYTE value[32];
    DWORD type = 0, size = sizeof value;
    error = RegQueryValueExW(key, kDpapiValue, NULL, &type, value, &size);
    if (error == ERROR_SUCCESS && (type != REG_BINARY || size != 32)) error = ERROR_INVALID_DATA;      /* a broken value is not replaced: old blobs may need it */
    else if (error == ERROR_FILE_NOT_FOUND) {
      error = random_bytes(value, 32) ? RegSetValueExW(key, kDpapiValue, 0, REG_BINARY, value, 32) : ERROR_INTERNAL_ERROR;
      if (error == ERROR_SUCCESS) {      /* read it back: that is the key every later call sees */
        size = sizeof value;
        error = RegQueryValueExW(key, kDpapiValue, NULL, &type, value, &size);
        if (error == ERROR_SUCCESS && (type != REG_BINARY || size != 32)) error = ERROR_INVALID_DATA;
      }
    }
    if (error == ERROR_SUCCESS) {
      mem_copy(g_master[which], value, 32);
      g_master_ready[which] = TRUE;
      mem_copy(out, value, 32);
    }
    mem_zero(value, sizeof value);
    RegCloseKey(key);
  }
  if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
  ReleaseSRWLockExclusive(&g_master_lock);
  return error;
}

/* The blob (little-endian):
 *   0   'T' 'C' 'D' 'P'
 *   4   u32 version (1)
 *   8   u32 flags (BLOB_MACHINE: made with the machine's key)
 *   12  salt[16], 28 iv[16]
 *   44  u32 size of the description in bytes (UTF-16 with its NUL, even, at least 2)
 *   48  u32 size of the cipher text (a multiple of 16, at least 16)
 *   52  description, cipher text, then HMAC-SHA-256 [32] of everything before it
 * The cipher key is HMAC(master, "TCOS-DPAPI-ENC" | salt | entropy) and the MAC key HMAC(master, "TCOS-DPAPI-MAC" | salt | entropy). */
#define BLOB_MAGIC 0x50444354u
#define BLOB_VERSION 1u
#define BLOB_MACHINE 1u
#define BLOB_HEADER 52u
#define BLOB_MAX_DESCRIPTION 0x10000u
#define BLOB_MAX_DATA 0x40000000u

static void put32(BYTE *p, DWORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24); }
static DWORD get32(const BYTE *p) { return p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24; }

static BOOL derive_keys(const BYTE master[32], const BYTE salt[16], const DATA_BLOB *entropy, BYTE enc[32], BYTE mac[32]) {
  const BYTE *e = entropy && entropy->cbData ? entropy->pbData : NULL;
  const ULONG en = e ? entropy->cbData : 0;
  return hmac_sha256(master, 32, "TCOS-DPAPI-ENC", 14, salt, 16, e, en, enc) && hmac_sha256(master, 32, "TCOS-DPAPI-MAC", 14, salt, 16, e, en, mac);
}

EXPORT BOOL WINAPI CryptProtectData(DATA_BLOB *in, LPCWSTR description, DATA_BLOB *entropy, PVOID reserved, CRYPTPROTECT_PROMPTSTRUCT *prompt, DWORD flags, DATA_BLOB *out) {
  if (out) { out->cbData = 0; out->pbData = NULL; }
  if (!in || !out || (in->cbData && !in->pbData) || (entropy && entropy->cbData && !entropy->pbData)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (in->cbData > BLOB_MAX_DATA) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  const SIZE_T description_chars = description ? wide_length(description) : 0;
  if (description_chars >= BLOB_MAX_DESCRIPTION / 2) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  const DWORD description_bytes = (DWORD)(description_chars + 1) * 2;
  const BOOL machine = (flags & CRYPTPROTECT_LOCAL_MACHINE) != 0;
  BYTE master[32], enc[32], mac[32], salt_iv[32];
  DWORD error = master_key(machine, master);
  if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
  if (!random_bytes(salt_iv, 32) || !derive_keys(master, salt_iv, entropy, enc, mac)) {
    mem_zero(master, 32);
    SetLastError(NTE_FAIL);
    return FALSE;
  }
  mem_zero(master, 32);
  const DWORD cipher_bytes = (in->cbData / 16 + 1) * 16;
  const SIZE_T total = BLOB_HEADER + (SIZE_T)description_bytes + cipher_bytes + 32;
  BYTE *blob = LocalAlloc(LMEM_FIXED, total);
  if (!blob) { mem_zero(enc, 32); mem_zero(mac, 32); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  put32(blob, BLOB_MAGIC);
  put32(blob + 4, BLOB_VERSION);
  put32(blob + 8, machine ? BLOB_MACHINE : 0);
  mem_copy(blob + 12, salt_iv, 32);
  put32(blob + 44, description_bytes);
  put32(blob + 48, cipher_bytes);
  BYTE *p = blob + BLOB_HEADER;
  for (SIZE_T i = 0; i < description_chars; ++i) { p[2 * i] = (BYTE)description[i]; p[2 * i + 1] = (BYTE)(description[i] >> 8); }
  p[description_bytes - 2] = p[description_bytes - 1] = 0;
  p += description_bytes;
  ULONG written = 0;
  BOOL ok = aes_cbc(TRUE, enc, salt_iv + 16, in->pbData, in->cbData, p, cipher_bytes, &written) && written == cipher_bytes &&
            hmac_sha256(mac, 32, blob, (ULONG)(total - 32), NULL, 0, NULL, 0, blob + total - 32);
  mem_zero(enc, 32);
  mem_zero(mac, 32);
  if (!ok) { LocalFree(blob); SetLastError(NTE_FAIL); return FALSE; }
  out->cbData = (DWORD)total;
  out->pbData = blob;
  SetLastError(ERROR_SUCCESS);      /* (as Windows does) */
  return TRUE;
}

EXPORT BOOL WINAPI CryptUnprotectData(DATA_BLOB *in, LPWSTR *description, DATA_BLOB *entropy, PVOID reserved, CRYPTPROTECT_PROMPTSTRUCT *prompt, DWORD flags, DATA_BLOB *out) {
  if (description) *description = NULL;
  if (out) { out->cbData = 0; out->pbData = NULL; }
  if (!in || !out || (in->cbData && !in->pbData) || (entropy && entropy->cbData && !entropy->pbData)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  const BYTE *blob = in->pbData;
  const SIZE_T size = in->cbData;
  /* The header, checked piece by piece against the size given (the blob is the caller's: anything may be in it). */
  if (size < BLOB_HEADER + 2 + 16 + 32 || get32(blob) != BLOB_MAGIC || get32(blob + 4) != BLOB_VERSION || (get32(blob + 8) & ~BLOB_MACHINE)) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
  const DWORD description_bytes = get32(blob + 44), cipher_bytes = get32(blob + 48);
  if (description_bytes < 2 || description_bytes > BLOB_MAX_DESCRIPTION || description_bytes % 2 || cipher_bytes < 16 || cipher_bytes % 16 || cipher_bytes > BLOB_MAX_DATA + 16 ||
      size != BLOB_HEADER + (SIZE_T)description_bytes + cipher_bytes + 32) {
    SetLastError(ERROR_INVALID_DATA);
    return FALSE;
  }
  const BOOL machine = (get32(blob + 8) & BLOB_MACHINE) != 0;
  BYTE master[32], enc[32], mac[32], check[32];
  DWORD error = master_key(machine, master);
  if (error != ERROR_SUCCESS) { SetLastError(error); return FALSE; }
  BOOL ok = derive_keys(master, blob + 12, entropy, enc, mac);
  mem_zero(master, 32);
  ok = ok && hmac_sha256(mac, 32, blob, (ULONG)(size - 32), NULL, 0, NULL, 0, check);
  mem_zero(mac, 32);
  BYTE difference = 0;
  for (int i = 0; i < 32; ++i) difference |= (BYTE)(check[i] ^ blob[size - 32 + i]);
  if (!ok || difference) { mem_zero(enc, 32); SetLastError(ERROR_INVALID_DATA); return FALSE; }
  BYTE *plain = LocalAlloc(LMEM_FIXED, cipher_bytes);
  if (!plain) { mem_zero(enc, 32); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  ULONG written = 0;
  ok = aes_cbc(FALSE, enc, blob + 28, blob + BLOB_HEADER + description_bytes, cipher_bytes, plain, cipher_bytes, &written);
  mem_zero(enc, 32);
  if (!ok || written > cipher_bytes) { mem_zero(plain, cipher_bytes); LocalFree(plain); SetLastError(ERROR_INVALID_DATA); return FALSE; }
  if (description) {
    WCHAR *text = LocalAlloc(LMEM_FIXED, description_bytes);
    if (!text) { mem_zero(plain, cipher_bytes); LocalFree(plain); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    const BYTE *d = blob + BLOB_HEADER;
    for (DWORD i = 0; i < description_bytes / 2; ++i) text[i] = (WCHAR)(d[2 * i] | d[2 * i + 1] << 8);
    text[description_bytes / 2 - 1] = 0;
    *description = text;
  }
  out->cbData = written;
  out->pbData = plain;
  SetLastError(ERROR_SUCCESS);
  return TRUE;
}

/* ----------------------------------------------------------------------------------------------------------------------------- memory */

static BOOL memory_crypt(LPVOID data, DWORD size, DWORD flags, BOOL encrypt) {
  if (size % CRYPTPROTECTMEMORY_BLOCK_SIZE || flags > CRYPTPROTECTMEMORY_SAME_LOGON || (!data && size)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!size) return TRUE;
  const NTSTATUS s = encrypt ? SystemFunction040(data, size, flags) : SystemFunction041(data, size, flags);      /* (the options are the same numbers) */
  if (s < 0) { SetLastError(s == (NTSTATUS)0xC000000D ? ERROR_INVALID_PARAMETER : ERROR_INTERNAL_ERROR); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI CryptProtectMemory(LPVOID data, DWORD size, DWORD flags) { return memory_crypt(data, size, flags, TRUE); }
EXPORT BOOL WINAPI CryptUnprotectMemory(LPVOID data, DWORD size, DWORD flags) { return memory_crypt(data, size, flags, FALSE); }

EXPORT LPVOID WINAPI CryptMemAlloc(ULONG size) { return LocalAlloc(LMEM_FIXED, size); }
EXPORT LPVOID WINAPI CryptMemRealloc(LPVOID p, ULONG size) { return p ? LocalReAlloc(p, size, LMEM_MOVEABLE) : LocalAlloc(LMEM_FIXED, size); }
EXPORT VOID WINAPI CryptMemFree(LPVOID p) { if (p) LocalFree(p); }

/* ----------------------------------------------------------------------------------------------------------------------------- what is not here */

EXPORT BOOL WINAPI CryptAcquireCertificatePrivateKey(PCCERT_CONTEXT cert, DWORD flags, void *parameters, HCRYPTPROV_OR_NCRYPT_KEY_HANDLE *key, DWORD *key_spec, BOOL *caller_free) {
  if (key) *key = 0;
  if (key_spec) *key_spec = 0;
  if (caller_free) *caller_free = FALSE;
  SetLastError(CRYPT_E_NO_KEY_PROPERTY);      /* no certificate here has a private key */
  return FALSE;
}

EXPORT BOOL WINAPI CryptVerifyCertificateSignatureEx(HCRYPTPROV_LEGACY provider, DWORD encoding, DWORD subject_type, void *subject, DWORD issuer_type, void *issuer, DWORD flags,
                                                     void *extra) {
  SetLastError(NTE_NOT_SUPPORTED);
  return FALSE;
}

/* No message is ever opened here, so every handle is a bad one. */
EXPORT BOOL WINAPI CryptMsgClose(HCRYPTMSG message) {
  if (!message) return TRUE;
  SetLastError(ERROR_INVALID_HANDLE);
  return FALSE;
}
EXPORT BOOL WINAPI CryptMsgGetParam(HCRYPTMSG message, DWORD type, DWORD index, void *data, DWORD *size) {
  if (size) *size = 0;
  SetLastError(message ? CRYPT_E_INVALID_MSG_TYPE : ERROR_INVALID_HANDLE);
  return FALSE;
}

EXPORT BOOL WINAPI CryptQueryObject(DWORD object_type, const void *object, DWORD expected_content, DWORD expected_format, DWORD flags, DWORD *encoding, DWORD *content_type,
                                    DWORD *format_type, HCERTSTORE *store, HCRYPTMSG *message, const void **context) {
  if (encoding) *encoding = 0;
  if (content_type) *content_type = 0;
  if (format_type) *format_type = 0;
  if (store) *store = NULL;
  if (message) *message = NULL;
  if (context) *context = NULL;
  SetLastError(CRYPT_E_NO_MATCH);      /* (Authenticode signatures and PKCS #7 are not read) */
  return FALSE;
}

#include "cert.inc"

/* An entry point of its own: the weak default of stubs.c would lose to the DllMain export of advapi32.dll, which this library links against. */
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) { return TRUE; }
