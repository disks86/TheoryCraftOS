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

/* kernel32.dll: the names of packages (appmodel): PackageIdFromFullName turns "Name_1.2.3.4_X64_ResourceId_PublisherId" into a PACKAGE_ID, PackageFullNameFromId does the
 * opposite (and works out the publisher id, the 13 characters of a hash of the publisher, if only the publisher is given). Included by kernel32.c. */

typedef struct { DWORD reserved, architecture; WORD revision, build, minor, major; WCHAR *name, *publisher, *resource_id, *publisher_id; } PackageId;

static const ULONG k_pkg_sha256[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static ULONG pkg_rotr(ULONG v, int n) { return (v >> n) | (v << (32 - n)); }
static void pkg_sha256_block(ULONG *h, const BYTE *p) {
  ULONG w[64];
  for (int i = 0; i < 16; ++i) w[i] = (ULONG)p[4 * i] << 24 | (ULONG)p[4 * i + 1] << 16 | (ULONG)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; ++i) {
    const ULONG s0 = pkg_rotr(w[i - 15], 7) ^ pkg_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = pkg_rotr(w[i - 2], 17) ^ pkg_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  ULONG a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; ++i) {
    const ULONG s1 = pkg_rotr(e, 6) ^ pkg_rotr(e, 11) ^ pkg_rotr(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + s1 + ch + k_pkg_sha256[i] + w[i];
    const ULONG s0 = pkg_rotr(a, 2) ^ pkg_rotr(a, 13) ^ pkg_rotr(a, 22), maj = (a & b) ^ (a & c) ^ (b & c), t2 = s0 + maj;
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
/* The SHA-256 of `length` bytes (the message is taken a block at a time; the last one or two blocks hold the padding and the length). */
static void pkg_sha256(const BYTE *data, size_t_ length, BYTE out[32]) {
  ULONG h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t_ done = 0;
  for (; length - done >= 64; done += 64) pkg_sha256_block(h, data + done);
  BYTE tail[128];
  const size_t_ rest = length - done;
  memset(tail, 0, sizeof(tail));
  if (rest) memcpy(tail, data + done, rest);
  tail[rest] = 0x80;
  const size_t_ total = rest < 56 ? 64 : 128;
  const ULONGLONG bits = (ULONGLONG)length * 8;
  for (int i = 0; i < 8; ++i) tail[total - 1 - i] = (BYTE)(bits >> (8 * i));
  pkg_sha256_block(h, tail);
  if (total == 128) pkg_sha256_block(h, tail + 64);
  for (int i = 0; i < 8; ++i) { out[4 * i] = (BYTE)(h[i] >> 24); out[4 * i + 1] = (BYTE)(h[i] >> 16); out[4 * i + 2] = (BYTE)(h[i] >> 8); out[4 * i + 3] = (BYTE)h[i]; }
}

#define PKG_ID_CHARS 13
static const char k_pkg_alphabet[] = "0123456789abcdefghjkmnpqrstvwxyz";
static int pkg_alphabet_has(WCHAR c) { for (int i = 0; k_pkg_alphabet[i]; i++) if ((WCHAR)k_pkg_alphabet[i] == c) return 1; return 0; }
/* The publisher id: the first 8 bytes of the SHA-256 of the publisher (UTF-16), as 13 groups of 5 bits (the 65th bit is 0) in the alphabet of 32 characters without i, l, o, u. */
static void pkg_publisher_id(const WCHAR *publisher, WCHAR out[PKG_ID_CHARS + 1]) {
  BYTE hash[32];
  pkg_sha256((const BYTE *)publisher, (size_t_)wcslen(publisher) * sizeof(WCHAR), hash);
  ULONGLONG bits = 0;
  for (int i = 0; i < 8; i++) bits = (bits << 8) | hash[i];
  for (int i = 0; i < PKG_ID_CHARS; i++) {
    const int shift = 59 - 5 * i;      /* (the 64 bits, followed by a zero bit: the last group has one bit of the hash and the zero) */
    const ULONGLONG group = shift >= 0 ? (bits >> shift) & 31 : (bits << 1) & 31;
    out[i] = (WCHAR)k_pkg_alphabet[group];
  }
  out[PKG_ID_CHARS] = 0;
}

static const struct { DWORD code; const char *name; } k_pkg_architectures[] = {
  {0, "x86"}, {5, "arm"}, {9, "x64"}, {11, "neutral"}, {12, "arm64"}, {0xFFFF, "unknown"}};
static int pkg_text_equal_ci(const WCHAR *w, size_t_ n, const char *a) {
  size_t_ i = 0;
  for (; i < n && a[i]; i++) if (upper(w[i]) != upper((WCHAR)(BYTE)a[i])) return 0;
  return i == n && !a[i];
}

EXPORT LONG WINAPI PackageIdFromFullName(const WCHAR *full_name, DWORD flags, DWORD *size, BYTE *buffer) {
  (void)flags;
  if (!full_name || !size) return ERROR_INVALID_PARAMETER;
  /* name _ major.minor.build.revision _ architecture _ resource id _ publisher id */
  const WCHAR *field[5];
  size_t_ length[5];
  const WCHAR *p = full_name;
  for (int i = 0; i < 5; i++) {
    field[i] = p;
    while (*p && *p != '_') p++;
    length[i] = (size_t_)(p - field[i]);
    if (i < 4) { if (*p != '_') return ERROR_INVALID_PARAMETER; p++; }
  }
  if (*p) return ERROR_INVALID_PARAMETER;      /* more after the publisher id */
  if (!length[0] || length[4] != PKG_ID_CHARS) return ERROR_INVALID_PARAMETER;
  for (size_t_ i = 0; i < PKG_ID_CHARS; i++) if (!pkg_alphabet_has(field[4][i])) return ERROR_INVALID_PARAMETER;
  DWORD part[4];
  const WCHAR *v = field[1], *v_end = field[1] + length[1];
  for (int i = 0; i < 4; i++) {
    if (v >= v_end || *v < '0' || *v > '9') return ERROR_INVALID_PARAMETER;
    DWORD n = 0;
    while (v < v_end && *v >= '0' && *v <= '9') { n = n * 10 + (DWORD)(*v - '0'); if (n > 0xFFFF) return ERROR_INVALID_PARAMETER; v++; }
    part[i] = n;
    if (i < 3) { if (v >= v_end || *v != '.') return ERROR_INVALID_PARAMETER; v++; }
  }
  if (v != v_end) return ERROR_INVALID_PARAMETER;
  int found = -1;
  for (size_t_ i = 0; i < sizeof(k_pkg_architectures) / sizeof(k_pkg_architectures[0]); i++) if (pkg_text_equal_ci(field[2], length[2], k_pkg_architectures[i].name)) found = (int)i;
  if (found < 0) return ERROR_INVALID_PARAMETER;
  const DWORD needed = (DWORD)sizeof(PackageId) + (DWORD)((length[0] + 1 + length[3] + 1 + PKG_ID_CHARS + 1) * sizeof(WCHAR));
  if (!buffer) {      /* only a size of 0 asks how much room is needed */
    if (*size) return ERROR_INVALID_PARAMETER;
    *size = needed;
    return ERROR_INSUFFICIENT_BUFFER;
  }
  if (*size < needed) { *size = needed; return ERROR_INSUFFICIENT_BUFFER; }
  PackageId id;
  memset(&id, 0, sizeof(id));
  id.architecture = k_pkg_architectures[found].code;
  id.major = (WORD)part[0]; id.minor = (WORD)part[1]; id.build = (WORD)part[2]; id.revision = (WORD)part[3];
  WCHAR *out = (WCHAR *)(buffer + sizeof(PackageId));
  id.name = out;
  memcpy(out, field[0], length[0] * sizeof(WCHAR)); out[length[0]] = 0; out += length[0] + 1;
  id.resource_id = out;
  memcpy(out, field[3], length[3] * sizeof(WCHAR)); out[length[3]] = 0; out += length[3] + 1;
  id.publisher_id = out;
  memcpy(out, field[4], PKG_ID_CHARS * sizeof(WCHAR)); out[PKG_ID_CHARS] = 0;
  memcpy(buffer, &id, sizeof(id));
  *size = needed;
  return ERROR_SUCCESS;
}

/* Appends decimal digits of v to out (and the NUL is left to the caller). */
static WCHAR *pkg_put_number(WCHAR *out, DWORD v) {
  WCHAR digits[10];
  int n = 0;
  do { digits[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
  while (n) *out++ = digits[--n];
  return out;
}

EXPORT LONG WINAPI PackageFullNameFromId(const PackageId *id, DWORD *length, WCHAR *full_name) {
  if (!id || !length) return ERROR_INVALID_PARAMETER;
  const char *architecture = NULL;
  for (size_t_ i = 0; i < sizeof(k_pkg_architectures) / sizeof(k_pkg_architectures[0]); i++) if (k_pkg_architectures[i].code == id->architecture) architecture = k_pkg_architectures[i].name;
  if (!id->name || !id->name[0] || !architecture || (!id->publisher_id && !id->publisher)) return ERROR_INVALID_PARAMETER;
  WCHAR computed[PKG_ID_CHARS + 1];
  const WCHAR *publisher_id = id->publisher_id;
  if (!publisher_id) { pkg_publisher_id(id->publisher, computed); publisher_id = computed; }
  static const WCHAR empty[] = {0};
  const WCHAR *resource = id->resource_id ? id->resource_id : empty;
  const size_t_ name_len = wcslen(id->name), resource_len = wcslen(resource), publisher_len = wcslen(publisher_id), architecture_len = strlen(architecture);
  const size_t_ needed = name_len + 1 + 4 * 6 + 1 + architecture_len + 1 + resource_len + 1 + publisher_len + 1;      /* (a version part is at most 5 digits) */
  WCHAR text[512];
  if (needed > sizeof(text) / sizeof(text[0])) return ERROR_INVALID_PARAMETER;
  WCHAR *out = text;
  memcpy(out, id->name, name_len * sizeof(WCHAR)); out += name_len;
  *out++ = '_';
  out = pkg_put_number(out, id->major); *out++ = '.';
  out = pkg_put_number(out, id->minor); *out++ = '.';
  out = pkg_put_number(out, id->build); *out++ = '.';
  out = pkg_put_number(out, id->revision);
  *out++ = '_';
  for (size_t_ i = 0; i < architecture_len; i++) *out++ = (WCHAR)(BYTE)architecture[i];
  *out++ = '_';
  memcpy(out, resource, resource_len * sizeof(WCHAR)); out += resource_len;
  *out++ = '_';
  memcpy(out, publisher_id, publisher_len * sizeof(WCHAR)); out += publisher_len;
  *out = 0;
  const DWORD chars = (DWORD)(out - text) + 1;
  if (!full_name || *length < chars) { *length = chars; return ERROR_INSUFFICIENT_BUFFER; }
  memcpy(full_name, text, chars * sizeof(WCHAR));
  *length = chars;
  return ERROR_SUCCESS;
}
