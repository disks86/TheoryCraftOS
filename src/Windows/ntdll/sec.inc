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
/* ntdll.dll: the SIDs derived from names (the rest of the Rtl security API is in security.c, built with the mingw headers). Included by ntdll.c. */

EXPORT DWORD NTAPI RtlLengthRequiredSid(DWORD count);
typedef struct { BYTE Value[6]; } SID_AUTHORITY_;
typedef struct { BYTE Revision, SubAuthorityCount; SID_AUTHORITY_ IdentifierAuthority; DWORD SubAuthority[1]; } SID_;

/* ---- SHA-1 and SHA-256 (for the SIDs that are derived from names) -------------------------------------------------------------------------------------------- */

static ULONG sha_rotl(ULONG v, int n) { return (v << n) | (v >> (32 - n)); }
static ULONG sha_rotr(ULONG v, int n) { return (v >> n) | (v << (32 - n)); }
/* Runs the compression function over the padded message; `block(state, 64 bytes)` is called per block. */
static void sha_run(const BYTE *data, SIZE_T length, void (*block)(ULONG *state, const BYTE *bytes), ULONG *state) {
  BYTE tail[128];
  SIZE_T full = length / 64;
  for (SIZE_T i = 0; i < full; ++i) block(state, data + i * 64);
  const SIZE_T rest = length - full * 64;
  memset(tail, 0, sizeof(tail));
  memcpy(tail, data + full * 64, rest);
  tail[rest] = 0x80;
  const SIZE_T total = rest + 1 + 8 <= 64 ? 64 : 128;
  const ULONGLONG bits = (ULONGLONG)length * 8;
  for (int i = 0; i < 8; ++i) tail[total - 1 - i] = (BYTE)(bits >> (8 * i));
  block(state, tail);
  if (total == 128) block(state, tail + 64);
}
static void sha1_block(ULONG *h, const BYTE *p) {
  ULONG w[80];
  for (int i = 0; i < 16; ++i) w[i] = (ULONG)p[4 * i] << 24 | (ULONG)p[4 * i + 1] << 16 | (ULONG)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 80; ++i) w[i] = sha_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  ULONG a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
  for (int i = 0; i < 80; ++i) {
    ULONG f, k;
    if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
    else { f = b ^ c ^ d; k = 0xCA62C1D6; }
    const ULONG t = sha_rotl(a, 5) + f + e + k + w[i];
    e = d; d = c; c = sha_rotl(b, 30); b = a; a = t;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
/* The 20 bytes of the SHA-1 of a message, in the byte order of a little endian machine (as five ULONGs that read the digest as big endian words are stored in memory). */
static void sha1_digest(const BYTE *data, SIZE_T length, BYTE out[20]) {
  ULONG h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
  sha_run(data, length, sha1_block, h);
  for (int i = 0; i < 5; ++i) { out[4 * i] = (BYTE)(h[i] >> 24); out[4 * i + 1] = (BYTE)(h[i] >> 16); out[4 * i + 2] = (BYTE)(h[i] >> 8); out[4 * i + 3] = (BYTE)h[i]; }
}
static const ULONG k_sha256[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static void sha256_block(ULONG *h, const BYTE *p) {
  ULONG w[64];
  for (int i = 0; i < 16; ++i) w[i] = (ULONG)p[4 * i] << 24 | (ULONG)p[4 * i + 1] << 16 | (ULONG)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    const ULONG s0 = sha_rotr(w[i - 15], 7) ^ sha_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = sha_rotr(w[i - 2], 17) ^ sha_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  ULONG a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; ++i) {
    const ULONG s1 = sha_rotr(e, 6) ^ sha_rotr(e, 11) ^ sha_rotr(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + s1 + ch + k_sha256[i] + w[i];
    const ULONG s0 = sha_rotr(a, 2) ^ sha_rotr(a, 13) ^ sha_rotr(a, 22), maj = (a & b) ^ (a & c) ^ (b & c), t2 = s0 + maj;
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
static void sha256_digest(const BYTE *data, SIZE_T length, BYTE out[32]) {
  ULONG h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  sha_run(data, length, sha256_block, h);
  for (int i = 0; i < 8; ++i) { out[4 * i] = (BYTE)(h[i] >> 24); out[4 * i + 1] = (BYTE)(h[i] >> 16); out[4 * i + 2] = (BYTE)(h[i] >> 8); out[4 * i + 3] = (BYTE)h[i]; }
}

/* ---- SIDs of services and capabilities ------------------------------------------------------------------------------------------------------------------------ */

EXPORT NTSTATUS NTAPI RtlCreateServiceSid(PUNICODE_STRING name, PVOID sid_in, DWORD *length) {
  static const SID_AUTHORITY_ nt_authority = {{0, 0, 0, 0, 0, 5}};
  SID_ *sid = sid_in;
  const ULONG count = 1 + 20 / sizeof(DWORD);
  if (!name || !length) return STATUS_INVALID_PARAMETER_;
  const DWORD sid_length = RtlLengthRequiredSid(count);
  if (*length < sid_length) { *length = sid_length; return STATUS_BUFFER_TOO_SMALL_; }
  sid->Revision = 1;
  sid->IdentifierAuthority = nt_authority;
  sid->SubAuthorityCount = (BYTE)count;
  sid->SubAuthority[0] = 80;      /* SECURITY_SERVICE_ID_BASE_RID */
  *length = sid_length;
  UNICODE_STRING upper;
  NTSTATUS status = RtlUpcaseUnicodeString(&upper, name, TRUE);
  if (status) return status;
  BYTE digest[20];
  sha1_digest((const BYTE *)upper.Buffer, upper.Length, digest);
  memcpy(sid->SubAuthority + 1, digest, 20);      /* (the digest is stored as it comes, byte by byte) */
  RtlFreeUnicodeString(&upper);
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlDeriveCapabilitySidsFromName(UNICODE_STRING *name, PVOID group_in, PVOID cap_in) {
  static const SID_AUTHORITY_ app_authority = {{0, 0, 0, 0, 0, 15}}, nt_authority = {{0, 0, 0, 0, 0, 5}};
  UNICODE_STRING upper;
  NTSTATUS status = RtlUpcaseUnicodeString(&upper, name, TRUE);
  if (status) return status;
  BYTE hash[32];
  sha256_digest((const BYTE *)upper.Buffer, upper.Length, hash);
  RtlFreeUnicodeString(&upper);
  SID_ *sid = cap_in;
  sid->Revision = 1;
  sid->IdentifierAuthority = app_authority;
  sid->SubAuthorityCount = 2 + 8;
  sid->SubAuthority[0] = 3;      /* SECURITY_BATCH_RID */
  sid->SubAuthority[1] = 1;      /* SECURITY_CAPABILITY_APP_RID */
  memcpy(sid->SubAuthority + 2, hash, sizeof(hash));
  sid = group_in;
  sid->Revision = 1;
  sid->IdentifierAuthority = nt_authority;
  sid->SubAuthorityCount = 1 + 8;
  sid->SubAuthority[0] = 32;      /* SECURITY_BUILTIN_DOMAIN_RID */
  memcpy(sid->SubAuthority + 1, hash, sizeof(hash));
  return STATUS_SUCCESS;
}

/* ---- the device family --------------------------------------------------------------------------------------------------------------------------------- */

EXPORT void NTAPI RtlGetDeviceFamilyInfoEnum(ULONGLONG *version, DWORD *family, DWORD *form) {
  if (version) *version = 0;
  if (family) *family = 3;      /* DEVICEFAMILYINFOENUM_DESKTOP */
  if (form) *form = 0;      /* DEVICEFAMILYDEVICEFORM_UNKNOWN */
}
EXPORT DWORD NTAPI RtlConvertDeviceFamilyInfoToString(DWORD *family_size, DWORD *form_size, WCHAR *family, WCHAR *form) {
  static const WCHAR desktop[] = {'W','i','n','d','o','w','s','.','D','e','s','k','t','o','p',0};
  static const WCHAR unknown[] = {'U','n','k','n','o','w','n',0};
  if (*family_size < sizeof(desktop) || *form_size < sizeof(unknown)) {
    *family_size = sizeof(desktop);
    *form_size = sizeof(unknown);
    return STATUS_BUFFER_TOO_SMALL_;
  }
  memcpy(family, desktop, sizeof(desktop));
  memcpy(form, unknown, sizeof(unknown));
  return STATUS_SUCCESS;
}

/* ---- the procedures the window system gives its client side (RtlInitializeNtUserPfn) ----------------------------------------------------------------------------- */

#define USER_PROC_COUNT 20
#define USER_WORKER_COUNT 16
static void user_thunk(void) {}
static struct { PVOID A[USER_PROC_COUNT], W[USER_PROC_COUNT], workers[USER_WORKER_COUNT]; } g_user_funcs;      /* what RtlRetrieveNtUserPfn hands out: the same entries at all times */
static struct { PVOID A[USER_PROC_COUNT], W[USER_PROC_COUNT], workers[USER_WORKER_COUNT]; } g_user_procs;      /* what was given */
static int g_user_procs_ready = 1, g_user_funcs_made;      /* (user32 gives its procedures when it starts: they are there from the beginning) */
static void user_funcs_make(void) {
  if (g_user_funcs_made) return;
  for (int i = 0; i < USER_PROC_COUNT; ++i) { g_user_funcs.A[i] = (PVOID)user_thunk; g_user_funcs.W[i] = (PVOID)user_thunk; }
  for (int i = 0; i < USER_WORKER_COUNT; ++i) g_user_funcs.workers[i] = (PVOID)user_thunk;
  g_user_funcs_made = 1;
}
EXPORT NTSTATUS NTAPI RtlInitializeNtUserPfn(const void *procs_a, ULONG size_a, const void *procs_w, ULONG size_w, const void *workers, ULONG size_workers) {
  if (size_a % sizeof(PVOID) || size_a > sizeof(g_user_procs.A) || size_w % sizeof(PVOID) || size_w > sizeof(g_user_procs.W) || size_workers % sizeof(PVOID) || size_workers > sizeof(g_user_procs.workers)) return STATUS_INVALID_PARAMETER_;
  if (g_user_procs_ready) return STATUS_INVALID_PARAMETER_;
  memcpy(g_user_procs.A, procs_a, size_a);
  memcpy(g_user_procs.W, procs_w, size_w);
  memcpy(g_user_procs.workers, workers, size_workers);
  g_user_procs_ready = 1;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlResetNtUserPfn(void) {
  if (!g_user_procs_ready) return STATUS_INVALID_PARAMETER_;
  g_user_procs_ready = 0;
  memset(&g_user_procs, 0, sizeof(g_user_procs));
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlRetrieveNtUserPfn(const void **procs_a, const void **procs_w, const void **workers) {
  if (!g_user_procs_ready) return STATUS_INVALID_PARAMETER_;
  user_funcs_make();
  *procs_a = g_user_funcs.A;
  *procs_w = g_user_funcs.W;
  *workers = g_user_funcs.workers;
  return STATUS_SUCCESS;
}
