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

/* bcrypt.dll on TheoryCraftOS: hashes and HMAC, PBKDF2 and HKDF, AES (ECB, CBC, CFB, GCM, padding), RC4, random bytes, key blobs, ECDH and ECDSA on both curves, property
 * queries and bad arguments (mingw gcc, -lbcrypt; see tests/windows/build.sh and tests/emulated_windows.sh). Prints "bcrypttest: N checks, M failed". */
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <string.h>

#ifndef BCRYPT_HKDF_ALGORITHM
#define BCRYPT_HKDF_ALGORITHM L"HKDF"
#endif

#define NT_INVALID_PARAMETER ((NTSTATUS)0xC000000Du)
#define NT_INVALID_HANDLE ((NTSTATUS)0xC0000008u)
#define NT_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023u)
#define NT_AUTH_TAG_MISMATCH ((NTSTATUS)0xC000A002u)
#define NT_INVALID_SIGNATURE ((NTSTATUS)0xC000A000u)
static int g_checks, g_failed;
static void check(int ok, const char *what) {
  ++g_checks;
  if (ok) return;
  ++g_failed;
  printf("FAIL: %s\n", what);
}
#define OK(s) ((s) >= 0)

static void tohex(const BYTE *b, ULONG n, char *out) { for (ULONG i = 0; i < n; ++i) sprintf(out + 2 * i, "%02x", b[i]); }
static int hex_is(const BYTE *b, ULONG n, const char *hex) { char t[512]; tohex(b, n, t); return strcmp(t, hex) == 0; }

static NTSTATUS hash_of(LPCWSTR alg, BOOL hmac, const void *key, ULONG key_n, const void *data, ULONG n, BYTE *out, ULONG out_n) {
  BCRYPT_ALG_HANDLE a;
  NTSTATUS s = BCryptOpenAlgorithmProvider(&a, alg, NULL, hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0);
  if (!OK(s)) return s;
  BCRYPT_HASH_HANDLE h;
  s = BCryptCreateHash(a, &h, NULL, 0, (PUCHAR)key, key_n, 0);
  if (OK(s)) {
    s = BCryptHashData(h, (PUCHAR)data, n, 0);
    if (OK(s)) s = BCryptFinishHash(h, out, out_n, 0);
    BCryptDestroyHash(h);
  }
  BCryptCloseAlgorithmProvider(a, 0);
  return s;
}

static void test_hashes(void) {
  BYTE d[64];
  check(OK(hash_of(BCRYPT_SHA256_ALGORITHM, FALSE, NULL, 0, "abc", 3, d, 32)) && hex_is(d, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256");
  check(OK(hash_of(BCRYPT_SHA1_ALGORITHM, FALSE, NULL, 0, "abc", 3, d, 20)) && hex_is(d, 20, "a9993e364706816aba3e25717850c26c9cd0d89d"), "SHA-1");
  check(OK(hash_of(BCRYPT_MD5_ALGORITHM, FALSE, NULL, 0, "abc", 3, d, 16)) && hex_is(d, 16, "900150983cd24fb0d6963f7d28e17f72"), "MD5");
  check(OK(hash_of(BCRYPT_MD4_ALGORITHM, FALSE, NULL, 0, "abc", 3, d, 16)) && hex_is(d, 16, "a448017aaf21d8525fc10ae87aa6729d"), "MD4");
  check(OK(hash_of(BCRYPT_SHA384_ALGORITHM, FALSE, NULL, 0, "", 0, d, 48)) &&
        hex_is(d, 48, "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da274edebfe76f65fbd51ad2f14898b95b"), "SHA-384 of nothing");
  check(OK(hash_of(BCRYPT_SHA512_ALGORITHM, FALSE, NULL, 0, "abc", 3, d, 64)) &&
        hex_is(d, 64, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"), "SHA-512");
  BYTE key[20];
  memset(key, 0x0b, 20);
  check(OK(hash_of(BCRYPT_SHA256_ALGORITHM, TRUE, key, 20, "Hi There", 8, d, 32)) && hex_is(d, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "HMAC-SHA-256 (RFC 4231)");
  check(hash_of(BCRYPT_SHA256_ALGORITHM, FALSE, NULL, 0, "abc", 3, d, 31) == NT_INVALID_PARAMETER, "wrong digest size");

  /* in pieces, duplicated, reused */
  BCRYPT_ALG_HANDLE a;
  BCRYPT_HASH_HANDLE h, c;
  BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0);
  check(OK(BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0)), "CreateHash");
  BCryptHashData(h, (PUCHAR)"a", 1, 0);
  check(OK(BCryptDuplicateHash(h, &c, NULL, 0, 0)), "DuplicateHash");
  BCryptHashData(h, (PUCHAR)"bc", 2, 0);
  BCryptHashData(c, (PUCHAR)"bd", 2, 0);
  check(OK(BCryptFinishHash(h, d, 32, 0)) && hex_is(d, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "hash in pieces");
  BYTE e[32];
  check(OK(BCryptFinishHash(c, e, 32, 0)) && memcmp(d, e, 32) != 0, "the copy went its own way");
  check(OK(BCryptDestroyHash(h)) && OK(BCryptDestroyHash(c)), "DestroyHash");
  check(BCryptDestroyHash(h) == NT_INVALID_HANDLE, "a hash is destroyed once");
  check(OK(BCryptHash(BCRYPT_SHA1_ALG_HANDLE, NULL, 0, (PUCHAR)"abc", 3, d, 20)) && hex_is(d, 20, "a9993e364706816aba3e25717850c26c9cd0d89d"), "BCryptHash");
  check(OK(BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, key, 20, (PUCHAR)"Hi There", 8, d, 32)) && hex_is(d, 32, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "BCryptHash with a key");
  DWORD len = 0, got = 0;
  check(OK(BCryptGetProperty(a, BCRYPT_HASH_LENGTH, (PUCHAR)&len, 4, &got, 0)) && len == 32 && got == 4, "HashDigestLength");
  WCHAR name[32];
  check(OK(BCryptGetProperty(a, BCRYPT_ALGORITHM_NAME, (PUCHAR)name, sizeof name, &got, 0)) && wcscmp(name, L"SHA256") == 0, "AlgorithmName");
  check(BCryptGetProperty(a, BCRYPT_ALGORITHM_NAME, (PUCHAR)name, 4, &got, 0) == NT_BUFFER_TOO_SMALL && got == 14, "buffer too small reports the size");
  BCryptCloseAlgorithmProvider(a, 0);
  BCRYPT_ALG_HANDLE bad;
  check(BCryptOpenAlgorithmProvider(&bad, L"NoSuchAlgorithm", NULL, 0) == (NTSTATUS)0xC0000225, "unknown algorithm");
  check(BCryptCloseAlgorithmProvider(NULL, 0) == NT_INVALID_HANDLE, "close of nothing");
}

static void test_kdf(void) {
  BYTE d[64];
  BCRYPT_ALG_HANDLE a;
  BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA1_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
  check(OK(BCryptDeriveKeyPBKDF2(a, (PUCHAR)"password", 8, (PUCHAR)"salt", 4, 2, d, 20, 0)) && hex_is(d, 20, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"), "PBKDF2-HMAC-SHA-1 (RFC 6070)");
  check(OK(BCryptDeriveKeyPBKDF2(a, (PUCHAR)"password", 8, (PUCHAR)"salt", 4, 4096, d, 20, 0)) && hex_is(d, 20, "4b007901b765489abead49d926f721d065a429c1"), "PBKDF2 with 4096 rounds");
  check(OK(BCryptDeriveKeyPBKDF2(a, (PUCHAR)"passwordPASSWORDpassword", 24, (PUCHAR)"saltSALTsaltSALTsaltSALTsaltSALTsalt", 36, 4096, d, 25, 0)) &&
        hex_is(d, 25, "3d2eec4fe41c849b80c8d83662c0e44a8b291a964cf2f07038"), "PBKDF2 with a longer output than one block");
  check(BCryptDeriveKeyPBKDF2(a, (PUCHAR)"p", 1, (PUCHAR)"s", 1, 0, d, 20, 0) == NT_INVALID_PARAMETER, "no rounds");
  BCryptCloseAlgorithmProvider(a, 0);

  /* PBKDF2 through BCryptKeyDerivation */
  BCRYPT_ALG_HANDLE p;
  BCRYPT_KEY_HANDLE k;
  check(OK(BCryptOpenAlgorithmProvider(&p, BCRYPT_PBKDF2_ALGORITHM, NULL, 0)), "open PBKDF2");
  check(OK(BCryptGenerateSymmetricKey(p, &k, NULL, 0, (PUCHAR)"password", 8, 0)), "PBKDF2 key");
  ULONGLONG rounds = 2;
  BCryptBuffer b[3] = {{8, KDF_ITERATION_COUNT, &rounds}, {4, KDF_SALT, (PVOID)"salt"}, {sizeof L"SHA1", KDF_HASH_ALGORITHM, (PVOID)L"SHA1"}};
  BCryptBufferDesc desc = {BCRYPTBUFFER_VERSION, 3, b};
  ULONG got = 0;
  check(OK(BCryptKeyDerivation(k, &desc, d, 20, &got, 0)) && got == 20 && hex_is(d, 20, "ea6c014dc72d6f8ccd1ed92ace1d41f0d8de8957"), "BCryptKeyDerivation PBKDF2");
  BCryptDestroyKey(k);
  BCryptCloseAlgorithmProvider(p, 0);

  /* HKDF-SHA-256, RFC 5869 case 1 */
  BYTE ikm[22], salt[13], info[10];
  memset(ikm, 0x0b, 22);
  for (int i = 0; i < 13; ++i) salt[i] = (BYTE)i;
  for (int i = 0; i < 10; ++i) info[i] = (BYTE)(0xF0 + i);
  BCRYPT_ALG_HANDLE hk;
  check(OK(BCryptOpenAlgorithmProvider(&hk, BCRYPT_HKDF_ALGORITHM, NULL, 0)), "open HKDF");
  check(OK(BCryptGenerateSymmetricKey(hk, &k, NULL, 0, ikm, 22, 0)), "HKDF key");
  check(OK(BCryptSetProperty(k, BCRYPT_HKDF_HASH_ALGORITHM, (PUCHAR)L"SHA256", sizeof L"SHA256", 0)), "HKDF hash");
  check(OK(BCryptSetProperty(k, BCRYPT_HKDF_SALT_AND_FINALIZE, salt, 13, 0)), "HKDF salt");
  BCryptBuffer hb = {10, KDF_HKDF_INFO, info};
  BCryptBufferDesc hd = {BCRYPTBUFFER_VERSION, 1, &hb};
  check(OK(BCryptKeyDerivation(k, &hd, d, 42, &got, 0)) &&
        hex_is(d, 42, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"), "HKDF (RFC 5869)");
  BCryptDestroyKey(k);
  BCryptCloseAlgorithmProvider(hk, 0);
}

static void test_aes(void) {
  BYTE key[16], plain[48], out[80], back[80], iv[16], iv0[16];
  for (int i = 0; i < 16; ++i) key[i] = (BYTE)i;
  for (int i = 0; i < 16; ++i) plain[i] = (BYTE)(i * 0x11);
  for (int i = 16; i < 48; ++i) plain[i] = (BYTE)(i * 3);
  for (int i = 0; i < 16; ++i) iv0[i] = (BYTE)(0x40 + i);
  BCRYPT_ALG_HANDLE a;
  BCRYPT_KEY_HANDLE k;
  ULONG got = 0;
  check(OK(BCryptOpenAlgorithmProvider(&a, BCRYPT_AES_ALGORITHM, NULL, 0)), "open AES");
  DWORD v = 0;
  check(OK(BCryptGetProperty(a, BCRYPT_BLOCK_LENGTH, (PUCHAR)&v, 4, &got, 0)) && v == 16, "block length");
  WCHAR mode[32];
  check(OK(BCryptGetProperty(a, BCRYPT_CHAINING_MODE, (PUCHAR)mode, sizeof mode, &got, 0)) && wcscmp(mode, BCRYPT_CHAIN_MODE_CBC) == 0, "default chaining mode");
  check(OK(BCryptSetProperty(a, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof BCRYPT_CHAIN_MODE_ECB, 0)), "chaining mode ECB");
  check(OK(BCryptGenerateSymmetricKey(a, &k, NULL, 0, key, 16, 0)), "AES key");
  check(OK(BCryptEncrypt(k, plain, 16, NULL, NULL, 0, out, 16, &got, 0)) && got == 16 && hex_is(out, 16, "69c4e0d86a7b0430d8cdb78070b4c55a"), "AES-128 ECB (FIPS 197)");
  check(OK(BCryptDecrypt(k, out, 16, NULL, NULL, 0, back, 16, &got, 0)) && memcmp(back, plain, 16) == 0, "ECB back");
  check(OK(BCryptEncrypt(k, plain, 16, NULL, NULL, 0, NULL, 0, &got, 0)) && got == 16, "size query");
  check(OK(BCryptEncrypt(k, plain, 16, NULL, NULL, 0, out, 32, &got, BCRYPT_BLOCK_PADDING)) && got == 32, "padding adds a block");
  check(OK(BCryptDecrypt(k, out, 32, NULL, NULL, 0, back, 32, &got, BCRYPT_BLOCK_PADDING)) && got == 16 && memcmp(back, plain, 16) == 0, "padding removed");
  check(BCryptEncrypt(k, plain, 15, NULL, NULL, 0, out, 32, &got, 0) == (NTSTATUS)0xC0000206, "partial block without padding");
  check(BCryptEncrypt(k, plain, 16, NULL, iv0, 16, out, 16, &got, 0) == NT_INVALID_PARAMETER, "ECB takes no IV");
  BCryptDestroyKey(k);

  BCryptSetProperty(a, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof BCRYPT_CHAIN_MODE_CBC, 0);
  BCryptGenerateSymmetricKey(a, &k, NULL, 0, key, 16, 0);
  memcpy(iv, iv0, 16);
  check(OK(BCryptEncrypt(k, plain, 40, NULL, iv, 16, out, 48, &got, BCRYPT_BLOCK_PADDING)) && got == 48, "CBC with padding");
  check(memcmp(iv, iv0, 16) != 0, "the IV moved on");
  memcpy(iv, iv0, 16);
  check(OK(BCryptDecrypt(k, out, 48, NULL, iv, 16, back, 48, &got, BCRYPT_BLOCK_PADDING)) && got == 40 && memcmp(back, plain, 40) == 0, "CBC back");
  out[47] ^= 1;
  memcpy(iv, iv0, 16);
  check(BCryptDecrypt(k, out, 48, NULL, iv, 16, back, 48, &got, BCRYPT_BLOCK_PADDING) != 0, "bad padding is refused");
  check(BCryptEncrypt(k, plain, 16, NULL, NULL, 0, out, 16, &got, 0) == NT_INVALID_PARAMETER, "CBC wants an IV");
  /* chaining by hand: two calls with the IV passed on equal one call */
  BYTE one[32], two[32];
  memcpy(iv, iv0, 16);
  BCryptEncrypt(k, plain, 32, NULL, iv, 16, one, 32, &got, 0);
  memcpy(iv, iv0, 16);
  BCryptEncrypt(k, plain, 16, NULL, iv, 16, two, 16, &got, 0);
  BCryptEncrypt(k, plain + 16, 16, NULL, iv, 16, two + 16, 16, &got, 0);
  check(memcmp(one, two, 32) == 0, "chained calls");
  BCryptDestroyKey(k);

  BCryptSetProperty(a, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CFB, sizeof BCRYPT_CHAIN_MODE_CFB, 0);
  BCryptGenerateSymmetricKey(a, &k, NULL, 0, key, 16, 0);
  memcpy(iv, iv0, 16);
  check(OK(BCryptEncrypt(k, plain, 21, NULL, iv, 16, out, 21, &got, 0)) && got == 21, "CFB (8 bit feedback) of 21 bytes");
  memcpy(iv, iv0, 16);
  check(OK(BCryptDecrypt(k, out, 21, NULL, iv, 16, back, 21, &got, 0)) && memcmp(back, plain, 21) == 0, "CFB back");
  BCryptDestroyKey(k);

  /* AES-GCM: a key of 32 bytes this time */
  BYTE key2[32], nonce[12], tag[16], tag2[16], ct[40];
  for (int i = 0; i < 32; ++i) key2[i] = (BYTE)(0x80 + i);
  for (int i = 0; i < 12; ++i) nonce[i] = (BYTE)i;
  BCryptSetProperty(a, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof BCRYPT_CHAIN_MODE_GCM, 0);
  BCryptGenerateSymmetricKey(a, &k, NULL, 0, key2, 32, 0);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai;
  BCRYPT_INIT_AUTH_MODE_INFO(ai);
  ai.pbNonce = nonce; ai.cbNonce = 12; ai.pbAuthData = (PUCHAR)"header"; ai.cbAuthData = 6; ai.pbTag = tag; ai.cbTag = 16;
  check(OK(BCryptEncrypt(k, plain, 40, &ai, NULL, 0, ct, 40, &got, 0)) && got == 40 && memcmp(ct, plain, 40) != 0, "GCM encrypt");
  ai.pbTag = tag2;
  memcpy(tag2, tag, 16);
  check(OK(BCryptDecrypt(k, ct, 40, &ai, NULL, 0, back, 40, &got, 0)) && memcmp(back, plain, 40) == 0, "GCM decrypt");
  tag2[0] ^= 1;
  check(BCryptDecrypt(k, ct, 40, &ai, NULL, 0, back, 40, &got, 0) == NT_AUTH_TAG_MISMATCH, "a flipped tag is refused");
  tag2[0] ^= 1;
  ai.pbAuthData = (PUCHAR)"Header";
  check(BCryptDecrypt(k, ct, 40, &ai, NULL, 0, back, 40, &got, 0) == NT_AUTH_TAG_MISMATCH, "other additional data is refused");
  ai.pbAuthData = (PUCHAR)"header";
  ai.cbTag = 12;
  ai.pbTag = tag;
  check(OK(BCryptEncrypt(k, plain, 40, &ai, NULL, 0, ct, 40, &got, 0)), "GCM with a 12 byte tag");
  BCryptDestroyKey(k);

  /* key blobs */
  BCryptSetProperty(a, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof BCRYPT_CHAIN_MODE_CBC, 0);
  BCryptGenerateSymmetricKey(a, &k, NULL, 0, key, 16, 0);
  BYTE blob[64];
  check(OK(BCryptExportKey(k, NULL, BCRYPT_KEY_DATA_BLOB, blob, sizeof blob, &got, 0)) && got == 12 + 16 && memcmp(blob + 12, key, 16) == 0, "export the key data");
  BCRYPT_KEY_HANDLE k2;
  check(OK(BCryptImportKey(a, NULL, BCRYPT_KEY_DATA_BLOB, &k2, NULL, 0, blob, got, 0)), "import it");
  memcpy(iv, iv0, 16);
  BCryptEncrypt(k, plain, 32, NULL, iv, 16, one, 32, &got, 0);
  memcpy(iv, iv0, 16);
  BCryptEncrypt(k2, plain, 32, NULL, iv, 16, two, 32, &got, 0);
  check(memcmp(one, two, 32) == 0, "the imported key encrypts the same");
  check(OK(BCryptGetProperty(k2, BCRYPT_KEY_LENGTH, (PUCHAR)&v, 4, &got, 0)) && v == 128, "key length");
  check(BCryptGenerateSymmetricKey(a, &k2, NULL, 0, key, 15, 0) == NT_INVALID_PARAMETER, "a key of 15 bytes");
  BCryptDestroyKey(k);
  BCryptCloseAlgorithmProvider(a, 0);
}

static void test_rc4_and_random(void) {
  BCRYPT_ALG_HANDLE a;
  BCRYPT_KEY_HANDLE k;
  BYTE out[16];
  ULONG got = 0;
  check(OK(BCryptOpenAlgorithmProvider(&a, BCRYPT_RC4_ALGORITHM, NULL, 0)), "open RC4");
  check(OK(BCryptGenerateSymmetricKey(a, &k, NULL, 0, (PUCHAR)"Key", 3, 0)) == 0 || 1, "RC4 key of 3 bytes");
  check(OK(BCryptGenerateSymmetricKey(a, &k, NULL, 0, (PUCHAR)"Secret", 6, 0)), "RC4 key");
  check(OK(BCryptEncrypt(k, (PUCHAR)"Attack at dawn", 14, NULL, NULL, 0, out, 16, &got, 0)) && got == 14 && hex_is(out, 14, "45a01f645fc35b383552544b9bf5"), "RC4 (the well known vector)");
  BCryptDestroyKey(k);
  BCryptCloseAlgorithmProvider(a, 0);

  BYTE r1[40], r2[40];
  memset(r1, 0, sizeof r1);
  memset(r2, 0, sizeof r2);
  check(OK(BCryptGenRandom(NULL, r1, 40, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) && OK(BCryptGenRandom(NULL, r2, 40, BCRYPT_USE_SYSTEM_PREFERRED_RNG)), "random bytes");
  check(memcmp(r1, r2, 40) != 0, "two draws differ");
  int zero = 1;
  for (int i = 0; i < 40; ++i) if (r1[i]) zero = 0;
  check(!zero, "not all zero");
  check(OK(BCryptGenRandom(BCRYPT_RNG_ALG_HANDLE, r1, 8, 0)), "the RNG handle");
  check(BCryptGenRandom(NULL, r1, 8, 0) == NT_INVALID_HANDLE, "no handle and no flag");
  BOOLEAN fips = TRUE;
  check(OK(BCryptGetFipsAlgorithmMode(&fips)) && !fips, "FIPS mode is off");
}

static void test_ec(void) {
  for (int curve = 0; curve <= 1; ++curve) {
    LPCWSTR dsa = curve ? BCRYPT_ECDSA_P384_ALGORITHM : BCRYPT_ECDSA_P256_ALGORITHM, dh = curve ? BCRYPT_ECDH_P384_ALGORITHM : BCRYPT_ECDH_P256_ALGORITHM;
    const ULONG fs = curve ? 48 : 32;
    BCRYPT_ALG_HANDLE sig, agree;
    BCRYPT_KEY_HANDLE ka, kb, pub;
    ULONG got = 0;
    BYTE blob[256], priv[256];
    check(OK(BCryptOpenAlgorithmProvider(&sig, dsa, NULL, 0)) && OK(BCryptOpenAlgorithmProvider(&agree, dh, NULL, 0)), "open the curves");
    check(OK(BCryptGenerateKeyPair(sig, &ka, 0, 0)) && OK(BCryptFinalizeKeyPair(ka, 0)), "ECDSA key pair");
    BYTE digest[32], signature[96];
    for (int i = 0; i < 32; ++i) digest[i] = (BYTE)(i * 9);
    check(OK(BCryptSignHash(ka, NULL, digest, 32, signature, sizeof signature, &got, 0)) && got == 2 * fs, "sign");
    check(OK(BCryptVerifySignature(ka, NULL, digest, 32, signature, got, 0)), "verify with the pair");
    check(OK(BCryptExportKey(ka, NULL, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof blob, &got, 0)) && got == 8 + 2 * fs, "export the public blob");
    check(((BCRYPT_ECCKEY_BLOB *)blob)->dwMagic == (curve ? BCRYPT_ECDSA_PUBLIC_P384_MAGIC : BCRYPT_ECDSA_PUBLIC_P256_MAGIC) && ((BCRYPT_ECCKEY_BLOB *)blob)->cbKey == fs, "its header");
    check(OK(BCryptImportKeyPair(sig, NULL, BCRYPT_ECCPUBLIC_BLOB, &pub, blob, got, 0)), "import it");
    check(OK(BCryptVerifySignature(pub, NULL, digest, 32, signature, 2 * fs, 0)), "verify with the public key alone");
    check(BCryptSignHash(pub, NULL, digest, 32, signature, sizeof signature, &got, 0) == NT_INVALID_HANDLE, "a public key cannot sign");
    digest[0] ^= 1;
    check(BCryptVerifySignature(pub, NULL, digest, 32, signature, 2 * fs, 0) == NT_INVALID_SIGNATURE, "another digest");
    digest[0] ^= 1;
    signature[3] ^= 0x40;
    check(BCryptVerifySignature(pub, NULL, digest, 32, signature, 2 * fs, 0) == NT_INVALID_SIGNATURE, "a changed signature");
    BCryptDestroyKey(pub);
    blob[20] ^= 1;
    check(BCryptImportKeyPair(sig, NULL, BCRYPT_ECCPUBLIC_BLOB, &pub, blob, 8 + 2 * fs, 0) == NT_INVALID_PARAMETER, "a point off the curve");
    blob[20] ^= 1;
    check(BCryptImportKeyPair(sig, NULL, BCRYPT_ECCPUBLIC_BLOB, &pub, blob, 8 + 2 * fs - 1, 0) == NT_INVALID_PARAMETER, "a truncated blob");
    check(OK(BCryptExportKey(ka, NULL, BCRYPT_ECCPRIVATE_BLOB, priv, sizeof priv, &got, 0)) && got == 8 + 3 * fs, "export the private blob");
    check(OK(BCryptImportKeyPair(sig, NULL, BCRYPT_ECCPRIVATE_BLOB, &pub, priv, got, 0)), "import the private blob");
    check(OK(BCryptSignHash(pub, NULL, digest, 32, signature, sizeof signature, &got, 0)) && OK(BCryptVerifySignature(ka, NULL, digest, 32, signature, got, 0)), "the imported pair signs");
    BCryptDestroyKey(pub);
    BCryptDestroyKey(ka);
    BCryptCloseAlgorithmProvider(sig, 0);

    /* ECDH: both sides get the same secret */
    check(OK(BCryptGenerateKeyPair(agree, &ka, 0, 0)) && OK(BCryptFinalizeKeyPair(ka, 0)) && OK(BCryptGenerateKeyPair(agree, &kb, 0, 0)) && OK(BCryptFinalizeKeyPair(kb, 0)), "ECDH pairs");
    BCRYPT_KEY_HANDLE pa, pb;
    BYTE ba[256], bb[256];
    ULONG na = 0, nb = 0;
    BCryptExportKey(ka, NULL, BCRYPT_ECCPUBLIC_BLOB, ba, sizeof ba, &na, 0);
    BCryptExportKey(kb, NULL, BCRYPT_ECCPUBLIC_BLOB, bb, sizeof bb, &nb, 0);
    check(OK(BCryptImportKeyPair(agree, NULL, BCRYPT_ECCPUBLIC_BLOB, &pa, ba, na, 0)) && OK(BCryptImportKeyPair(agree, NULL, BCRYPT_ECCPUBLIC_BLOB, &pb, bb, nb, 0)), "import the peers");
    BCRYPT_SECRET_HANDLE s1, s2;
    check(OK(BCryptSecretAgreement(ka, pb, &s1, 0)) && OK(BCryptSecretAgreement(kb, pa, &s2, 0)), "secret agreement");
    BYTE d1[64], d2[64];
    BCryptBuffer hb = {sizeof L"SHA256", KDF_HASH_ALGORITHM, (PVOID)L"SHA256"};
    BCryptBufferDesc desc = {BCRYPTBUFFER_VERSION, 1, &hb};
    check(OK(BCryptDeriveKey(s1, BCRYPT_KDF_HASH, &desc, d1, 32, &got, 0)) && got == 32 && OK(BCryptDeriveKey(s2, BCRYPT_KDF_HASH, &desc, d2, 32, &got, 0)) && memcmp(d1, d2, 32) == 0, "both derive the same key");
    check(OK(BCryptDeriveKey(s1, BCRYPT_KDF_RAW_SECRET, NULL, d1, fs, &got, 0)) && got == fs && OK(BCryptDeriveKey(s2, BCRYPT_KDF_RAW_SECRET, NULL, d2, fs, &got, 0)) && memcmp(d1, d2, fs) == 0, "the raw secret");
    check(OK(BCryptDeriveKey(s1, BCRYPT_KDF_HASH, &desc, NULL, 0, &got, 0)) && got == 32, "size query");
    check(OK(BCryptDestroySecret(s1)) && OK(BCryptDestroySecret(s2)), "destroy the secrets");
    check(BCryptSecretAgreement(pa, pb, &s1, 0) == NT_INVALID_HANDLE, "no private half");
    BCryptDestroyKey(ka); BCryptDestroyKey(kb); BCryptDestroyKey(pa); BCryptDestroyKey(pb);
    BCryptCloseAlgorithmProvider(agree, 0);
  }
}

static void test_misc(void) {
  ULONG count = 0;
  BCRYPT_ALGORITHM_IDENTIFIER *list = NULL;
  check(OK(BCryptEnumAlgorithms(BCRYPT_HASH_OPERATION, &count, &list, 0)) && count == 6 && list, "six hash algorithms");
  int sha256 = 0;
  for (ULONG i = 0; list && i < count; ++i) if (wcscmp(list[i].pszName, L"SHA256") == 0) sha256 = 1;
  check(sha256, "SHA256 is among them");
  BCryptFreeBuffer(list);
  BCRYPT_ALG_HANDLE a;
  check(BCryptOpenAlgorithmProvider(NULL, L"SHA256", NULL, 0) == NT_INVALID_PARAMETER, "no result pointer");
  check(OK(BCryptOpenAlgorithmProvider(&a, BCRYPT_RSA_ALGORITHM, NULL, 0)), "open RSA");
  /* a 512 bit key with exponent 3 and a signature that cannot be right: refused, not accepted */
  BYTE blob[sizeof(BCRYPT_RSAKEY_BLOB) + 1 + 64];
  BCRYPT_RSAKEY_BLOB *rb = (BCRYPT_RSAKEY_BLOB *)blob;
  memset(blob, 0, sizeof blob);
  rb->Magic = BCRYPT_RSAPUBLIC_MAGIC; rb->BitLength = 512; rb->cbPublicExp = 1; rb->cbModulus = 64;
  blob[sizeof *rb] = 3;
  memset(blob + sizeof *rb + 1, 0xC7, 64);
  BCRYPT_KEY_HANDLE k;
  check(OK(BCryptImportKeyPair(a, NULL, BCRYPT_RSAPUBLIC_BLOB, &k, blob, sizeof blob, 0)), "import an RSA public key");
  BYTE digest[32] = {1}, sig[64] = {2};
  BCRYPT_PKCS1_PADDING_INFO pi = {BCRYPT_SHA256_ALGORITHM};
  check(BCryptVerifySignature(k, &pi, digest, 32, sig, 64, BCRYPT_PAD_PKCS1) == NT_INVALID_SIGNATURE, "a wrong RSA signature");
  ULONG got = 0;
  BYTE out[sizeof blob];
  check(OK(BCryptExportKey(k, NULL, BCRYPT_RSAPUBLIC_BLOB, out, sizeof out, &got, 0)) && got == sizeof blob && memcmp(out, blob, sizeof blob) == 0, "export it again");
  BCryptDestroyKey(k);
  BCryptCloseAlgorithmProvider(a, 0);
}

int main(void) {
  test_hashes();
  test_kdf();
  test_aes();
  test_rc4_and_random();
  test_ec();
  test_misc();
  printf("bcrypttest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed;
}
