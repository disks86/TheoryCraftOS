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

/* bcrypt.dll for TheoryCraftOS: Cryptography API: Next Generation, the primitives. The DLL keeps what CNG keeps in the objects of its callers - providers, hash objects, keys,
 * shared secrets - and the kernel does the arithmetic with src/Crypto, one call of NtTcosCrypto per operation (src/Process/WindowsCrypto.cpp); random bytes come from
 * NtQuerySystemInformation (class 0x1000, the system's source). Hashes keep their input until BCryptFinishHash, so a hash object is only as large as what is hashed.
 * Algorithms: MD4, MD5, SHA-1/256/384/512 (and HMAC of each), AES (ECB, CBC, CFB 8 and 128 bit feedback, GCM; padding PKCS #7), RC4, RNG, PBKDF2 and HKDF (BCryptKeyDerivation),
 * ECDSA and ECDH on P-256 and P-384 (key pairs, import/export of the ECC blobs, sign, verify, secret agreement and BCryptDeriveKey with HASH, HMAC and TRUNCATE),
 * RSA signature verification (PKCS #1 v1.5 and PSS, public key blobs). Not here: MD2, DES/3DES/RC2, AES-CCM/CMAC/GMAC, P-521, RSA keys that sign or encrypt, the
 * context/provider registration functions (they stay stubs: src/Windows/stubs/bcrypt.inc). Built freestanding over kernel32.dll and ntdll.dll with the OS's mingw compiler. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winternl.h>
#undef DECLSPEC_IMPORT
#define DECLSPEC_IMPORT
#include <bcrypt.h>

#define EXPORT __declspec(dllexport)

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

__declspec(dllimport) NTSTATUS NTAPI NtTcosCrypto(ULONG64 *arguments);

#define ST_SUCCESS ((NTSTATUS)0)
#define ST_INVALID_HANDLE ((NTSTATUS)0xC0000008)
#define ST_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define ST_NO_MEMORY ((NTSTATUS)0xC0000017)
#define ST_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)
#define ST_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)
#define ST_NOT_FOUND ((NTSTATUS)0xC0000225)
#define ST_INVALID_BUFFER_SIZE ((NTSTATUS)0xC0000206)
#define ST_AUTH_TAG_MISMATCH ((NTSTATUS)0xC000A002)
#define ST_INVALID_SIGNATURE ((NTSTATUS)0xC000A000)
#define ST_ACCESS_DENIED_ ((NTSTATUS)0xC0000022)

/* Operations of NtTcosCrypto, and the algorithm codes the kernel knows. */
#define OP_HASH 1
#define OP_PBKDF2 2
#define OP_HKDF 3
#define OP_AES 4
#define OP_GCM 5
#define OP_EC 6
#define OP_RSA 7
#define K_MD5 0
#define K_SHA1 1
#define K_SHA256 2
#define K_SHA384 3
#define K_SHA512 4
#define K_MD4 6

enum { A_MD4 = 1, A_MD5, A_SHA1, A_SHA256, A_SHA384, A_SHA512, A_AES, A_RC4, A_RNG, A_ECDSA, A_ECDH, A_RSA, A_PBKDF2, A_HKDF };
enum { M_ECB = 0, M_CBC = 1, M_CFB = 2, M_GCM = 4 };

#define MAGIC_ALG 0x41474342u      /* 'BCGA' */
#define MAGIC_HASH 0x48474342u
#define MAGIC_KEY 0x4B474342u
#define MAGIC_SECRET 0x53474342u

typedef struct Alg {
  DWORD magic;
  int id;
  int curve;            /* ECDSA/ECDH: 0 P-256, 1 P-384 */
  int mode;
  DWORD message_block;  /* CFB: bytes of feedback (1 or 16) */
  BOOL hmac;
  BOOL pseudo;
} Alg;

typedef struct Hash {
  DWORD magic;
  Alg *alg;
  BYTE *key;
  DWORD key_size;
  BYTE *data;
  ULONG64 size, capacity;
} Hash;

enum { KEY_SYMMETRIC = 1, KEY_EC, KEY_RSA };
typedef struct Key {
  DWORD magic;
  Alg *alg;
  int kind;
  int mode;
  DWORD message_block;
  BYTE *secret;         /* the key material (heap) */
  DWORD secret_size;
  int hash;             /* KDF keys: the hash set by BCRYPT_HKDF_HASH_ALGORITHM */
  BYTE *salt;           /* HKDF salt, set by HkdfSaltAndFinalize */
  DWORD salt_size;
  BOOL prk;
  BYTE rc4[258];        /* S[256], i, j */
  BYTE priv[48];
  BOOL has_priv, finalized;
  BYTE pub[97];
  BYTE modulus[512];
  DWORD modulus_size;
  BYTE exponent[8];
  DWORD exponent_size;
} Key;

typedef struct Secret {
  DWORD magic;
  BYTE bytes[48];
  DWORD size;
} Secret;

static void *xmemcpy(void *d, const void *s, ULONG64 n) { BYTE *p = d; const BYTE *q = s; while (n--) *p++ = *q++; return d; }
static void xmemset(void *d, int v, ULONG64 n) { BYTE *p = d; while (n--) *p++ = (BYTE)v; }
static int wide_equal(const WCHAR *a, const WCHAR *b) {
  for (;; ++a, ++b) {
    WCHAR x = *a, y = *b;
    if (x >= 'a' && x <= 'z') x -= 32;
    if (y >= 'a' && y <= 'z') y -= 32;
    if (x != y) return 0;
    if (!x) return 1;
  }
}
static DWORD wide_size(const WCHAR *s) { DWORD n = 0; while (s[n]) ++n; return (n + 1) * 2; }
static void *alloc(ULONG64 size) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size); }
static void release(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
/* The memory of keys is wiped before it goes back to the heap. */
static void wipe_and_release(void *p, ULONG64 size) { if (p) { volatile BYTE *b = p; while (size--) *b++ = 0; HeapFree(GetProcessHeap(), 0, p); } }

static NTSTATUS call(ULONG64 op, ULONG64 a1, ULONG64 a2, ULONG64 a3, ULONG64 a4, ULONG64 a5, ULONG64 a6, ULONG64 a7, ULONG64 a8, ULONG64 a9, ULONG64 a10, ULONG64 a11) {
  ULONG64 w[14] = {op, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, 0, 0};
  return NtTcosCrypto(w);
}
#define P(x) ((ULONG64)(ULONG_PTR)(x))

/* ----------------------------------------------------------------------------------------------------------------------------- algorithms */

static const WCHAR *const names[] = {0, L"MD4", L"MD5", L"SHA1", L"SHA256", L"SHA384", L"SHA512", L"AES", L"RC4", L"RNG", L"ECDSA", L"ECDH", L"RSA", L"PBKDF2", L"HKDF"};

static int kernel_hash_of(int id) {
  switch (id) {
    case A_MD4: return K_MD4;
    case A_MD5: return K_MD5;
    case A_SHA1: return K_SHA1;
    case A_SHA256: return K_SHA256;
    case A_SHA384: return K_SHA384;
    case A_SHA512: return K_SHA512;
  }
  return -1;
}
static DWORD hash_length(int id) {
  switch (id) {
    case A_MD4: case A_MD5: return 16;
    case A_SHA1: return 20;
    case A_SHA256: return 32;
    case A_SHA384: return 48;
    case A_SHA512: return 64;
  }
  return 0;
}
static DWORD hash_block(int id) { return id == A_SHA384 || id == A_SHA512 ? 128 : 64; }
static int hash_by_name(const WCHAR *name) {
  for (int id = A_MD4; id <= A_SHA512; ++id) if (wide_equal(name, names[id])) return id;
  return 0;
}

/* The algorithms that have a handle of their own value (BCRYPT_SHA256_ALG_HANDLE ...): a few static objects. */
static Alg g_pseudo[32];
static Alg *pseudo_alg(ULONG_PTR h) {
  int id = 0, mode = M_CBC;
  BOOL hmac = FALSE;
  switch (h) {
    case 0x11: id = A_MD4; break;
    case 0x21: id = A_MD5; break;
    case 0x31: id = A_SHA1; break;
    case 0x41: id = A_SHA256; break;
    case 0x51: id = A_SHA384; break;
    case 0x61: id = A_SHA512; break;
    case 0x71: id = A_RC4; break;
    case 0x81: id = A_RNG; break;
    case 0x91: id = A_MD5; hmac = TRUE; break;
    case 0xa1: id = A_SHA1; hmac = TRUE; break;
    case 0xb1: id = A_SHA256; hmac = TRUE; break;
    case 0xc1: id = A_SHA384; hmac = TRUE; break;
    case 0xd1: id = A_SHA512; hmac = TRUE; break;
    case 0xe1: id = A_RSA; break;
    case 0x131: id = A_MD4; hmac = TRUE; break;
    case 0x1a1: id = A_AES; mode = M_CBC; break;
    case 0x1b1: id = A_AES; mode = M_ECB; break;
    case 0x1c1: id = A_AES; mode = M_CFB; break;
    case 0x1e1: id = A_AES; mode = M_GCM; break;
    case 0x391: id = A_HKDF; break;
    default: return 0;
  }
  Alg *a = &g_pseudo[(h >> 4) & 31];
  if (a->magic != MAGIC_ALG) { a->magic = MAGIC_ALG; a->id = id; a->mode = mode; a->hmac = hmac; a->message_block = id == A_AES && mode == M_CFB ? 1 : 16; a->pseudo = TRUE; }
  return a;
}
static Alg *alg_of(BCRYPT_ALG_HANDLE h) {
  if (!h) return 0;
  if ((ULONG_PTR)h < 0x10000) return pseudo_alg((ULONG_PTR)h);
  Alg *a = (Alg *)h;
  return a->magic == MAGIC_ALG ? a : 0;
}
static Key *key_of(BCRYPT_KEY_HANDLE h) { Key *k = (Key *)h; return k && (ULONG_PTR)h >= 0x10000 && k->magic == MAGIC_KEY ? k : 0; }
static Hash *hash_of(BCRYPT_HASH_HANDLE h) { Hash *k = (Hash *)h; return k && (ULONG_PTR)h >= 0x10000 && k->magic == MAGIC_HASH ? k : 0; }

EXPORT NTSTATUS WINAPI BCryptOpenAlgorithmProvider(BCRYPT_ALG_HANDLE *out, LPCWSTR id, LPCWSTR implementation, ULONG flags) {
  (void)implementation;
  if (!out || !id) return ST_INVALID_PARAMETER;
  if (flags & ~(ULONG)(BCRYPT_ALG_HANDLE_HMAC_FLAG | BCRYPT_HASH_REUSABLE_FLAG | 0x2 /* CAPI_AES_FLAG */ | 0x4 /* MULTI_FLAG */)) return ST_INVALID_PARAMETER;
  int found = 0, curve = 0;
  for (int i = A_MD4; i <= A_HKDF; ++i) if (i != A_ECDSA && i != A_ECDH && wide_equal(id, names[i])) { found = i; break; }
  if (!found && wide_equal(id, L"RSA_SIGN")) found = A_RSA;
  if (!found && (wide_equal(id, L"ECDSA_P256") || wide_equal(id, L"ECDSA_P384"))) { found = A_ECDSA; curve = wide_equal(id, L"ECDSA_P384"); }
  if (!found && (wide_equal(id, L"ECDH_P256") || wide_equal(id, L"ECDH_P384"))) { found = A_ECDH; curve = wide_equal(id, L"ECDH_P384"); }
  if (!found) {
    if (wide_equal(id, L"MD2") || wide_equal(id, L"3DES") || wide_equal(id, L"DES") || wide_equal(id, L"RC2") || wide_equal(id, L"ECDSA_P521") || wide_equal(id, L"ECDH_P521") ||
        wide_equal(id, L"AES-CMAC") || wide_equal(id, L"AES-GMAC") || wide_equal(id, L"DSA") || wide_equal(id, L"DH")) return ST_NOT_SUPPORTED;
    return ST_NOT_FOUND;
  }
  if ((flags & BCRYPT_ALG_HANDLE_HMAC_FLAG) && (found < A_MD4 || found > A_SHA512)) return ST_INVALID_PARAMETER;
  Alg *a = alloc(sizeof(Alg));
  if (!a) return ST_NO_MEMORY;
  a->magic = MAGIC_ALG;
  a->id = found;
  a->curve = curve;
  a->mode = M_CBC;
  a->message_block = found == A_AES ? 1 : 16;
  a->hmac = (flags & BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0;
  *out = a;
  return ST_SUCCESS;
}

EXPORT NTSTATUS WINAPI BCryptCloseAlgorithmProvider(BCRYPT_ALG_HANDLE h, ULONG flags) {
  (void)flags;
  Alg *a = alg_of(h);
  if (!a) return ST_INVALID_HANDLE;
  if (!a->pseudo) { a->magic = 0; release(a); }
  return ST_SUCCESS;
}

/* ----------------------------------------------------------------------------------------------------------------------------- properties */

static NTSTATUS give(const void *value, ULONG size, PUCHAR out, ULONG capacity, ULONG *result) {
  if (!result) return ST_INVALID_PARAMETER;
  *result = size;
  if (!out) return ST_SUCCESS;
  if (capacity < size) return ST_BUFFER_TOO_SMALL;
  xmemcpy(out, value, size);
  return ST_SUCCESS;
}
static NTSTATUS give_dword(DWORD v, PUCHAR out, ULONG capacity, ULONG *result) { return give(&v, 4, out, capacity, result); }
static const WCHAR *mode_name(int mode) {
  return mode == M_ECB ? BCRYPT_CHAIN_MODE_ECB : mode == M_CFB ? BCRYPT_CHAIN_MODE_CFB : mode == M_GCM ? BCRYPT_CHAIN_MODE_GCM : BCRYPT_CHAIN_MODE_CBC;
}
static int mode_by_name(const WCHAR *name) {
  if (wide_equal(name, BCRYPT_CHAIN_MODE_ECB)) return M_ECB;
  if (wide_equal(name, BCRYPT_CHAIN_MODE_CBC)) return M_CBC;
  if (wide_equal(name, BCRYPT_CHAIN_MODE_CFB)) return M_CFB;
  if (wide_equal(name, BCRYPT_CHAIN_MODE_GCM)) return M_GCM;
  return -1;
}

EXPORT NTSTATUS WINAPI BCryptGetProperty(BCRYPT_HANDLE handle, LPCWSTR property, PUCHAR out, ULONG capacity, ULONG *result, ULONG flags) {
  (void)flags;
  if (!handle || !property || !result) return ST_INVALID_PARAMETER;
  Alg *alg = alg_of(handle);
  Key *key = alg ? 0 : key_of(handle);
  Hash *hash = alg || key ? 0 : hash_of(handle);
  Secret *secret = (Secret *)handle;
  if (!alg && !key && !hash) {
    if ((ULONG_PTR)handle < 0x10000 || secret->magic != MAGIC_SECRET) return ST_INVALID_HANDLE;
    if (wide_equal(property, BCRYPT_ALGORITHM_NAME)) return give(L"ECDH", 10, out, capacity, result);
    return ST_NOT_SUPPORTED;
  }
  Alg *a = alg ? alg : key ? key->alg : hash->alg;
  if (wide_equal(property, BCRYPT_ALGORITHM_NAME)) {
    const WCHAR *n = names[a->id];
    WCHAR text[16];
    DWORD len = 0;
    for (; n[len]; ++len) text[len] = n[len];
    if (a->id == A_ECDSA || a->id == A_ECDH) { const WCHAR *s = a->curve ? L"_P384" : L"_P256"; for (int i = 0; s[i]; ++i) text[len++] = s[i]; }
    text[len] = 0;
    return give(text, (len + 1) * 2, out, capacity, result);
  }
  if (wide_equal(property, BCRYPT_OBJECT_LENGTH)) return give_dword(a->id == A_AES ? 0x200 : 0x100, out, capacity, result);
  if (wide_equal(property, BCRYPT_HASH_LENGTH)) { if (!hash_length(a->id)) return ST_NOT_SUPPORTED; return give_dword(hash_length(a->id), out, capacity, result); }
  if (wide_equal(property, BCRYPT_HASH_BLOCK_LENGTH)) { if (!hash_length(a->id)) return ST_NOT_SUPPORTED; return give_dword(hash_block(a->id), out, capacity, result); }
  if (wide_equal(property, L"IsKeyedHash")) return give_dword(a->hmac, out, capacity, result);
  if (wide_equal(property, BCRYPT_BLOCK_LENGTH)) { if (a->id != A_AES) return ST_NOT_SUPPORTED; return give_dword(16, out, capacity, result); }
  if (wide_equal(property, BCRYPT_CHAINING_MODE)) {
    if (a->id != A_AES) return ST_NOT_SUPPORTED;
    const WCHAR *n = mode_name(key ? key->mode : a->mode);
    return give(n, wide_size(n), out, capacity, result);
  }
  if (wide_equal(property, L"MessageBlockLength")) { if (a->id != A_AES) return ST_NOT_SUPPORTED; return give_dword(key ? key->message_block : a->message_block, out, capacity, result); }
  if (wide_equal(property, BCRYPT_KEY_LENGTH) || wide_equal(property, BCRYPT_KEY_STRENGTH)) {
    if (key && key->kind == KEY_SYMMETRIC) return give_dword(key->secret_size * 8, out, capacity, result);
    if (key && key->kind == KEY_RSA) return give_dword(key->modulus_size * 8, out, capacity, result);
    if (a->id == A_ECDSA || a->id == A_ECDH) return give_dword(a->curve ? 384 : 256, out, capacity, result);
    if (a->id == A_AES) return give_dword(256, out, capacity, result);
    return ST_NOT_SUPPORTED;
  }
  if (wide_equal(property, BCRYPT_KEY_LENGTHS)) {
    BCRYPT_KEY_LENGTHS_STRUCT s;
    if (a->id == A_AES) { s.dwMinLength = 128; s.dwMaxLength = 256; s.dwIncrement = 64; }
    else if (a->id == A_RC4) { s.dwMinLength = 40; s.dwMaxLength = 2048; s.dwIncrement = 8; }
    else if (a->id == A_ECDSA || a->id == A_ECDH) { s.dwMinLength = 256; s.dwMaxLength = 384; s.dwIncrement = 128; }
    else if (a->id == A_RSA) { s.dwMinLength = 512; s.dwMaxLength = 16384; s.dwIncrement = 64; }
    else return ST_NOT_SUPPORTED;
    return give(&s, sizeof s, out, capacity, result);
  }
  if (wide_equal(property, BCRYPT_AUTH_TAG_LENGTH)) {
    BCRYPT_AUTH_TAG_LENGTHS_STRUCT s;
    if (a->id != A_AES) return ST_NOT_SUPPORTED;
    s.dwMinLength = 12; s.dwMaxLength = 16; s.dwIncrement = 1;
    return give(&s, sizeof s, out, capacity, result);
  }
  if (wide_equal(property, BCRYPT_SIGNATURE_LENGTH)) {
    if (a->id == A_ECDSA) return give_dword(a->curve ? 96 : 64, out, capacity, result);
    if (key && key->kind == KEY_RSA) return give_dword(key->modulus_size, out, capacity, result);
    return ST_NOT_SUPPORTED;
  }
  if (wide_equal(property, L"ProviderHandle") || wide_equal(property, L"InitializationVector")) return ST_NOT_SUPPORTED;
  return ST_NOT_SUPPORTED;
}

EXPORT NTSTATUS WINAPI BCryptSetProperty(BCRYPT_HANDLE handle, LPCWSTR property, PUCHAR input, ULONG size, ULONG flags) {
  (void)flags;
  if (!handle || !property || (!input && size)) return ST_INVALID_PARAMETER;
  Alg *alg = alg_of(handle);
  Key *key = alg ? 0 : key_of(handle);
  if (!alg && !key) return ST_INVALID_HANDLE;
  Alg *a = alg ? alg : key->alg;
  if (wide_equal(property, BCRYPT_CHAINING_MODE)) {
    if (a->id != A_AES || size < 2 || size > 64 || a->pseudo) return a->pseudo ? ST_ACCESS_DENIED_ : ST_INVALID_PARAMETER;
    WCHAR text[32];
    if (size / 2 >= 32) return ST_INVALID_PARAMETER;
    xmemcpy(text, input, size);
    text[size / 2 - 1 < 31 ? size / 2 : 31] = 0;
    text[31] = 0;
    int m = mode_by_name(text);
    if (m < 0) return ST_NOT_SUPPORTED;
    if (key) key->mode = m; else a->mode = m;
    return ST_SUCCESS;
  }
  if (wide_equal(property, L"MessageBlockLength")) {
    if (size != 4 || a->id != A_AES) return ST_INVALID_PARAMETER;
    DWORD v = *(DWORD *)input;
    if (v != 1 && v != 16) return ST_INVALID_PARAMETER;
    if (key) key->message_block = v; else a->message_block = v;
    return ST_SUCCESS;
  }
  if (key && key->alg->id == A_HKDF) {
    if (wide_equal(property, BCRYPT_HKDF_HASH_ALGORITHM)) {
      WCHAR text[16];
      if (size < 2 || size > sizeof text) return ST_INVALID_PARAMETER;
      xmemset(text, 0, sizeof text);
      xmemcpy(text, input, size > sizeof text - 2 ? sizeof text - 2 : size);
      int h = hash_by_name(text);
      if (!h || h == A_MD4) return ST_NOT_SUPPORTED;
      key->hash = h;
      return ST_SUCCESS;
    }
    if (wide_equal(property, BCRYPT_HKDF_SALT_AND_FINALIZE) || wide_equal(property, BCRYPT_HKDF_PRK_AND_FINALIZE)) {
      release(key->salt);
      key->salt = size ? alloc(size) : 0;
      if (size && !key->salt) return ST_NO_MEMORY;
      if (size) xmemcpy(key->salt, input, size);
      key->salt_size = size;
      key->prk = wide_equal(property, BCRYPT_HKDF_PRK_AND_FINALIZE);
      key->finalized = TRUE;
      return ST_SUCCESS;
    }
  }
  return ST_NOT_SUPPORTED;
}

/* ----------------------------------------------------------------------------------------------------------------------------- random */

EXPORT NTSTATUS WINAPI BCryptGenRandom(BCRYPT_ALG_HANDLE h, PUCHAR buffer, ULONG size, ULONG flags) {
  if (flags & ~(ULONG)(BCRYPT_USE_SYSTEM_PREFERRED_RNG | 1)) return ST_INVALID_PARAMETER;
  if (!buffer && size) return ST_INVALID_PARAMETER;
  if (h) {
    Alg *a = alg_of(h);
    if (!a || a->id != A_RNG) return ST_INVALID_HANDLE;
    if (flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG) return ST_INVALID_PARAMETER;
  } else if (!(flags & BCRYPT_USE_SYSTEM_PREFERRED_RNG)) {
    return ST_INVALID_HANDLE;
  }
  while (size) {
    ULONG n = size > 0x10000 ? 0x10000 : size;
    NTSTATUS s = NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)0x1000, buffer, n, 0);
    if (s < 0) return s;
    buffer += n;
    size -= n;
  }
  return ST_SUCCESS;
}

/* ----------------------------------------------------------------------------------------------------------------------------- hashes */

static NTSTATUS hash_buffer(int id, const BYTE *key, DWORD key_size, BOOL hmac, const BYTE *data, ULONG64 size, BYTE *out) {
  int k = kernel_hash_of(id);
  if (k < 0) return ST_NOT_SUPPORTED;
  static const BYTE nothing = 0;
  return call(OP_HASH, k, hmac ? 1 : 0, P(data ? data : &nothing), size, P(key ? key : &nothing), key_size, P(out), 0, 0, 0, 0);
}

EXPORT NTSTATUS WINAPI BCryptCreateHash(BCRYPT_ALG_HANDLE h, BCRYPT_HASH_HANDLE *out, PUCHAR object, ULONG object_size, PUCHAR secret, ULONG secret_size, ULONG flags) {
  (void)object;
  (void)object_size;
  Alg *a = alg_of(h);
  if (!a) return ST_INVALID_HANDLE;
  if (!out || (flags & ~(ULONG)BCRYPT_HASH_REUSABLE_FLAG) || (!secret && secret_size)) return ST_INVALID_PARAMETER;
  if (!hash_length(a->id)) return ST_NOT_SUPPORTED;
  if (secret_size && !a->hmac) return ST_INVALID_PARAMETER;
  Hash *x = alloc(sizeof(Hash));
  if (!x) return ST_NO_MEMORY;
  x->magic = MAGIC_HASH;
  x->alg = a;
  if (a->hmac) {
    x->key = alloc(secret_size ? secret_size : 1);
    if (!x->key) { release(x); return ST_NO_MEMORY; }
    if (secret_size) xmemcpy(x->key, secret, secret_size);
    x->key_size = secret_size;
  }
  *out = x;
  return ST_SUCCESS;
}

EXPORT NTSTATUS WINAPI BCryptHashData(BCRYPT_HASH_HANDLE h, PUCHAR input, ULONG size, ULONG flags) {
  Hash *x = hash_of(h);
  if (!x) return ST_INVALID_HANDLE;
  if (flags || (!input && size)) return ST_INVALID_PARAMETER;
  if (x->size > 0x7FFFFFFFull - size) return ST_INVALID_PARAMETER;
  if (x->size + size > x->capacity) {
    ULONG64 cap = x->capacity ? x->capacity : 256;
    while (cap < x->size + size) cap *= 2;
    BYTE *n = alloc(cap);
    if (!n) return ST_NO_MEMORY;
    if (x->size) xmemcpy(n, x->data, x->size);
    release(x->data);
    x->data = n;
    x->capacity = cap;
  }
  if (size) xmemcpy(x->data + x->size, input, size);
  x->size += size;
  return ST_SUCCESS;
}

EXPORT NTSTATUS WINAPI BCryptFinishHash(BCRYPT_HASH_HANDLE h, PUCHAR out, ULONG size, ULONG flags) {
  Hash *x = hash_of(h);
  if (!x) return ST_INVALID_HANDLE;
  if (flags || !out || size != hash_length(x->alg->id)) return ST_INVALID_PARAMETER;
  NTSTATUS s = hash_buffer(x->alg->id, x->key, x->key_size, x->alg->hmac, x->data, x->size, out);
  x->size = 0;      /* (the object can be used again: the hash starts over) */
  return s;
}

EXPORT NTSTATUS WINAPI BCryptDuplicateHash(BCRYPT_HASH_HANDLE h, BCRYPT_HASH_HANDLE *out, PUCHAR object, ULONG object_size, ULONG flags) {
  (void)object;
  (void)object_size;
  Hash *x = hash_of(h);
  if (!x) return ST_INVALID_HANDLE;
  if (flags || !out) return ST_INVALID_PARAMETER;
  Hash *c = alloc(sizeof(Hash));
  if (!c) return ST_NO_MEMORY;
  *c = *x;
  c->key = alloc(x->key_size ? x->key_size : 1);
  c->data = alloc(x->capacity ? x->capacity : 1);
  if (!c->key || !c->data) { release(c->key); release(c->data); release(c); return ST_NO_MEMORY; }
  xmemcpy(c->key, x->key, x->key_size);
  xmemcpy(c->data, x->data, x->size);
  *out = c;
  return ST_SUCCESS;
}

EXPORT NTSTATUS WINAPI BCryptDestroyHash(BCRYPT_HASH_HANDLE h) {
  Hash *x = hash_of(h);
  if (!x) return ST_INVALID_HANDLE;
  x->magic = 0;
  wipe_and_release(x->key, x->key_size);
  release(x->data);
  release(x);
  return ST_SUCCESS;
}

EXPORT NTSTATUS WINAPI BCryptHash(BCRYPT_ALG_HANDLE h, PUCHAR secret, ULONG secret_size, PUCHAR input, ULONG size, PUCHAR out, ULONG out_size) {
  Alg *a = alg_of(h);
  if (!a) return ST_INVALID_HANDLE;
  if (!hash_length(a->id)) return ST_NOT_SUPPORTED;
  if (!out || out_size != hash_length(a->id) || (!input && size) || (!secret && secret_size)) return ST_INVALID_PARAMETER;
  return hash_buffer(a->id, secret, secret_size, a->hmac, input, size, out);
}

/* ----------------------------------------------------------------------------------------------------------------------------- symmetric keys */

static void rc4_setup(Key *k) {
  BYTE *s = k->rc4;
  for (int i = 0; i < 256; ++i) s[i] = (BYTE)i;
  int j = 0;
  for (int i = 0; i < 256; ++i) {
    j = (j + s[i] + k->secret[i % k->secret_size]) & 255;
    BYTE t = s[i]; s[i] = s[j]; s[j] = t;
  }
  s[256] = 0;
  s[257] = 0;
}

static NTSTATUS new_symmetric(Alg *a, const BYTE *secret, ULONG size, Key **out) {
  if (!secret || !size) return ST_INVALID_PARAMETER;
  switch (a->id) {
    case A_AES: if (size != 16 && size != 24 && size != 32) return ST_INVALID_PARAMETER; break;
    case A_RC4: if (size < 5 || size > 64) return ST_INVALID_PARAMETER; break;
    case A_PBKDF2: case A_HKDF: break;
    default: if (!(a->hmac && hash_length(a->id))) return ST_NOT_SUPPORTED;
  }
  Key *k = alloc(sizeof(Key));
  if (!k) return ST_NO_MEMORY;
  k->secret = alloc(size);
  if (!k->secret) { release(k); return ST_NO_MEMORY; }
  k->magic = MAGIC_KEY;
  k->alg = a;
  k->kind = KEY_SYMMETRIC;
  k->mode = a->mode;
  k->message_block = a->message_block;
  xmemcpy(k->secret, secret, size);
  k->secret_size = size;
  if (a->id == A_RC4) rc4_setup(k);
  *out = k;
  return ST_SUCCESS;
}

EXPORT NTSTATUS WINAPI BCryptGenerateSymmetricKey(BCRYPT_ALG_HANDLE h, BCRYPT_KEY_HANDLE *out, PUCHAR object, ULONG object_size, PUCHAR secret, ULONG secret_size, ULONG flags) {
  (void)object;
  (void)object_size;
  Alg *a = alg_of(h);
  if (!a) return ST_INVALID_HANDLE;
  if (!out || flags) return ST_INVALID_PARAMETER;
  Key *k;
  NTSTATUS s = new_symmetric(a, secret, secret_size, &k);
  if (s >= 0) *out = k;
  return s;
}

static void key_free(Key *k) {
  k->magic = 0;
  wipe_and_release(k->secret, k->secret_size);
  wipe_and_release(k->salt, k->salt_size);
  xmemset(k, 0, sizeof *k);
  release(k);
}

EXPORT NTSTATUS WINAPI BCryptDestroyKey(BCRYPT_KEY_HANDLE h) {
  Key *k = key_of(h);
  if (!k) return ST_INVALID_HANDLE;
  key_free(k);
  return ST_SUCCESS;
}

EXPORT NTSTATUS WINAPI BCryptDuplicateKey(BCRYPT_KEY_HANDLE h, BCRYPT_KEY_HANDLE *out, PUCHAR object, ULONG object_size, ULONG flags) {
  (void)object;
  (void)object_size;
  Key *k = key_of(h);
  if (!k) return ST_INVALID_HANDLE;
  if (flags || !out) return ST_INVALID_PARAMETER;
  Key *c = alloc(sizeof(Key));
  if (!c) return ST_NO_MEMORY;
  *c = *k;
  c->secret = alloc(k->secret_size ? k->secret_size : 1);
  c->salt = alloc(k->salt_size ? k->salt_size : 1);
  if (!c->secret || !c->salt) { release(c->secret); release(c->salt); release(c); return ST_NO_MEMORY; }
  xmemcpy(c->secret, k->secret, k->secret_size);
  xmemcpy(c->salt, k->salt, k->salt_size);
  *out = c;
  return ST_SUCCESS;
}

/* ----------------------------------------------------------------------------------------------------------------------------- blobs */

#define MAGIC_KEY_DATA 0x4d42444bu      /* 'KDBM' */
#define MAGIC_ECDSA_PUB(c) ((c) ? 0x33534345u : 0x31534345u)
#define MAGIC_ECDSA_PRIV(c) ((c) ? 0x34534345u : 0x32534345u)
#define MAGIC_ECDH_PUB(c) ((c) ? 0x334b4345u : 0x314b4345u)
#define MAGIC_ECDH_PRIV(c) ((c) ? 0x344b4345u : 0x324b4345u)
#define MAGIC_RSA_PUB 0x31415352u

static DWORD field_size(int curve) { return curve ? 48 : 32; }

static NTSTATUS key_from_secret(Alg *a, const BYTE *secret, DWORD size, Key **out) { return new_symmetric(a, secret, size, out); }

EXPORT NTSTATUS WINAPI BCryptImportKey(BCRYPT_ALG_HANDLE h, BCRYPT_KEY_HANDLE import_key, LPCWSTR type, BCRYPT_KEY_HANDLE *out, PUCHAR object, ULONG object_size, PUCHAR input,
                                       ULONG size, ULONG flags) {
  (void)object;
  (void)object_size;
  Alg *a = alg_of(h);
  if (!a) return ST_INVALID_HANDLE;
  if (!type || !out || !input || flags) return ST_INVALID_PARAMETER;
  if (import_key) return ST_NOT_SUPPORTED;      /* (wrapped keys) */
  if (!wide_equal(type, BCRYPT_KEY_DATA_BLOB)) return ST_NOT_SUPPORTED;
  BCRYPT_KEY_DATA_BLOB_HEADER *hd = (BCRYPT_KEY_DATA_BLOB_HEADER *)input;
  if (size < sizeof *hd || hd->dwMagic != BCRYPT_KEY_DATA_BLOB_MAGIC || hd->dwVersion != 1 || hd->cbKeyData > size - sizeof *hd) return ST_INVALID_PARAMETER;
  Key *k;
  NTSTATUS s = key_from_secret(a, input + sizeof *hd, hd->cbKeyData, &k);
  if (s >= 0) *out = k;
  return s;
}

static NTSTATUS import_ec(Alg *a, const BYTE *input, ULONG size, BOOL private_blob, Key **out) {
  BCRYPT_ECCKEY_BLOB *hd = (BCRYPT_ECCKEY_BLOB *)input;
  if (size < sizeof *hd) return ST_INVALID_PARAMETER;
  DWORD fs = field_size(a->curve);
  DWORD want = a->id == A_ECDSA ? (private_blob ? MAGIC_ECDSA_PRIV(a->curve) : MAGIC_ECDSA_PUB(a->curve)) : (private_blob ? MAGIC_ECDH_PRIV(a->curve) : MAGIC_ECDH_PUB(a->curve));
  if (hd->dwMagic != want || hd->cbKey != fs) return ST_INVALID_PARAMETER;
  ULONG need = sizeof *hd + 2 * fs + (private_blob ? fs : 0);
  if (size < need) return ST_INVALID_PARAMETER;
  Key *k = alloc(sizeof(Key));
  if (!k) return ST_NO_MEMORY;
  k->magic = MAGIC_KEY;
  k->alg = a;
  k->kind = KEY_EC;
  k->pub[0] = 4;
  xmemcpy(k->pub + 1, input + sizeof *hd, 2 * fs);
  if (private_blob) { xmemcpy(k->priv, input + sizeof *hd + 2 * fs, fs); k->has_priv = TRUE; }
  k->finalized = TRUE;
  ULONG64 w[14] = {OP_EC, 5 /* verify needs a valid point: ask for ECDH with a throw-away scalar */, 0};
  (void)w;
  /* The point must be on the curve: an ECDH with the key's own scalar (or any scalar) rejects points that are not. */
  BYTE one[48], shared[48];
  xmemset(one, 0, sizeof one);
  one[fs - 1] = 1;
  if (call(OP_EC, 3, a->curve, P(one), fs, P(k->pub), 1 + 2 * fs, 0, 0, P(shared), 0, 0) < 0) { key_free(k); return ST_INVALID_PARAMETER; }
  if (private_blob) {
    BYTE derived[97];
    if (call(OP_EC, 2, a->curve, P(k->priv), fs, 0, 0, 0, 0, P(derived), 0, 0) < 0) { key_free(k); return ST_INVALID_PARAMETER; }
    for (DWORD i = 0; i < 1 + 2 * fs; ++i) if (derived[i] != k->pub[i]) { key_free(k); return ST_INVALID_PARAMETER; }
  }
  *out = k;
  return ST_SUCCESS;
}

static NTSTATUS import_rsa_public(Alg *a, const BYTE *input, ULONG size, Key **out) {
  BCRYPT_RSAKEY_BLOB *hd = (BCRYPT_RSAKEY_BLOB *)input;
  if (size < sizeof *hd || hd->Magic != MAGIC_RSA_PUB) return ST_INVALID_PARAMETER;
  if (hd->cbPublicExp == 0 || hd->cbPublicExp > 8 || hd->cbModulus == 0 || hd->cbModulus > 512) return ST_INVALID_PARAMETER;
  if (size - sizeof *hd < (ULONG64)hd->cbPublicExp + hd->cbModulus) return ST_INVALID_PARAMETER;
  Key *k = alloc(sizeof(Key));
  if (!k) return ST_NO_MEMORY;
  k->magic = MAGIC_KEY;
  k->alg = a;
  k->kind = KEY_RSA;
  k->exponent_size = hd->cbPublicExp;
  k->modulus_size = hd->cbModulus;
  xmemcpy(k->exponent, input + sizeof *hd, hd->cbPublicExp);
  xmemcpy(k->modulus, input + sizeof *hd + hd->cbPublicExp, hd->cbModulus);
  k->finalized = TRUE;
  *out = k;
  return ST_SUCCESS;
}

EXPORT NTSTATUS WINAPI BCryptImportKeyPair(BCRYPT_ALG_HANDLE h, BCRYPT_KEY_HANDLE import_key, LPCWSTR type, BCRYPT_KEY_HANDLE *out, PUCHAR input, ULONG size, ULONG flags) {
  Alg *a = alg_of(h);
  if (!a) return ST_INVALID_HANDLE;
  if (!type || !out || !input || flags) return ST_INVALID_PARAMETER;
  if (import_key) return ST_NOT_SUPPORTED;
  Key *k = 0;
  NTSTATUS s;
  if (a->id == A_ECDSA || a->id == A_ECDH) {
    if (wide_equal(type, BCRYPT_ECCPUBLIC_BLOB) || wide_equal(type, BCRYPT_PUBLIC_KEY_BLOB)) s = import_ec(a, input, size, FALSE, &k);
    else if (wide_equal(type, BCRYPT_ECCPRIVATE_BLOB) || wide_equal(type, BCRYPT_PRIVATE_KEY_BLOB)) s = import_ec(a, input, size, TRUE, &k);
    else return ST_NOT_SUPPORTED;
  } else if (a->id == A_RSA) {
    if (wide_equal(type, BCRYPT_RSAPUBLIC_BLOB) || wide_equal(type, BCRYPT_PUBLIC_KEY_BLOB)) s = import_rsa_public(a, input, size, &k);
    else return ST_NOT_SUPPORTED;
  } else {
    return ST_NOT_SUPPORTED;
  }
  if (s >= 0) *out = k;
  return s;
}

EXPORT NTSTATUS WINAPI BCryptExportKey(BCRYPT_KEY_HANDLE h, BCRYPT_KEY_HANDLE export_key, LPCWSTR type, PUCHAR out, ULONG capacity, ULONG *result, ULONG flags) {
  Key *k = key_of(h);
  if (!k) return ST_INVALID_HANDLE;
  if (!type || !result || flags) return ST_INVALID_PARAMETER;
  if (export_key) return ST_NOT_SUPPORTED;
  if (k->kind == KEY_SYMMETRIC) {
    if (!wide_equal(type, BCRYPT_KEY_DATA_BLOB)) return ST_NOT_SUPPORTED;
    if (k->alg->id == A_PBKDF2 || k->alg->id == A_HKDF) return ST_NOT_SUPPORTED;
    ULONG need = sizeof(BCRYPT_KEY_DATA_BLOB_HEADER) + k->secret_size;
    *result = need;
    if (!out) return ST_SUCCESS;
    if (capacity < need) return ST_BUFFER_TOO_SMALL;
    BCRYPT_KEY_DATA_BLOB_HEADER hd = {BCRYPT_KEY_DATA_BLOB_MAGIC, 1, k->secret_size};
    xmemcpy(out, &hd, sizeof hd);
    xmemcpy(out + sizeof hd, k->secret, k->secret_size);
    return ST_SUCCESS;
  }
  if (k->kind == KEY_EC) {
    BOOL priv = wide_equal(type, BCRYPT_ECCPRIVATE_BLOB) || wide_equal(type, BCRYPT_PRIVATE_KEY_BLOB);
    if (!priv && !wide_equal(type, BCRYPT_ECCPUBLIC_BLOB) && !wide_equal(type, BCRYPT_PUBLIC_KEY_BLOB)) return ST_NOT_SUPPORTED;
    if (priv && !k->has_priv) return ST_INVALID_PARAMETER;
    if (!k->finalized) return ST_INVALID_HANDLE;
    DWORD fs = field_size(k->alg->curve);
    ULONG need = sizeof(BCRYPT_ECCKEY_BLOB) + 2 * fs + (priv ? fs : 0);
    *result = need;
    if (!out) return ST_SUCCESS;
    if (capacity < need) return ST_BUFFER_TOO_SMALL;
    BCRYPT_ECCKEY_BLOB hd;
    int c = k->alg->curve;
    hd.dwMagic = k->alg->id == A_ECDSA ? (priv ? MAGIC_ECDSA_PRIV(c) : MAGIC_ECDSA_PUB(c)) : (priv ? MAGIC_ECDH_PRIV(c) : MAGIC_ECDH_PUB(c));
    hd.cbKey = fs;
    xmemcpy(out, &hd, sizeof hd);
    xmemcpy(out + sizeof hd, k->pub + 1, 2 * fs);
    if (priv) xmemcpy(out + sizeof hd + 2 * fs, k->priv, fs);
    return ST_SUCCESS;
  }
  if (k->kind == KEY_RSA) {
    if (!wide_equal(type, BCRYPT_RSAPUBLIC_BLOB) && !wide_equal(type, BCRYPT_PUBLIC_KEY_BLOB)) return ST_NOT_SUPPORTED;
    ULONG need = sizeof(BCRYPT_RSAKEY_BLOB) + k->exponent_size + k->modulus_size;
    *result = need;
    if (!out) return ST_SUCCESS;
    if (capacity < need) return ST_BUFFER_TOO_SMALL;
    BCRYPT_RSAKEY_BLOB hd = {MAGIC_RSA_PUB, k->modulus_size * 8, k->exponent_size, k->modulus_size, 0, 0};
    xmemcpy(out, &hd, sizeof hd);
    xmemcpy(out + sizeof hd, k->exponent, k->exponent_size);
    xmemcpy(out + sizeof hd + k->exponent_size, k->modulus, k->modulus_size);
    return ST_SUCCESS;
  }
  return ST_INVALID_HANDLE;
}

/* ----------------------------------------------------------------------------------------------------------------------------- key pairs */

EXPORT NTSTATUS WINAPI BCryptGenerateKeyPair(BCRYPT_ALG_HANDLE h, BCRYPT_KEY_HANDLE *out, ULONG length, ULONG flags) {
  Alg *a = alg_of(h);
  if (!a) return ST_INVALID_HANDLE;
  if (!out || flags) return ST_INVALID_PARAMETER;
  if (a->id != A_ECDSA && a->id != A_ECDH) return ST_NOT_SUPPORTED;      /* (RSA key generation is not done) */
  if (length && length != (a->curve ? 384u : 256u)) return ST_INVALID_PARAMETER;
  Key *k = alloc(sizeof(Key));
  if (!k) return ST_NO_MEMORY;
  k->magic = MAGIC_KEY;
  k->alg = a;
  k->kind = KEY_EC;
  *out = k;
  return ST_SUCCESS;
}

EXPORT NTSTATUS WINAPI BCryptFinalizeKeyPair(BCRYPT_KEY_HANDLE h, ULONG flags) {
  Key *k = key_of(h);
  if (!k) return ST_INVALID_HANDLE;
  if (flags) return ST_INVALID_PARAMETER;
  if (k->kind != KEY_EC || k->finalized) return ST_INVALID_HANDLE;
  DWORD fs = field_size(k->alg->curve);
  NTSTATUS s = call(OP_EC, 1, k->alg->curve, 0, 0, 0, 0, 0, 0, P(k->priv), 0, 0);
  if (s < 0) return s;
  s = call(OP_EC, 2, k->alg->curve, P(k->priv), fs, 0, 0, 0, 0, P(k->pub), 0, 0);
  if (s < 0) return s;
  k->has_priv = TRUE;
  k->finalized = TRUE;
  return ST_SUCCESS;
}

/* ----------------------------------------------------------------------------------------------------------------------------- encryption */

static NTSTATUS aes_run(Key *k, int mode, BOOL encrypt, BYTE *iv, const BYTE *in, BYTE *out, ULONG size) {
  int kernel_mode = mode == M_ECB ? 0 : mode == M_CBC ? 1 : (k->message_block == 1 ? 3 : 2);
  static const BYTE nothing = 0;
  return call(OP_AES, kernel_mode, encrypt, P(k->secret), k->secret_size, kernel_mode ? P(iv) : 0, P(in ? in : &nothing), P(out), size, 0, 0, 0);
}

static NTSTATUS crypt(Key *k, PUCHAR in, ULONG size, VOID *info, PUCHAR iv, ULONG iv_size, PUCHAR out, ULONG capacity, ULONG *result, ULONG flags, BOOL encrypt) {
  if (!k) return ST_INVALID_HANDLE;
  if (!result || (!in && size)) return ST_INVALID_PARAMETER;
  if (k->kind != KEY_SYMMETRIC) return ST_NOT_SUPPORTED;
  if (k->alg->id == A_RC4) {
    if (flags || info) return ST_INVALID_PARAMETER;
    *result = size;
    if (!out) return ST_SUCCESS;
    if (capacity < size) return ST_BUFFER_TOO_SMALL;
    BYTE *s = k->rc4;
    int i = s[256], j = s[257];
    for (ULONG n = 0; n < size; ++n) {
      i = (i + 1) & 255;
      j = (j + s[i]) & 255;
      BYTE t = s[i]; s[i] = s[j]; s[j] = t;
      out[n] = in[n] ^ s[(BYTE)(s[i] + s[j])];
    }
    s[256] = (BYTE)i;
    s[257] = (BYTE)j;
    return ST_SUCCESS;
  }
  if (k->alg->id != A_AES) return ST_NOT_SUPPORTED;
  int mode = k->mode;
  if (mode == M_GCM) {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO *ai = info;
    if (flags || !ai || ai->cbSize != sizeof *ai || ai->dwInfoVersion != 1) return ST_INVALID_PARAMETER;
    if (!ai->pbNonce || ai->cbNonce != 12 || !ai->pbTag || ai->cbTag < 4 || ai->cbTag > 16 || (!ai->pbAuthData && ai->cbAuthData)) return ST_INVALID_PARAMETER;
    if (ai->dwFlags & (BCRYPT_AUTH_MODE_CHAIN_CALLS_FLAG | BCRYPT_AUTH_MODE_IN_PROGRESS_FLAG)) return ST_NOT_SUPPORTED;
    if (k->secret_size == 24) return ST_NOT_SUPPORTED;
    *result = size;
    if (!out) return ST_SUCCESS;
    if (capacity < size) return ST_BUFFER_TOO_SMALL;
    BYTE tag[16];
    static const BYTE nothing = 0;
    if (!encrypt) {
      if (ai->cbTag != 16) return ST_NOT_SUPPORTED;      /* (a shortened tag cannot be checked by the kernel's full-tag open) */
      xmemcpy(tag, ai->pbTag, 16);
    }
    NTSTATUS s = call(OP_GCM, encrypt, P(k->secret), k->secret_size, P(ai->pbNonce), 12, P(ai->pbAuthData ? ai->pbAuthData : &nothing), ai->cbAuthData, P(in ? in : &nothing), size, P(out), P(tag));
    if (s < 0) return s;
    if (encrypt) xmemcpy(ai->pbTag, tag, ai->cbTag);
    return ST_SUCCESS;
  }
  if (info) return ST_INVALID_PARAMETER;
  if (flags & ~(ULONG)BCRYPT_BLOCK_PADDING) return ST_INVALID_PARAMETER;
  BOOL pad = (flags & BCRYPT_BLOCK_PADDING) != 0;
  if (mode != M_ECB) { if (!iv || iv_size < 16) return ST_INVALID_PARAMETER; }
  else if (iv) return ST_INVALID_PARAMETER;
  if (mode == M_CFB && pad) return ST_INVALID_PARAMETER;
  BOOL whole = mode != M_CFB;
  ULONG padded = size;
  if (encrypt) {
    if (pad) padded = (size / 16 + 1) * 16;
    else if (whole && size % 16) return ST_INVALID_BUFFER_SIZE;
    *result = padded;
  } else {
    if (whole && (size % 16 || (pad && size == 0))) return ST_INVALID_BUFFER_SIZE;
    *result = size;      /* (shortened by the padding once the data is decrypted) */
  }
  if (!out) return ST_SUCCESS;
  BYTE *plain = alloc((encrypt ? padded : size) + 16);
  if (!plain) return ST_NO_MEMORY;
  NTSTATUS s;
  BYTE chain[16];
  if (iv) xmemcpy(chain, iv, 16);
  if (encrypt) {
    if (capacity < padded) { release(plain); return ST_BUFFER_TOO_SMALL; }
    if (size) xmemcpy(plain, in, size);
    for (ULONG i = size; i < padded; ++i) plain[i] = (BYTE)(padded - size);
    s = aes_run(k, mode, TRUE, chain, plain, plain, padded);
    if (s >= 0) { xmemcpy(out, plain, padded); if (iv) xmemcpy(iv, chain, 16); }
  } else {
    if (capacity < size && pad == FALSE) { release(plain); return ST_BUFFER_TOO_SMALL; }
    s = aes_run(k, mode, FALSE, chain, in, plain, size);
    if (s >= 0) {
      ULONG n = size;
      if (pad) {
        BYTE p = size ? plain[size - 1] : 0;
        if (p == 0 || p > 16 || p > size) s = ST_INVALID_PARAMETER;
        else { for (ULONG i = size - p; i < size; ++i) if (plain[i] != p) s = ST_INVALID_PARAMETER; n = size - p; }
      }
      if (s >= 0) {
        *result = n;
        if (capacity < n) s = ST_BUFFER_TOO_SMALL;
        else { xmemcpy(out, plain, n); if (iv) xmemcpy(iv, chain, 16); }
      }
    }
  }
  wipe_and_release(plain, (encrypt ? padded : size) + 16);
  return s;
}

EXPORT NTSTATUS WINAPI BCryptEncrypt(BCRYPT_KEY_HANDLE h, PUCHAR in, ULONG size, VOID *info, PUCHAR iv, ULONG iv_size, PUCHAR out, ULONG capacity, ULONG *result, ULONG flags) {
  return crypt(key_of(h), in, size, info, iv, iv_size, out, capacity, result, flags, TRUE);
}
EXPORT NTSTATUS WINAPI BCryptDecrypt(BCRYPT_KEY_HANDLE h, PUCHAR in, ULONG size, VOID *info, PUCHAR iv, ULONG iv_size, PUCHAR out, ULONG capacity, ULONG *result, ULONG flags) {
  return crypt(key_of(h), in, size, info, iv, iv_size, out, capacity, result, flags, FALSE);
}

/* ----------------------------------------------------------------------------------------------------------------------------- signatures */

static int hash_of_padding(const WCHAR *name) { int id = name ? hash_by_name(name) : 0; return id && id != A_MD4 ? id : 0; }

EXPORT NTSTATUS WINAPI BCryptSignHash(BCRYPT_KEY_HANDLE h, VOID *padding, PUCHAR digest, ULONG digest_size, PUCHAR out, ULONG capacity, ULONG *result, ULONG flags) {
  (void)padding;
  Key *k = key_of(h);
  if (!k) return ST_INVALID_HANDLE;
  if (!result || !digest || !digest_size || flags) return ST_INVALID_PARAMETER;
  if (k->kind != KEY_EC || k->alg->id != A_ECDSA) return ST_NOT_SUPPORTED;
  if (!k->finalized || !k->has_priv) return ST_INVALID_HANDLE;
  DWORD fs = field_size(k->alg->curve);
  *result = 2 * fs;
  if (!out) return ST_SUCCESS;
  if (capacity < 2 * fs) return ST_BUFFER_TOO_SMALL;
  return call(OP_EC, 4, k->alg->curve, P(k->priv), fs, P(digest), digest_size, 0, 0, P(out), 0, 0);
}

EXPORT NTSTATUS WINAPI BCryptVerifySignature(BCRYPT_KEY_HANDLE h, VOID *padding, PUCHAR digest, ULONG digest_size, PUCHAR signature, ULONG signature_size, ULONG flags) {
  Key *k = key_of(h);
  if (!k) return ST_INVALID_HANDLE;
  if (!digest || !digest_size || !signature) return ST_INVALID_PARAMETER;
  if (k->kind == KEY_EC && k->alg->id == A_ECDSA) {
    if (flags) return ST_INVALID_PARAMETER;
    if (!k->finalized) return ST_INVALID_HANDLE;
    return call(OP_EC, 5, k->alg->curve, P(k->pub), 1 + 2 * field_size(k->alg->curve), P(digest), digest_size, P(signature), signature_size, 0, 0, 0);
  }
  if (k->kind == KEY_RSA) {
    int pss = flags == BCRYPT_PAD_PSS;
    if (flags != BCRYPT_PAD_PKCS1 && !pss) return ST_INVALID_PARAMETER;
    if (!padding) return ST_INVALID_PARAMETER;
    int id = hash_of_padding(*(const WCHAR **)padding);
    if (!id) return ST_INVALID_PARAMETER;
    if (digest_size != hash_length(id)) return ST_INVALID_PARAMETER;
    if (pss && ((BCRYPT_PSS_PADDING_INFO *)padding)->cbSalt != hash_length(id)) return ST_NOT_SUPPORTED;
    return call(OP_RSA, pss, kernel_hash_of(id), P(k->modulus), k->modulus_size, P(k->exponent), k->exponent_size, P(digest), P(signature), signature_size, 0, 0);
  }
  return ST_NOT_SUPPORTED;
}

/* ----------------------------------------------------------------------------------------------------------------------------- secret agreement and key derivation */

EXPORT NTSTATUS WINAPI BCryptSecretAgreement(BCRYPT_KEY_HANDLE priv, BCRYPT_KEY_HANDLE pub, BCRYPT_SECRET_HANDLE *out, ULONG flags) {
  Key *a = key_of(priv), *b = key_of(pub);
  if (!a || !b) return ST_INVALID_HANDLE;
  if (!out || flags) return ST_INVALID_PARAMETER;
  if (a->kind != KEY_EC || b->kind != KEY_EC || a->alg->id != A_ECDH || b->alg->id != A_ECDH || a->alg->curve != b->alg->curve) return ST_INVALID_HANDLE;
  if (!a->has_priv || !a->finalized || !b->finalized) return ST_INVALID_HANDLE;
  DWORD fs = field_size(a->alg->curve);
  Secret *s = alloc(sizeof(Secret));
  if (!s) return ST_NO_MEMORY;
  NTSTATUS st = call(OP_EC, 3, a->alg->curve, P(a->priv), fs, P(b->pub), 1 + 2 * fs, 0, 0, P(s->bytes), 0, 0);
  if (st < 0) { release(s); return st; }
  s->magic = MAGIC_SECRET;
  s->size = fs;
  *out = s;
  return ST_SUCCESS;
}

EXPORT NTSTATUS WINAPI BCryptDestroySecret(BCRYPT_SECRET_HANDLE h) {
  Secret *s = (Secret *)h;
  if (!s || (ULONG_PTR)h < 0x10000 || s->magic != MAGIC_SECRET) return ST_INVALID_HANDLE;
  wipe_and_release(s, sizeof *s);
  return ST_SUCCESS;
}

static BCryptBuffer *find_buffer(BCryptBufferDesc *list, ULONG type) {
  if (!list) return 0;
  for (ULONG i = 0; i < list->cBuffers; ++i) if (list->pBuffers[i].BufferType == type) return &list->pBuffers[i];
  return 0;
}

static int hash_from_buffer(BCryptBuffer *b, int fallback) {
  if (!b) return fallback;
  WCHAR text[16];
  xmemset(text, 0, sizeof text);
  xmemcpy(text, b->pvBuffer, b->cbBuffer > sizeof text - 2 ? sizeof text - 2 : b->cbBuffer);
  return hash_by_name(text);
}

EXPORT NTSTATUS WINAPI BCryptDeriveKey(BCRYPT_SECRET_HANDLE h, LPCWSTR kdf, BCryptBufferDesc *params, PUCHAR out, ULONG capacity, ULONG *result, ULONG flags) {
  Secret *sec = (Secret *)h;
  if (!sec || (ULONG_PTR)h < 0x10000 || sec->magic != MAGIC_SECRET) return ST_INVALID_HANDLE;
  if (!kdf || !result || flags) return ST_INVALID_PARAMETER;
  if (wide_equal(kdf, BCRYPT_KDF_RAW_SECRET)) {
    *result = sec->size;
    if (!out) return ST_SUCCESS;
    if (capacity < sec->size) return ST_BUFFER_TOO_SMALL;
    for (DWORD i = 0; i < sec->size; ++i) out[i] = sec->bytes[sec->size - 1 - i];      /* (the raw secret is little endian) */
    return ST_SUCCESS;
  }
  BOOL is_hmac = wide_equal(kdf, BCRYPT_KDF_HMAC);
  if (!wide_equal(kdf, BCRYPT_KDF_HASH) && !is_hmac) return ST_NOT_SUPPORTED;
  int id = hash_from_buffer(find_buffer(params, KDF_HASH_ALGORITHM), A_SHA256);
  if (!id || id == A_MD4) return ST_INVALID_PARAMETER;
  DWORD hl = hash_length(id);
  *result = hl;
  if (!out) return ST_SUCCESS;
  if (capacity < hl) return ST_BUFFER_TOO_SMALL;
  BCryptBuffer *pre = find_buffer(params, KDF_SECRET_PREPEND), *app = find_buffer(params, KDF_SECRET_APPEND), *hk = find_buffer(params, KDF_HMAC_KEY);
  ULONG pre_n = pre ? pre->cbBuffer : 0, app_n = app ? app->cbBuffer : 0;
  BYTE *m = alloc(pre_n + sec->size + app_n + 1);
  if (!m) return ST_NO_MEMORY;
  if (pre_n) xmemcpy(m, pre->pvBuffer, pre_n);
  for (DWORD i = 0; i < sec->size; ++i) m[pre_n + i] = sec->bytes[sec->size - 1 - i];      /* (the secret is used little endian, as CNG does) */
  if (app_n) xmemcpy(m + pre_n + sec->size, app->pvBuffer, app_n);
  NTSTATUS s;
  if (is_hmac) s = hash_buffer(id, hk ? hk->pvBuffer : 0, hk ? hk->cbBuffer : 0, TRUE, m, pre_n + sec->size + app_n, out);
  else s = hash_buffer(id, 0, 0, FALSE, m, pre_n + sec->size + app_n, out);
  wipe_and_release(m, pre_n + sec->size + app_n + 1);
  return s;
}

EXPORT NTSTATUS WINAPI BCryptDeriveKeyPBKDF2(BCRYPT_ALG_HANDLE prf, PUCHAR password, ULONG password_size, PUCHAR salt, ULONG salt_size, ULONGLONG iterations, PUCHAR out,
                                             ULONG out_size, ULONG flags) {
  Alg *a = alg_of(prf);
  if (!a) return ST_INVALID_HANDLE;
  if (flags || !out || !out_size || !iterations || (!password && password_size) || (!salt && salt_size)) return ST_INVALID_PARAMETER;
  if (!a->hmac || kernel_hash_of(a->id) < 0 || a->id == A_MD4) return ST_NOT_SUPPORTED;
  static const BYTE nothing = 0;
  return call(OP_PBKDF2, kernel_hash_of(a->id), P(password ? password : &nothing), password_size, P(salt ? salt : &nothing), salt_size, iterations, P(out), out_size, 0, 0, 0);
}

EXPORT NTSTATUS WINAPI BCryptKeyDerivation(BCRYPT_KEY_HANDLE h, BCryptBufferDesc *params, PUCHAR out, ULONG out_size, ULONG *result, ULONG flags) {
  Key *k = key_of(h);
  if (!k) return ST_INVALID_HANDLE;
  if (!out || !out_size || !result || flags) return ST_INVALID_PARAMETER;
  static const BYTE nothing = 0;
  if (k->alg->id == A_PBKDF2) {
    BCryptBuffer *it = find_buffer(params, KDF_ITERATION_COUNT), *salt = find_buffer(params, KDF_SALT);
    int id = hash_from_buffer(find_buffer(params, KDF_HASH_ALGORITHM), A_SHA1);
    if (!it || it->cbBuffer != 8 || !id || id == A_MD4) return ST_INVALID_PARAMETER;
    *result = out_size;
    return call(OP_PBKDF2, kernel_hash_of(id), P(k->secret), k->secret_size, salt ? P(salt->pvBuffer) : P(&nothing), salt ? salt->cbBuffer : 0, *(ULONGLONG *)it->pvBuffer, P(out), out_size, 0, 0, 0);
  }
  if (k->alg->id == A_HKDF) {
    BCryptBuffer *info = find_buffer(params, KDF_HKDF_INFO);
    int id = k->hash > 0 ? k->hash : A_SHA256;
    if (!k->finalized) return ST_INVALID_PARAMETER;
    *result = out_size;
    return call(OP_HKDF, kernel_hash_of(id), k->prk, P(k->secret), k->secret_size, k->salt ? P(k->salt) : P(&nothing), k->salt_size, info ? P(info->pvBuffer) : P(&nothing), info ? info->cbBuffer : 0, out_size,
                P(out), 0);
  }
  return ST_NOT_SUPPORTED;
}

/* ----------------------------------------------------------------------------------------------------------------------------- the rest */

EXPORT VOID WINAPI BCryptFreeBuffer(PVOID buffer) { release(buffer); }

EXPORT NTSTATUS WINAPI BCryptGetFipsAlgorithmMode(BOOLEAN *enabled) {
  if (!enabled) return ST_INVALID_PARAMETER;
  *enabled = FALSE;
  return ST_SUCCESS;
}


/* BCryptEnumAlgorithms: the operations are the CRYPT_*_OPERATION flags (cipher 1, hash 2, asymmetric encryption 4, secret agreement 8, signature 16, rng 32). */
EXPORT NTSTATUS WINAPI BCryptEnumAlgorithms(ULONG operations, ULONG *count, BCRYPT_ALGORITHM_IDENTIFIER **list, ULONG flags) {
  static const struct { const WCHAR *name; ULONG cls, ops; } table[] = {
      {L"AES", 1, 1}, {L"RC4", 1, 1}, {L"MD4", 2, 2}, {L"MD5", 2, 2}, {L"SHA1", 2, 2}, {L"SHA256", 2, 2}, {L"SHA384", 2, 2}, {L"SHA512", 2, 2},
      {L"RSA", 3, 4 | 16}, {L"ECDH_P256", 4, 8}, {L"ECDH_P384", 4, 8}, {L"ECDSA_P256", 3, 16}, {L"ECDSA_P384", 3, 16}, {L"RNG", 6, 32}, {L"PBKDF2", 5, 0}, {L"HKDF", 5, 0}};
  if (!count || !list || flags) return ST_INVALID_PARAMETER;
  ULONG n = 0, chars = 0;
  for (ULONG i = 0; i < sizeof table / sizeof table[0]; ++i) if (!operations || (table[i].ops & operations)) { ++n; chars += wide_size(table[i].name); }
  BYTE *block = alloc(n * sizeof(BCRYPT_ALGORITHM_IDENTIFIER) + chars + 1);
  if (!block) return ST_NO_MEMORY;
  BCRYPT_ALGORITHM_IDENTIFIER *id = (BCRYPT_ALGORITHM_IDENTIFIER *)block;
  WCHAR *text = (WCHAR *)(block + n * sizeof(BCRYPT_ALGORITHM_IDENTIFIER));
  ULONG at = 0;
  for (ULONG i = 0; i < sizeof table / sizeof table[0]; ++i) {
    if (operations && !(table[i].ops & operations)) continue;
    DWORD sz = wide_size(table[i].name);
    xmemcpy(text, table[i].name, sz);
    id[at].pszName = text;
    id[at].dwClass = table[i].cls;
    id[at].dwFlags = 0;
    text += sz / 2;
    ++at;
  }
  *count = n;
  *list = id;
  return ST_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) { (void)module; (void)reason; (void)reserved; return TRUE; }
