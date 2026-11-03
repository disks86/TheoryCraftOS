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

/* advapi32.dll: hashing (MD4, MD5, SHA-1, SHA-2), the stream cipher RC4 and the block cipher AES, and the CryptoAPI (CryptCreateHash, CryptEncrypt ...) over them. The provider is a
 * software one that does symmetric work only: hashes (MD4, MD5, SHA-1, SHA-256/384/512, HMAC), session keys (RC4, AES-128/192/256 in ECB or CBC, PKCS #5 padding), key derivation
 * from a hash, and plain-text key blobs. Public-key keys (RSA) and signatures are not there: CryptGenKey for AT_KEYEXCHANGE / AT_SIGNATURE and CryptSignHash answer NTE_BAD_ALGID.
 * Random bytes come from the system's source (NtQuerySystemInformation class 0x1000). */
#include "security.h"
#include <wincrypt.h>


#define PROVIDER_HANDLE 0x435259u      /* the pseudo-handle CryptAcquireContext gives (advapi32.c) */
#define NTE_BAD_ALGID_ ((DWORD)0x80090008)
#define NTE_BAD_FLAGS_ ((DWORD)0x80090009)
#define NTE_BAD_KEY_ ((DWORD)0x80090003)
#define NTE_BAD_HASH_ ((DWORD)0x80090002)
#define NTE_BAD_DATA_ ((DWORD)0x80090005)
#define NTE_BAD_LEN_ ((DWORD)0x80090004)
#define NTE_BAD_TYPE_ ((DWORD)0x8009000A)
#define NTE_BAD_UID_ ((DWORD)0x80090001)
#define NTE_NO_KEY_ ((DWORD)0x8009000D)
#define NTE_BAD_HASH_STATE_ ((DWORD)0x8009000C)
#define NTE_NO_MEMORY_ ((DWORD)0x8009000E)
#define NTE_FAIL_ ((DWORD)0x80090020)
#define NTE_BAD_PROV_TYPE_ ((DWORD)0x80090014)
#define NTE_PROV_TYPE_NOT_DEF_ ((DWORD)0x80090017)
#define NTE_BAD_FLAGS_VALUE NTE_BAD_FLAGS_

/* ---- MD4, MD5 ------------------------------------------------------------------------------------------------------------------------------------------------ */

static unsigned rol32(unsigned x, int n) { return (x << n) | (x >> (32 - n)); }
static unsigned ror32(unsigned x, int n) { return (x >> n) | (x << (32 - n)); }
static unsigned load_le32(const BYTE *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static unsigned load_be32(const BYTE *p) { return p[3] | (p[2] << 8) | (p[1] << 16) | ((unsigned)p[0] << 24); }
static void store_le32(BYTE *p, unsigned v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24); }
static void store_be32(BYTE *p, unsigned v) { p[3] = (BYTE)v; p[2] = (BYTE)(v >> 8); p[1] = (BYTE)(v >> 16); p[0] = (BYTE)(v >> 24); }
static void store_be64(BYTE *p, ULONGLONG v) { int i; for (i = 7; i >= 0; --i) { p[i] = (BYTE)v; v >>= 8; } }
static ULONGLONG load_be64(const BYTE *p) { ULONGLONG v = 0; int i; for (i = 0; i < 8; ++i) v = (v << 8) | p[i]; return v; }

static void md4_block(unsigned s[4], const BYTE *block)
{
    unsigned x[16], a = s[0], b = s[1], c = s[2], d = s[3];
    int i;
    for (i = 0; i < 16; ++i) x[i] = load_le32(block + 4 * i);
#define F4(x, y, z) (((x) & (y)) | (~(x) & (z)))
#define G4(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define H4(x, y, z) ((x) ^ (y) ^ (z))
    for (i = 0; i < 16; i += 4) {
        a = rol32(a + F4(b, c, d) + x[i], 3);     d = rol32(d + F4(a, b, c) + x[i + 1], 7);
        c = rol32(c + F4(d, a, b) + x[i + 2], 11); b = rol32(b + F4(c, d, a) + x[i + 3], 19);
    }
    for (i = 0; i < 4; ++i) {
        a = rol32(a + G4(b, c, d) + x[i] + 0x5a827999u, 3);      d = rol32(d + G4(a, b, c) + x[i + 4] + 0x5a827999u, 5);
        c = rol32(c + G4(d, a, b) + x[i + 8] + 0x5a827999u, 9);  b = rol32(b + G4(c, d, a) + x[i + 12] + 0x5a827999u, 13);
    }
    {
        static const int order[4] = {0, 2, 1, 3};
        for (i = 0; i < 4; ++i) {
            int k = order[i];
            a = rol32(a + H4(b, c, d) + x[k] + 0x6ed9eba1u, 3);       d = rol32(d + H4(a, b, c) + x[k + 8] + 0x6ed9eba1u, 9);
            c = rol32(c + H4(d, a, b) + x[k + 4] + 0x6ed9eba1u, 11);  b = rol32(b + H4(c, d, a) + x[k + 12] + 0x6ed9eba1u, 15);
        }
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

static const unsigned md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
static void md5_block(unsigned s[4], const BYTE *block)
{
    static const int shifts[4][4] = {{7, 12, 17, 22}, {5, 9, 14, 20}, {4, 11, 16, 23}, {6, 10, 15, 21}};
    unsigned x[16], a = s[0], b = s[1], c = s[2], d = s[3];
    int i;
    for (i = 0; i < 16; ++i) x[i] = load_le32(block + 4 * i);
    for (i = 0; i < 64; ++i) {
        unsigned f;
        int g, round = i / 16;
        switch (round) {
        case 0: f = (b & c) | (~b & d); g = i; break;
        case 1: f = (d & b) | (~d & c); g = (5 * i + 1) & 15; break;
        case 2: f = b ^ c ^ d; g = (3 * i + 5) & 15; break;
        default: f = c ^ (b | ~d); g = (7 * i) & 15; break;
        }
        f = f + a + md5_k[i] + x[g];
        a = d; d = c; c = b;
        b = b + rol32(f, shifts[round][i & 3]);
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

/* ---- SHA-1, SHA-2 ----------------------------------------------------------------------------------------------------------------------------------------------- */

static void sha1_block(unsigned s[5], const BYTE *block)
{
    unsigned w[80], a = s[0], b = s[1], c = s[2], d = s[3], e = s[4];
    int i;
    for (i = 0; i < 16; ++i) w[i] = load_be32(block + 4 * i);
    for (i = 16; i < 80; ++i) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; ++i) {
        unsigned f, k, t;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
        else { f = b ^ c ^ d; k = 0xca62c1d6u; }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e;
}
static const unsigned sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
static void sha256_block(unsigned s[8], const BYTE *block)
{
    unsigned w[64], v[8];
    int i;
    for (i = 0; i < 16; ++i) w[i] = load_be32(block + 4 * i);
    for (i = 16; i < 64; ++i) {
        unsigned s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (i = 0; i < 8; ++i) v[i] = s[i];
    for (i = 0; i < 64; ++i) {
        unsigned s1 = ror32(v[4], 6) ^ ror32(v[4], 11) ^ ror32(v[4], 25), ch = (v[4] & v[5]) ^ (~v[4] & v[6]), t1 = v[7] + s1 + ch + sha256_k[i] + w[i];
        unsigned s0 = ror32(v[0], 2) ^ ror32(v[0], 13) ^ ror32(v[0], 22), maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]), t2 = s0 + maj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (i = 0; i < 8; ++i) s[i] += v[i];
}
static const ULONGLONG sha512_k[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull, 0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull, 0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull, 0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};
static ULONGLONG ror64(ULONGLONG x, int n) { return (x >> n) | (x << (64 - n)); }
static void sha512_block(ULONGLONG s[8], const BYTE *block)
{
    ULONGLONG w[80], v[8];
    int i;
    for (i = 0; i < 16; ++i) w[i] = load_be64(block + 8 * i);
    for (i = 16; i < 80; ++i) {
        ULONGLONG s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7), s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (i = 0; i < 8; ++i) v[i] = s[i];
    for (i = 0; i < 80; ++i) {
        ULONGLONG s1 = ror64(v[4], 14) ^ ror64(v[4], 18) ^ ror64(v[4], 41), ch = (v[4] & v[5]) ^ (~v[4] & v[6]), t1 = v[7] + s1 + ch + sha512_k[i] + w[i];
        ULONGLONG s0 = ror64(v[0], 28) ^ ror64(v[0], 34) ^ ror64(v[0], 39), maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]), t2 = s0 + maj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1; v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (i = 0; i < 8; ++i) s[i] += v[i];
}

/* ---- one hash engine for all of them ------------------------------------------------------------------------------------------------------------------------------ */

typedef struct {
    ALG_ID alg;
    DWORD block, size;      /* the block size of the algorithm and the size of its result */
    ULONGLONG length;       /* bytes hashed */
    BYTE buffer[128];
    DWORD fill;
    union { unsigned w[8]; ULONGLONG q[8]; } state;
} Hash;

static BOOL hash_init(Hash *h, ALG_ID alg)
{
    memset(h, 0, sizeof(*h));
    h->alg = alg;
    switch (alg) {
    case CALG_MD4: case CALG_MD5:
        h->block = 64; h->size = 16;
        h->state.w[0] = 0x67452301u; h->state.w[1] = 0xefcdab89u; h->state.w[2] = 0x98badcfeu; h->state.w[3] = 0x10325476u;
        return TRUE;
    case CALG_SHA1:
        h->block = 64; h->size = 20;
        h->state.w[0] = 0x67452301u; h->state.w[1] = 0xefcdab89u; h->state.w[2] = 0x98badcfeu; h->state.w[3] = 0x10325476u; h->state.w[4] = 0xc3d2e1f0u;
        return TRUE;
    case CALG_SHA_256: {
        static const unsigned iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        h->block = 64; h->size = 32;
        memcpy(h->state.w, iv, sizeof(iv));
        return TRUE;
    }
    case CALG_SHA_384: {
        static const ULONGLONG iv[8] = {0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull, 0x152fecd8f70e5939ull, 0x67332667ffc00b31ull, 0x8eb44a8768581511ull, 0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull};
        h->block = 128; h->size = 48;
        memcpy(h->state.q, iv, sizeof(iv));
        return TRUE;
    }
    case CALG_SHA_512: {
        static const ULONGLONG iv[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
        h->block = 128; h->size = 64;
        memcpy(h->state.q, iv, sizeof(iv));
        return TRUE;
    }
    default: return FALSE;
    }
}
static void hash_compress(Hash *h, const BYTE *block)
{
    switch (h->alg) {
    case CALG_MD4: md4_block(h->state.w, block); break;
    case CALG_MD5: md5_block(h->state.w, block); break;
    case CALG_SHA1: sha1_block(h->state.w, block); break;
    case CALG_SHA_256: sha256_block(h->state.w, block); break;
    default: sha512_block(h->state.q, block); break;
    }
}
static void hash_update(Hash *h, const BYTE *data, DWORD size)
{
    h->length += size;
    while (size) {
        DWORD n = h->block - h->fill;
        if (n > size) n = size;
        memcpy(h->buffer + h->fill, data, n);
        h->fill += n; data += n; size -= n;
        if (h->fill == h->block) { hash_compress(h, h->buffer); h->fill = 0; }
    }
}
static void hash_final(Hash *h, BYTE *out)
{
    ULONGLONG bits = h->length * 8;
    BYTE pad[144];
    DWORD pad_len = (h->fill < h->block - (h->block == 128 ? 16u : 8u)) ? (h->block - (h->block == 128 ? 16u : 8u)) - h->fill : (2 * h->block - (h->block == 128 ? 16u : 8u)) - h->fill, i;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    {
        ULONGLONG saved = h->length;
        hash_update(h, pad, pad_len);
        h->length = saved;
    }
    if (h->alg == CALG_MD4 || h->alg == CALG_MD5) { BYTE tail[8]; store_le32(tail, (unsigned)bits); store_le32(tail + 4, (unsigned)(bits >> 32)); hash_update(h, tail, 8); }
    else if (h->block == 64) { BYTE tail[8]; store_be64(tail, bits); hash_update(h, tail, 8); }
    else { BYTE tail[16] = {0}; store_be64(tail + 8, bits); hash_update(h, tail, 16); }
    if (h->alg == CALG_MD4 || h->alg == CALG_MD5) for (i = 0; i < 4; ++i) store_le32(out + 4 * i, h->state.w[i]);
    else if (h->block == 64) for (i = 0; i < h->size / 4; ++i) store_be32(out + 4 * i, h->state.w[i]);
    else for (i = 0; i < h->size / 8; ++i) store_be64(out + 8 * i, h->state.q[i]);
}

/* ---- the classic entry points: MD4*, MD5*, A_SHA* ---------------------------------------------------------------------------------------------------------------- */

typedef struct { ULONG i[2]; ULONG buf[4]; UCHAR in[64]; UCHAR digest[16]; } MD_CTX;      /* the layout of RSA's MD4 / MD5 context (88 bytes) */
static void md_init(MD_CTX *c, int md5)
{
    memset(c, 0, sizeof(*c));
    c->buf[0] = 0x67452301u; c->buf[1] = 0xefcdab89u; c->buf[2] = 0x98badcfeu; c->buf[3] = 0x10325476u;
    (void)md5;
}
static void md_update(MD_CTX *c, const BYTE *data, ULONG size, int md5)
{
    ULONGLONG count = ((ULONGLONG)c->i[1] << 32 | c->i[0]);      /* bits so far */
    DWORD have = (DWORD)((count >> 3) & 63);
    count += (ULONGLONG)size * 8;
    c->i[0] = (ULONG)count; c->i[1] = (ULONG)(count >> 32);
    while (size) {
        DWORD n = 64 - have;
        if (n > size) n = size;
        memcpy(c->in + have, data, n);
        have += n; data += n; size -= n;
        if (have == 64) { if (md5) md5_block((unsigned *)c->buf, c->in); else md4_block((unsigned *)c->buf, c->in); have = 0; }
    }
}
static void md_final(MD_CTX *c, int md5)
{
    BYTE pad[72] = {0x80}, bits[8];
    ULONGLONG count = ((ULONGLONG)c->i[1] << 32 | c->i[0]);
    DWORD have = (DWORD)((count >> 3) & 63), i;
    store_le32(bits, (unsigned)count); store_le32(bits + 4, (unsigned)(count >> 32));
    md_update(c, pad, have < 56 ? 56 - have : 120 - have, md5);
    md_update(c, bits, 8, md5);
    for (i = 0; i < 4; ++i) store_le32(c->digest + 4 * i, c->buf[i]);
}
__declspec(dllexport) VOID WINAPI MD4Init(MD_CTX *c) { md_init(c, 0); }
__declspec(dllexport) VOID WINAPI MD4Update(MD_CTX *c, const BYTE *data, ULONG size) { md_update(c, data, size, 0); }
__declspec(dllexport) VOID WINAPI MD4Final(MD_CTX *c) { md_final(c, 0); }
__declspec(dllexport) VOID WINAPI MD5Init(MD_CTX *c) { md_init(c, 1); }
__declspec(dllexport) VOID WINAPI MD5Update(MD_CTX *c, const BYTE *data, ULONG size) { md_update(c, data, size, 1); }
__declspec(dllexport) VOID WINAPI MD5Final(MD_CTX *c) { md_final(c, 1); }

typedef struct { ULONG unknown[6]; ULONG state[5]; ULONG count[2]; UCHAR buffer[64]; } SHA_CTX_;      /* NT's SHA context (116 bytes) */
__declspec(dllexport) VOID WINAPI A_SHAInit(SHA_CTX_ *c)
{
    memset(c, 0, sizeof(*c));
    c->state[0] = 0x67452301u; c->state[1] = 0xefcdab89u; c->state[2] = 0x98badcfeu; c->state[3] = 0x10325476u; c->state[4] = 0xc3d2e1f0u;
}
__declspec(dllexport) VOID WINAPI A_SHAUpdate(SHA_CTX_ *c, const BYTE *data, UINT size)
{
    DWORD have = c->count[1] & 63;
    ULONGLONG total = ((ULONGLONG)c->count[0] << 32 | c->count[1]) + size;      /* count: bytes, high word first */
    c->count[0] = (ULONG)(total >> 32); c->count[1] = (ULONG)total;
    while (size) {
        DWORD n = 64 - have;
        if (n > size) n = size;
        memcpy(c->buffer + have, data, n);
        have += n; data += n; size -= n;
        if (have == 64) { sha1_block((unsigned *)c->state, c->buffer); have = 0; }
    }
}
__declspec(dllexport) VOID WINAPI A_SHAFinal(SHA_CTX_ *c, BYTE *result)
{
    ULONGLONG bytes = ((ULONGLONG)c->count[0] << 32 | c->count[1]), bits = bytes * 8;
    BYTE pad[72] = {0x80}, tail[8];
    DWORD have = (DWORD)(bytes & 63), i;
    store_be64(tail, bits);
    A_SHAUpdate(c, pad, have < 56 ? 56 - have : 120 - have);
    A_SHAUpdate(c, tail, 8);
    for (i = 0; i < 5; ++i) store_be32(result + 4 * i, c->state[i]);
}

/* ---- RC4 and AES ---------------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { BYTE s[256]; BYTE i, j; } Rc4;
static void rc4_setup(Rc4 *r, const BYTE *key, DWORD length)
{
    DWORD i, j = 0;
    for (i = 0; i < 256; ++i) r->s[i] = (BYTE)i;
    for (i = 0; i < 256; ++i) { BYTE t = r->s[i]; j = (j + t + key[i % length]) & 255; r->s[i] = r->s[j]; r->s[j] = t; }
    r->i = r->j = 0;
}
static void rc4_crypt(Rc4 *r, BYTE *data, DWORD length)
{
    while (length--) {
        BYTE t;
        r->i++;
        t = r->s[r->i];
        r->j = (BYTE)(r->j + t);
        r->s[r->i] = r->s[r->j];
        r->s[r->j] = t;
        *data++ ^= r->s[(BYTE)(r->s[r->i] + r->s[r->j])];
    }
}

static BYTE aes_sbox[256], aes_inv[256];
static volatile LONG aes_ready;
static BYTE xtime(BYTE x) { return (BYTE)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static BYTE gmul(BYTE a, BYTE b) { BYTE r = 0; while (b) { if (b & 1) r ^= a; a = xtime(a); b >>= 1; } return r; }
static void aes_tables(void)
{
    BYTE p = 1, q = 1;
    if (aes_ready) return;
    do {      /* the multiplicative inverse by walking a generator and its inverse, then the affine map */
        BYTE x;
        p = (BYTE)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
        q ^= q << 1; q ^= q << 2; q ^= q << 4;
        if (q & 0x80) q ^= 0x09;
        x = (BYTE)(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
        aes_sbox[p] = (BYTE)(x ^ 0x63);
    } while (p != 1);
    aes_sbox[0] = 0x63;
    {
        int i;
        for (i = 0; i < 256; ++i) aes_inv[aes_sbox[i]] = (BYTE)i;
    }
    aes_ready = 1;
}
typedef struct { BYTE rk[15][16]; int rounds; } Aes;
static void aes_setup(Aes *a, const BYTE *key, DWORD length)
{
    BYTE w[240];
    int nk = (int)(length / 4), total, i;
    BYTE rcon = 1;
    aes_tables();
    a->rounds = nk + 6;
    total = 4 * (a->rounds + 1);
    memcpy(w, key, length);
    for (i = nk; i < total; ++i) {
        BYTE t[4];
        memcpy(t, w + 4 * (i - 1), 4);
        if (i % nk == 0) {
            BYTE r = t[0];
            t[0] = (BYTE)(aes_sbox[t[1]] ^ rcon); t[1] = aes_sbox[t[2]]; t[2] = aes_sbox[t[3]]; t[3] = aes_sbox[r];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) { t[0] = aes_sbox[t[0]]; t[1] = aes_sbox[t[1]]; t[2] = aes_sbox[t[2]]; t[3] = aes_sbox[t[3]]; }
        w[4 * i] = w[4 * (i - nk)] ^ t[0]; w[4 * i + 1] = w[4 * (i - nk) + 1] ^ t[1]; w[4 * i + 2] = w[4 * (i - nk) + 2] ^ t[2]; w[4 * i + 3] = w[4 * (i - nk) + 3] ^ t[3];
    }
    for (i = 0; i <= a->rounds; ++i) memcpy(a->rk[i], w + 16 * i, 16);
}
static void aes_encrypt_block(const Aes *a, const BYTE in[16], BYTE out[16])
{
    BYTE s[16], t[16];
    int r, i;
    for (i = 0; i < 16; ++i) s[i] = in[i] ^ a->rk[0][i];
    for (r = 1; r <= a->rounds; ++r) {
        for (i = 0; i < 16; ++i) t[i] = aes_sbox[s[i]];
        for (i = 0; i < 16; ++i) s[i] = t[(i + 4 * (i & 3)) & 15];      /* ShiftRows (column-major state: row = i & 3) */
        if (r != a->rounds)
            for (i = 0; i < 16; i += 4) {
                BYTE c0 = s[i], c1 = s[i + 1], c2 = s[i + 2], c3 = s[i + 3];
                t[i] = (BYTE)(xtime(c0) ^ (xtime(c1) ^ c1) ^ c2 ^ c3);
                t[i + 1] = (BYTE)(c0 ^ xtime(c1) ^ (xtime(c2) ^ c2) ^ c3);
                t[i + 2] = (BYTE)(c0 ^ c1 ^ xtime(c2) ^ (xtime(c3) ^ c3));
                t[i + 3] = (BYTE)((xtime(c0) ^ c0) ^ c1 ^ c2 ^ xtime(c3));
            }
        else memcpy(t, s, 16);
        for (i = 0; i < 16; ++i) s[i] = t[i] ^ a->rk[r][i];
    }
    memcpy(out, s, 16);
}
static void aes_decrypt_block(const Aes *a, const BYTE in[16], BYTE out[16])
{
    BYTE s[16], t[16];
    int r, i;
    for (i = 0; i < 16; ++i) s[i] = in[i] ^ a->rk[a->rounds][i];
    for (r = a->rounds - 1; r >= 0; --r) {
        for (i = 0; i < 16; ++i) t[(i + 4 * (i & 3)) & 15] = s[i];      /* InvShiftRows */
        for (i = 0; i < 16; ++i) s[i] = aes_inv[t[i]];
        for (i = 0; i < 16; ++i) s[i] ^= a->rk[r][i];
        if (r != 0)
            for (i = 0; i < 16; i += 4) {
                BYTE c0 = s[i], c1 = s[i + 1], c2 = s[i + 2], c3 = s[i + 3];
                t[i] = (BYTE)(gmul(c0, 14) ^ gmul(c1, 11) ^ gmul(c2, 13) ^ gmul(c3, 9));
                t[i + 1] = (BYTE)(gmul(c0, 9) ^ gmul(c1, 14) ^ gmul(c2, 11) ^ gmul(c3, 13));
                t[i + 2] = (BYTE)(gmul(c0, 13) ^ gmul(c1, 9) ^ gmul(c2, 14) ^ gmul(c3, 11));
                t[i + 3] = (BYTE)(gmul(c0, 11) ^ gmul(c1, 13) ^ gmul(c2, 9) ^ gmul(c3, 14));
            }
        else memcpy(t, s, 16);
        memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}

/* ---- random bytes ----------------------------------------------------------------------------------------------------------------------------------------------- */

static BOOL system_random(BYTE *out, DWORD length)
{
    while (length) {
        DWORD n = length > 0x10000 ? 0x10000 : length;
        if (NtQuerySystemInformation((SYSTEM_INFORMATION_CLASS)0x1000, out, n, NULL) < 0) return FALSE;
        out += n; length -= n;
    }
    return TRUE;
}
BOOL TCOS_FillRandom(BYTE *out, DWORD length) { return system_random(out, length); }

/* RtlGenRandom / SystemFunction036: unlike CryptGenRandom it needs no provider. */
__declspec(dllexport) BOOLEAN WINAPI SystemFunction036(PVOID buffer, ULONG length)
{
    if (!buffer && length) return FALSE;
    return system_random(buffer, length) ? TRUE : FALSE;
}

/* RtlEncryptDecryptRC4 / SystemFunction032 (data, key as USTRINGs) and 033 (the same) */
typedef struct { USHORT Length, MaximumLength; PUCHAR Buffer; } USTRING_;
__declspec(dllexport) NTSTATUS WINAPI SystemFunction032(USTRING_ *data, const USTRING_ *key)
{
    Rc4 r;
    if (!data || !key || !key->Length || (data->Length && !data->Buffer) || !key->Buffer) return STATUS_INVALID_PARAMETER;
    rc4_setup(&r, key->Buffer, key->Length);
    rc4_crypt(&r, data->Buffer, data->Length);
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI SystemFunction033(USTRING_ *data, const USTRING_ *key) { return SystemFunction032(data, key); }

/* RtlEncryptMemory / RtlDecryptMemory (SystemFunction040 / 041): the block is scrambled with a key of this boot, unique to the process for the flag 0 (same logon: 1, same luid: 2 are
 * all the same key here, as there is one process-wide secret). Sizes are multiples of 8. */
static BYTE g_memory_key[16];
static volatile LONG g_memory_key_ready;
static BOOL memory_key(void)
{
    if (!g_memory_key_ready) {
        BYTE k[16];
        if (!system_random(k, 16)) return FALSE;
        memcpy(g_memory_key, k, 16);
        g_memory_key_ready = 1;
    }
    return TRUE;
}
static NTSTATUS memory_crypt(PVOID memory, ULONG size, ULONG option, BOOL encrypt)
{
    Aes a;
    BYTE *p = memory, chain[16] = {0};
    ULONG i, off;
    if (size % 8 || option > 2 || (!memory && size)) return STATUS_INVALID_PARAMETER;
    if (!memory_key()) return STATUS_UNSUCCESSFUL;
    aes_setup(&a, g_memory_key, 16);
    chain[0] = (BYTE)option;
    for (off = 0; off + 8 <= size; off += 8) {      /* 8-byte units: each is padded to a block, ciphered, and the first 8 bytes kept in a feedback mode */
        BYTE block[16] = {0}, out[16];
        memcpy(block, chain, 8);
        aes_encrypt_block(&a, block, out);      /* a keystream, so that encrypt and decrypt are the same operation on the data */
        for (i = 0; i < 8; ++i) p[off + i] ^= out[i];
        memcpy(chain, encrypt ? p + off : p + off, 0);
        chain[0] = (BYTE)(option + off / 8 + 1);
        chain[1] = (BYTE)((off / 8 + 1) >> 8);
    }
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI SystemFunction040(PVOID memory, ULONG size, ULONG option) { return memory_crypt(memory, size, option, TRUE); }
__declspec(dllexport) NTSTATUS WINAPI SystemFunction041(PVOID memory, ULONG size, ULONG option) { return memory_crypt(memory, size, option, FALSE); }

/* ---- IsTextUnicode ---------------------------------------------------------------------------------------------------------------------------------------------------- */

NTSTATUS NTAPI RtlIsTextUnicode_(void);
BOOLEAN NTAPI RtlIsTextUnicode(PVOID buffer, ULONG size, PULONG flags);
__declspec(dllexport) BOOL WINAPI IsTextUnicode(const VOID *buffer, INT size, LPINT flags)
{
    ULONG f = flags ? (ULONG)*flags : ~0u;
    BOOL r = RtlIsTextUnicode((PVOID)buffer, (ULONG)size, flags ? &f : NULL);
    if (flags) *flags = (INT)f;
    return r;
}

/* ---- the CryptoAPI: provider -------------------------------------------------------------------------------------------------------------------------------------- */

#define PROVIDER_NAME L"Microsoft Enhanced RSA and AES Cryptographic Provider"
static BOOL provider_ok(HCRYPTPROV p)
{
    if ((ULONG_PTR)p != PROVIDER_HANDLE) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    return TRUE;
}

typedef struct {
    DWORD magic;
    Hash hash;
    BOOL hmac, finished;
    Hash outer;
    BYTE value[64];
    BYTE key_block[128];
    DWORD key_block_size;
} HashObject;
typedef struct {
    DWORD magic;
    ALG_ID alg;
    DWORD key_length, mode, padding;
    BYTE key[32], iv[16], salt[16];
    DWORD salt_length, effective_bits;
    Aes aes;
    Rc4 rc4;
    BOOL iv_set;
} KeyObject;
#define HASH_MAGIC 0x48534852u
#define KEY_MAGIC 0x4B45594Bu
#define MAX_OBJECTS 128
static HashObject g_hashes[MAX_OBJECTS];
static KeyObject g_keys[MAX_OBJECTS];
static volatile LONG g_object_lock;
static void olock(void) { while (InterlockedCompareExchange(&g_object_lock, 1, 0)) Sleep(0); }
static void ounlock(void) { InterlockedExchange(&g_object_lock, 0); }

static HashObject *hash_of(HCRYPTHASH h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    HashObject *o;
    if (v < 0x48410001u || v > 0x48410000u + MAX_OBJECTS) { SetLastError(NTE_BAD_HASH_); return NULL; }
    o = &g_hashes[v - 0x48410001u];
    if (o->magic != HASH_MAGIC) { SetLastError(NTE_BAD_HASH_); return NULL; }
    return o;
}
static KeyObject *key_of(HCRYPTKEY h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    KeyObject *o;
    if (v < 0x4B450001u || v > 0x4B450000u + MAX_OBJECTS) { SetLastError(NTE_BAD_KEY_); return NULL; }
    o = &g_keys[v - 0x4B450001u];
    if (o->magic != KEY_MAGIC) { SetLastError(NTE_BAD_KEY_); return NULL; }
    return o;
}
static HashObject *new_hash(HCRYPTHASH *out)
{
    DWORD i;
    olock();
    for (i = 0; i < MAX_OBJECTS; ++i) if (!g_hashes[i].magic) { memset(&g_hashes[i], 0, sizeof(g_hashes[i])); g_hashes[i].magic = HASH_MAGIC; *out = (HCRYPTHASH)(ULONG_PTR)(0x48410001u + i); ounlock(); return &g_hashes[i]; }
    ounlock();
    SetLastError(NTE_NO_MEMORY_);
    return NULL;
}
static KeyObject *new_key(HCRYPTKEY *out)
{
    DWORD i;
    olock();
    for (i = 0; i < MAX_OBJECTS; ++i) if (!g_keys[i].magic) { memset(&g_keys[i], 0, sizeof(g_keys[i])); g_keys[i].magic = KEY_MAGIC; *out = (HCRYPTKEY)(ULONG_PTR)(0x4B450001u + i); ounlock(); return &g_keys[i]; }
    ounlock();
    SetLastError(NTE_NO_MEMORY_);
    return NULL;
}

__declspec(dllexport) BOOL WINAPI CryptContextAddRef(HCRYPTPROV p, DWORD *reserved, DWORD flags)
{
    (void)reserved;
    if (!provider_ok(p)) return FALSE;
    if (flags) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    return TRUE;
}

/* ---- hashes ------------------------------------------------------------------------------------------------------------------------------------------------------------ */

__declspec(dllexport) BOOL WINAPI CryptCreateHash(HCRYPTPROV p, ALG_ID alg, HCRYPTKEY key, DWORD flags, HCRYPTHASH *out)
{
    HashObject *h;
    HCRYPTHASH handle;
    if (!provider_ok(p)) return FALSE;
    if (!out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (flags) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (alg == CALG_HMAC) {
        KeyObject *k = key_of(key);
        if (!k) return FALSE;
        if (!(h = new_hash(&handle))) return FALSE;
        h->hmac = TRUE;
        h->hash.alg = 0;      /* the digest is chosen by CryptSetHashParam(HP_HMAC_INFO) */
        memcpy(h->key_block, k->key, k->key_length);
        h->key_block_size = k->key_length;
        *out = handle;
        return TRUE;
    }
    if (alg == CALG_MAC) { SetLastError(NTE_BAD_ALGID_); return FALSE; }
    if (GET_ALG_CLASS(alg) != ALG_CLASS_HASH) { SetLastError(NTE_BAD_ALGID_); return FALSE; }
    if (!(h = new_hash(&handle))) return FALSE;
    if (!hash_init(&h->hash, alg)) { h->magic = 0; SetLastError(NTE_BAD_ALGID_); return FALSE; }
    *out = handle;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptDestroyHash(HCRYPTHASH h)
{
    HashObject *o = hash_of(h);
    if (!o) return FALSE;
    olock();
    o->magic = 0;
    ounlock();
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptDuplicateHash(HCRYPTHASH h, DWORD *reserved, DWORD flags, HCRYPTHASH *out)
{
    HashObject *o = hash_of(h), *copy;
    HCRYPTHASH handle;
    (void)reserved;
    if (!o) return FALSE;
    if (!out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (flags) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (!(copy = new_hash(&handle))) return FALSE;
    { DWORD magic = copy->magic; *copy = *o; copy->magic = magic; }
    *out = handle;
    return TRUE;
}
static BOOL hmac_prepare(HashObject *h)
{
    /* the inner hash starts with key ^ ipad: done when the digest is known (HP_HMAC_INFO) */
    BYTE block[128], key[128];
    DWORD i, size = h->hash.block;
    memset(key, 0, sizeof(key));
    if (h->key_block_size > size) {
        Hash k;
        hash_init(&k, h->hash.alg);
        hash_update(&k, h->key_block, h->key_block_size);
        hash_final(&k, key);
    } else memcpy(key, h->key_block, h->key_block_size);
    for (i = 0; i < size; ++i) block[i] = key[i] ^ 0x36;
    hash_update(&h->hash, block, size);
    for (i = 0; i < size; ++i) block[i] = key[i] ^ 0x5c;
    hash_init(&h->outer, h->hash.alg);
    hash_update(&h->outer, block, size);
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptHashData(HCRYPTHASH h, const BYTE *data, DWORD size, DWORD flags)
{
    HashObject *o = hash_of(h);
    if (!o) return FALSE;
    if (flags & ~CRYPT_USERDATA) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (o->finished) { SetLastError(NTE_BAD_HASH_STATE_); return FALSE; }
    if (o->hmac && !o->hash.alg) { SetLastError(NTE_BAD_HASH_STATE_); return FALSE; }
    if (size && !data) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    hash_update(&o->hash, data, size);
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptHashSessionKey(HCRYPTHASH h, HCRYPTKEY k, DWORD flags)
{
    HashObject *o = hash_of(h);
    KeyObject *key = key_of(k);
    if (!o || !key) return FALSE;
    if (flags & ~CRYPT_LITTLE_ENDIAN) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (o->finished) { SetLastError(NTE_BAD_HASH_STATE_); return FALSE; }
    {
        BYTE swapped[32];
        DWORD i;
        for (i = 0; i < key->key_length; ++i) swapped[i] = (flags & CRYPT_LITTLE_ENDIAN) ? key->key[i] : key->key[key->key_length - 1 - i];
        hash_update(&o->hash, swapped, key->key_length);
    }
    return TRUE;
}
static void finish_hash(HashObject *o)
{
    if (o->finished) return;
    {
        Hash copy = o->hash;
        BYTE inner[64];
        hash_final(&copy, inner);
        if (o->hmac) {
            Hash outer = o->outer;
            hash_update(&outer, inner, copy.size);
            hash_final(&outer, o->value);
        } else memcpy(o->value, inner, copy.size);
    }
}
__declspec(dllexport) BOOL WINAPI CryptGetHashParam(HCRYPTHASH h, DWORD param, BYTE *data, DWORD *size, DWORD flags)
{
    HashObject *o = hash_of(h);
    if (!o) return FALSE;
    if (flags) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    switch (param) {
    case HP_ALGID: {
        DWORD v = o->hmac ? CALG_HMAC : o->hash.alg;
        if (!data) { *size = 4; return TRUE; }
        if (*size < 4) { *size = 4; SetLastError(ERROR_MORE_DATA); return FALSE; }
        *(DWORD *)data = v; *size = 4;
        return TRUE;
    }
    case HP_HASHSIZE:
        if (!data) { *size = 4; return TRUE; }
        if (*size < 4) { *size = 4; SetLastError(ERROR_MORE_DATA); return FALSE; }
        *(DWORD *)data = o->hash.size; *size = 4;
        return TRUE;
    case HP_HASHVAL:
        if (o->hmac && !o->hash.alg) { SetLastError(NTE_BAD_HASH_STATE_); return FALSE; }
        if (!data) { *size = o->hash.size; return TRUE; }
        if (*size < o->hash.size) { *size = o->hash.size; SetLastError(ERROR_MORE_DATA); return FALSE; }
        finish_hash(o);
        memcpy(data, o->value, o->hash.size);
        *size = o->hash.size;
        o->finished = TRUE;      /* (a hash that has been read can take no more data) */
        return TRUE;
    default: SetLastError(NTE_BAD_TYPE_); return FALSE;
    }
}
__declspec(dllexport) BOOL WINAPI CryptSetHashParam(HCRYPTHASH h, DWORD param, const BYTE *data, DWORD flags)
{
    HashObject *o = hash_of(h);
    if (!o) return FALSE;
    if (flags) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (!data) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    switch (param) {
    case HP_HMAC_INFO: {
        const HMAC_INFO *info = (const HMAC_INFO *)data;
        if (!o->hmac) { SetLastError(NTE_BAD_TYPE_); return FALSE; }
        if (o->hash.alg) { SetLastError(NTE_BAD_HASH_STATE_); return FALSE; }
        if (!hash_init(&o->hash, info->HashAlgid)) { SetLastError(NTE_BAD_ALGID_); return FALSE; }
        hmac_prepare(o);
        return TRUE;
    }
    case HP_HASHVAL:
        if (o->hash.size == 0) { SetLastError(NTE_BAD_HASH_STATE_); return FALSE; }
        memcpy(o->value, data, o->hash.size);
        o->finished = TRUE;
        return TRUE;
    default: SetLastError(NTE_BAD_TYPE_); return FALSE;
    }
}

/* ---- keys --------------------------------------------------------------------------------------------------------------------------------------------------------- */

static DWORD block_size_of(ALG_ID alg) { return (alg == CALG_AES_128 || alg == CALG_AES_192 || alg == CALG_AES_256) ? 16 : 0; }
static DWORD default_bits(ALG_ID alg)
{
    switch (alg) {
    case CALG_AES_128: return 128;
    case CALG_AES_192: return 192;
    case CALG_AES_256: return 256;
    case CALG_RC4: return 128;
    default: return 0;
    }
}
static BOOL prepare_key(KeyObject *k)
{
    switch (k->alg) {
    case CALG_RC4: {
        BYTE full[32 + 16];
        DWORD n = k->key_length;
        memcpy(full, k->key, n);
        if (k->salt_length) { memcpy(full + n, k->salt, k->salt_length); n += k->salt_length; }
        rc4_setup(&k->rc4, full, n);
        return TRUE;
    }
    case CALG_AES_128: case CALG_AES_192: case CALG_AES_256:
        if (k->key_length != (k->alg == CALG_AES_128 ? 16u : (k->alg == CALG_AES_192 ? 24u : 32u))) return FALSE;
        aes_setup(&k->aes, k->key, k->key_length);
        return TRUE;
    default: return FALSE;
    }
}
static KeyObject *make_key(ALG_ID alg, const BYTE *material, DWORD length, HCRYPTKEY *out)
{
    KeyObject *k;
    if (!default_bits(alg)) { SetLastError(NTE_BAD_ALGID_); return NULL; }
    if (length == 0 || length > 32) { SetLastError(NTE_BAD_LEN_); return NULL; }
    if (!(k = new_key(out))) return NULL;
    k->alg = alg;
    k->key_length = length;
    memcpy(k->key, material, length);
    k->mode = CRYPT_MODE_CBC;
    k->padding = PKCS5_PADDING;
    k->effective_bits = length * 8;
    if (!prepare_key(k)) { k->magic = 0; SetLastError(NTE_BAD_LEN_); return NULL; }
    return k;
}
__declspec(dllexport) BOOL WINAPI CryptGenKey(HCRYPTPROV p, ALG_ID alg, DWORD flags, HCRYPTKEY *out)
{
    BYTE material[32];
    DWORD bits = flags >> 16, length;
    if (!provider_ok(p)) return FALSE;
    if (!out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (alg == AT_KEYEXCHANGE || alg == AT_SIGNATURE || GET_ALG_CLASS(alg) == ALG_CLASS_SIGNATURE || GET_ALG_CLASS(alg) == ALG_CLASS_KEY_EXCHANGE) { SetLastError(NTE_BAD_ALGID_); return FALSE; }
    if (flags & ~(0xFFFF0000u | CRYPT_EXPORTABLE | CRYPT_CREATE_SALT | CRYPT_NO_SALT)) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (!default_bits(alg)) { SetLastError(NTE_BAD_ALGID_); return FALSE; }
    if (!bits) bits = default_bits(alg);
    if (alg != CALG_RC4 && bits != default_bits(alg)) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (alg == CALG_RC4 && (bits < 40 || bits > 256 || bits % 8)) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    length = bits / 8;
    if (!system_random(material, length)) { SetLastError(NTE_FAIL_); return FALSE; }
    {
        KeyObject *k = make_key(alg, material, length, out);
        if (!k) return FALSE;
        if (flags & CRYPT_CREATE_SALT) {
            DWORD n = alg == CALG_RC4 && bits == 40 ? 11 : 0;
            if (n && system_random(k->salt, n)) { k->salt_length = n; prepare_key(k); }
        }
    }
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptDestroyKey(HCRYPTKEY h)
{
    KeyObject *k = key_of(h);
    if (!k) return FALSE;
    olock();
    memset(k, 0, sizeof(*k));
    ounlock();
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptDuplicateKey(HCRYPTKEY h, DWORD *reserved, DWORD flags, HCRYPTKEY *out)
{
    KeyObject *k = key_of(h), *copy;
    HCRYPTKEY handle;
    (void)reserved;
    if (!k) return FALSE;
    if (!out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (flags) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (!(copy = new_key(&handle))) return FALSE;
    { DWORD magic = copy->magic; *copy = *k; copy->magic = magic; }
    *out = handle;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptDeriveKey(HCRYPTPROV p, ALG_ID alg, HCRYPTHASH hash, DWORD flags, HCRYPTKEY *out)
{
    HashObject *h = hash_of(hash);
    DWORD bits = flags >> 16, length, base_size;
    BYTE material[64];
    if (!provider_ok(p) || !h) return FALSE;
    if (!out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (flags & ~(0xFFFF0000u | CRYPT_EXPORTABLE | CRYPT_CREATE_SALT | CRYPT_NO_SALT | CRYPT_UPDATE_KEY | CRYPT_SERVER)) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (!default_bits(alg)) { SetLastError(NTE_BAD_ALGID_); return FALSE; }
    if (!bits) bits = default_bits(alg);
    if (alg != CALG_RC4 && bits != default_bits(alg)) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (alg == CALG_RC4 && (bits < 40 || bits > 256 || bits % 8)) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    length = bits / 8;
    if (h->hmac && !h->hash.alg) { SetLastError(NTE_BAD_HASH_STATE_); return FALSE; }
    finish_hash(h);
    base_size = h->hash.size;
    if (length <= base_size) memcpy(material, h->value, length);
    else {      /* longer than the hash: X1 = H(base ^ 0x36 repeated), X2 = H(base ^ 0x5c repeated), key = X1 | X2 */
        BYTE pad[64], x[128];
        Hash a, b;
        DWORD i;
        for (i = 0; i < 64; ++i) pad[i] = (i < base_size ? h->value[i] : 0) ^ 0x36;
        hash_init(&a, h->hash.alg); hash_update(&a, pad, 64); hash_final(&a, x);
        for (i = 0; i < 64; ++i) pad[i] = (i < base_size ? h->value[i] : 0) ^ 0x5c;
        hash_init(&b, h->hash.alg); hash_update(&b, pad, 64); hash_final(&b, x + base_size);
        memcpy(material, x, length > base_size * 2 ? base_size * 2 : length);
    }
    h->finished = TRUE;
    return make_key(alg, material, length, out) != NULL;
}

/* A plain-text key blob: BLOBHEADER { bType PLAINTEXTKEYBLOB, bVersion 2, reserved, aiKeyAlg } then a length and the key. */
#pragma pack(push, 1)
typedef struct { BYTE type, version; WORD reserved; ALG_ID alg; DWORD length; } PlainBlobHeader;
#pragma pack(pop)
__declspec(dllexport) BOOL WINAPI CryptExportKey(HCRYPTKEY key, HCRYPTKEY exp_key, DWORD type, DWORD flags, BYTE *data, DWORD *size)
{
    KeyObject *k = key_of(key);
    DWORD need;
    if (!k) return FALSE;
    if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (flags) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (type != PLAINTEXTKEYBLOB) { SetLastError(exp_key ? NTE_BAD_KEY_ : NTE_BAD_TYPE_); return FALSE; }
    if (exp_key) { SetLastError(NTE_BAD_KEY_); return FALSE; }
    need = sizeof(PlainBlobHeader) + k->key_length;
    if (!data) { *size = need; return TRUE; }
    if (*size < need) { *size = need; SetLastError(ERROR_MORE_DATA); return FALSE; }
    {
        PlainBlobHeader *h = (PlainBlobHeader *)data;
        h->type = PLAINTEXTKEYBLOB; h->version = CUR_BLOB_VERSION; h->reserved = 0; h->alg = k->alg; h->length = k->key_length;
        memcpy(data + sizeof(*h), k->key, k->key_length);
    }
    *size = need;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptImportKey(HCRYPTPROV p, const BYTE *data, DWORD size, HCRYPTKEY pub_key, DWORD flags, HCRYPTKEY *out)
{
    const PlainBlobHeader *h = (const PlainBlobHeader *)data;
    if (!provider_ok(p)) return FALSE;
    if (!data || !out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (flags & ~(CRYPT_EXPORTABLE | CRYPT_NO_SALT)) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (size < sizeof(*h)) { SetLastError(NTE_BAD_DATA_); return FALSE; }
    if (h->type != PLAINTEXTKEYBLOB || pub_key) { SetLastError(h->type == PLAINTEXTKEYBLOB ? NTE_BAD_KEY_ : NTE_BAD_TYPE_); return FALSE; }
    if (h->version != CUR_BLOB_VERSION) { SetLastError(NTE_BAD_VER); return FALSE; }
    if (h->length > size - sizeof(*h)) { SetLastError(NTE_BAD_DATA_); return FALSE; }
    return make_key(h->alg, data + sizeof(*h), h->length, out) != NULL;
}
__declspec(dllexport) BOOL WINAPI CryptGetUserKey(HCRYPTPROV p, DWORD spec, HCRYPTKEY *out)
{
    if (!provider_ok(p)) return FALSE;
    if (!out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    SetLastError(NTE_NO_KEY_);
    return FALSE;
}
__declspec(dllexport) BOOL WINAPI CryptGetKeyParam(HCRYPTKEY h, DWORD param, BYTE *data, DWORD *size, DWORD flags)
{
    KeyObject *k = key_of(h);
    DWORD v = 0, n = 4;
    const BYTE *src = NULL;
    if (!k) return FALSE;
    if (flags) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    switch (param) {
    case KP_ALGID: v = k->alg; break;
    case KP_KEYLEN: v = k->key_length * 8; break;
    case KP_BLOCKLEN: v = block_size_of(k->alg) * 8; break;
    case KP_MODE: v = k->mode; break;
    case KP_PADDING: v = k->padding; break;
    case KP_MODE_BITS: v = 8; break;
    case KP_EFFECTIVE_KEYLEN: v = k->effective_bits; break;
    case KP_PERMISSIONS: v = CRYPT_ENCRYPT | CRYPT_DECRYPT | CRYPT_EXPORT | CRYPT_READ | CRYPT_WRITE | CRYPT_MAC; break;
    case KP_IV: if (!block_size_of(k->alg)) { SetLastError(NTE_BAD_TYPE_); return FALSE; } src = k->iv; n = 16; break;
    case KP_SALT: src = k->salt; n = k->salt_length; break;
    default: SetLastError(NTE_BAD_TYPE_); return FALSE;
    }
    if (!data) { *size = n; return TRUE; }
    if (*size < n) { *size = n; SetLastError(ERROR_MORE_DATA); return FALSE; }
    if (src) memcpy(data, src, n); else *(DWORD *)data = v;
    *size = n;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptSetKeyParam(HCRYPTKEY h, DWORD param, const BYTE *data, DWORD flags)
{
    KeyObject *k = key_of(h);
    if (!k) return FALSE;
    if (flags) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (!data) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    switch (param) {
    case KP_IV: if (!block_size_of(k->alg)) { SetLastError(NTE_BAD_TYPE_); return FALSE; } memcpy(k->iv, data, 16); k->iv_set = TRUE; return TRUE;
    case KP_MODE: {
        DWORD m = *(const DWORD *)data;
        if (!block_size_of(k->alg) || (m != CRYPT_MODE_CBC && m != CRYPT_MODE_ECB)) { SetLastError(NTE_BAD_DATA_); return FALSE; }
        k->mode = m;
        return TRUE;
    }
    case KP_PADDING: if (*(const DWORD *)data != PKCS5_PADDING) { SetLastError(NTE_BAD_DATA_); return FALSE; } return TRUE;
    case KP_SALT: if (k->alg != CALG_RC4) { SetLastError(NTE_BAD_TYPE_); return FALSE; } k->salt_length = 11; memcpy(k->salt, data, 11); prepare_key(k); return TRUE;
    case KP_EFFECTIVE_KEYLEN: k->effective_bits = *(const DWORD *)data; return TRUE;
    case KP_PERMISSIONS: case KP_MODE_BITS: return TRUE;
    default: SetLastError(NTE_BAD_TYPE_); return FALSE;
    }
}

/* ---- encrypting and decrypting ---------------------------------------------------------------------------------------------------------------------------------- */

__declspec(dllexport) BOOL WINAPI CryptEncrypt(HCRYPTKEY key, HCRYPTHASH hash, BOOL final, DWORD flags, BYTE *data, DWORD *size, DWORD capacity)
{
    KeyObject *k = key_of(key);
    HashObject *h = NULL;
    DWORD len, block;
    if (!k) return FALSE;
    if (hash && !(h = hash_of(hash))) return FALSE;
    if (flags & ~CRYPT_OAEP) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (!size || (!data && *size && capacity)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    len = *size;
    block = block_size_of(k->alg);
    if (h) {
        if (h->finished) { SetLastError(NTE_BAD_HASH_STATE_); return FALSE; }
        if (len > capacity) len = capacity;
        if (data) hash_update(&h->hash, data, len < *size ? len : *size);
        len = *size;
    }
    if (!block) {      /* a stream cipher */
        if (len > capacity) { *size = len; SetLastError(ERROR_MORE_DATA); return FALSE; }
        if (!data) { *size = len; return TRUE; }
        rc4_crypt(&k->rc4, data, len);
        return TRUE;
    }
    {
        DWORD padded = final ? (len / block + 1) * block : len;
        BYTE previous[16], *p = data;
        DWORD off;
        if (!final && len % block) { SetLastError(NTE_BAD_DATA_); return FALSE; }
        if (padded > capacity || !data) { *size = padded; if (data || capacity) SetLastError(ERROR_MORE_DATA); else if (!data) return TRUE; return FALSE; }
        if (final) memset(data + len, (int)(padded - len), padded - len);
        memcpy(previous, k->iv, 16);
        for (off = 0; off < padded; off += 16) {
            BYTE blk[16];
            DWORD i;
            memcpy(blk, p + off, 16);
            if (k->mode == CRYPT_MODE_CBC) for (i = 0; i < 16; ++i) blk[i] ^= previous[i];
            aes_encrypt_block(&k->aes, blk, p + off);
            memcpy(previous, p + off, 16);
        }
        memcpy(k->iv, previous, 16);
        *size = padded;
    }
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptDecrypt(HCRYPTKEY key, HCRYPTHASH hash, BOOL final, DWORD flags, BYTE *data, DWORD *size)
{
    KeyObject *k = key_of(key);
    HashObject *h = NULL;
    DWORD len, block;
    if (!k) return FALSE;
    if (hash && !(h = hash_of(hash))) return FALSE;
    if (flags & ~CRYPT_OAEP) { SetLastError(NTE_BAD_FLAGS_); return FALSE; }
    if (!size || (!data && *size)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    len = *size;
    block = block_size_of(k->alg);
    if (!block) {
        if (data) rc4_crypt(&k->rc4, data, len);
        if (h && data) hash_update(&h->hash, data, len);
        return TRUE;
    }
    if (len % block || (final && len == 0)) { SetLastError(NTE_BAD_DATA_); return FALSE; }
    {
        BYTE previous[16], *p = data;
        DWORD off;
        memcpy(previous, k->iv, 16);
        for (off = 0; off < len; off += 16) {
            BYTE cipher[16], plain[16];
            DWORD i;
            memcpy(cipher, p + off, 16);
            aes_decrypt_block(&k->aes, cipher, plain);
            if (k->mode == CRYPT_MODE_CBC) for (i = 0; i < 16; ++i) plain[i] ^= previous[i];
            memcpy(p + off, plain, 16);
            memcpy(previous, cipher, 16);
        }
        memcpy(k->iv, previous, 16);
        if (final) {
            BYTE pad = p[len - 1];
            DWORD i;
            if (pad == 0 || pad > 16 || pad > len) { SetLastError(NTE_BAD_DATA_); return FALSE; }
            for (i = 0; i < pad; ++i) if (p[len - 1 - i] != pad) { SetLastError(NTE_BAD_DATA_); return FALSE; }
            len -= pad;
        }
        if (h) hash_update(&h->hash, data, len);
        *size = len;
    }
    return TRUE;
}

/* ---- signatures: no asymmetric keys here ------------------------------------------------------------------------------------------------------------------------ */

__declspec(dllexport) BOOL WINAPI CryptSignHashW(HCRYPTHASH h, DWORD spec, LPCWSTR description, DWORD flags, BYTE *signature, DWORD *length)
{
    (void)description; (void)flags; (void)signature; (void)length; (void)spec;
    if (!hash_of(h)) return FALSE;
    SetLastError(NTE_BAD_ALGID_);
    return FALSE;
}
__declspec(dllexport) BOOL WINAPI CryptSignHashA(HCRYPTHASH h, DWORD spec, LPCSTR description, DWORD flags, BYTE *signature, DWORD *length)
{
    (void)description;
    return CryptSignHashW(h, spec, NULL, flags, signature, length);
}
__declspec(dllexport) BOOL WINAPI CryptVerifySignatureW(HCRYPTHASH h, const BYTE *signature, DWORD length, HCRYPTKEY pub_key, LPCWSTR description, DWORD flags)
{
    (void)signature; (void)length; (void)description; (void)flags;
    if (!hash_of(h)) return FALSE;
    if (!key_of(pub_key)) return FALSE;
    SetLastError(NTE_BAD_ALGID_);
    return FALSE;
}
__declspec(dllexport) BOOL WINAPI CryptVerifySignatureA(HCRYPTHASH h, const BYTE *signature, DWORD length, HCRYPTKEY pub_key, LPCSTR description, DWORD flags)
{
    (void)description;
    return CryptVerifySignatureW(h, signature, length, pub_key, NULL, flags);
}

/* ---- providers: what there is ------------------------------------------------------------------------------------------------------------------------------------ */

static const struct { ALG_ID alg; DWORD bits; const char *name; } g_algs[] = {
    {CALG_MD4, 128, "MD4"}, {CALG_MD5, 128, "MD5"}, {CALG_SHA1, 160, "SHA-1"}, {CALG_SHA_256, 256, "SHA-256"}, {CALG_SHA_384, 384, "SHA-384"}, {CALG_SHA_512, 512, "SHA-512"},
    {CALG_HMAC, 0, "HMAC"}, {CALG_RC4, 128, "RC4"}, {CALG_AES_128, 128, "AES-128"}, {CALG_AES_192, 192, "AES-192"}, {CALG_AES_256, 256, "AES-256"}};
#define ALG_COUNT (sizeof(g_algs) / sizeof(g_algs[0]))
static DWORD g_enum_index;

__declspec(dllexport) BOOL WINAPI CryptGetProvParam(HCRYPTPROV p, DWORD param, BYTE *data, DWORD *size, DWORD flags)
{
    if (!provider_ok(p)) return FALSE;
    if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    switch (param) {
    case PP_NAME: {
        static const char name[] = "Microsoft Enhanced RSA and AES Cryptographic Provider";
        if (!data) { *size = sizeof(name); return TRUE; }
        if (*size < sizeof(name)) { *size = sizeof(name); SetLastError(ERROR_MORE_DATA); return FALSE; }
        memcpy(data, name, sizeof(name)); *size = sizeof(name);
        return TRUE;
    }
    case PP_VERSION: case PP_IMPTYPE: case PP_PROVTYPE: case PP_KEYSET_TYPE: case PP_SIG_KEYSIZE_INC: case PP_KEYX_KEYSIZE_INC: case PP_SESSION_KEYSIZE: {
        DWORD v = param == PP_VERSION ? 0x200 : (param == PP_IMPTYPE ? CRYPT_IMPL_SOFTWARE : (param == PP_PROVTYPE ? PROV_RSA_AES : (param == PP_SESSION_KEYSIZE ? 128 : 0)));
        if (!data) { *size = 4; return TRUE; }
        if (*size < 4) { *size = 4; SetLastError(ERROR_MORE_DATA); return FALSE; }
        *(DWORD *)data = v; *size = 4;
        return TRUE;
    }
    case PP_CONTAINER: case PP_UNIQUE_CONTAINER:
        SetLastError(NTE_BAD_KEYSET);
        return FALSE;
    case PP_ENUMALGS: case PP_ENUMALGS_EX: {
        const BOOL ex = param == PP_ENUMALGS_EX;
        DWORD need = ex ? sizeof(PROV_ENUMALGS_EX) : sizeof(PROV_ENUMALGS);
        if (flags & CRYPT_FIRST) g_enum_index = 0;
        if (!data) { *size = need; return TRUE; }
        if (g_enum_index >= ALG_COUNT) { SetLastError(ERROR_NO_MORE_ITEMS); return FALSE; }
        if (*size < need) { *size = need; SetLastError(ERROR_MORE_DATA); return FALSE; }
        memset(data, 0, need);
        {
            const char *n = g_algs[g_enum_index].name;
            if (ex) {
                PROV_ENUMALGS_EX *e = (PROV_ENUMALGS_EX *)data;
                e->aiAlgid = g_algs[g_enum_index].alg; e->dwDefaultLen = g_algs[g_enum_index].bits; e->dwMinLen = g_algs[g_enum_index].bits; e->dwMaxLen = g_algs[g_enum_index].bits;
                e->dwNameLen = (DWORD)strlen(n) + 1; strcpy(e->szName, n);
                e->dwLongNameLen = e->dwNameLen; strcpy(e->szLongName, n);
            } else {
                PROV_ENUMALGS *e = (PROV_ENUMALGS *)data;
                e->aiAlgid = g_algs[g_enum_index].alg; e->dwBitLen = g_algs[g_enum_index].bits; e->dwNameLen = (DWORD)strlen(n) + 1; strcpy(e->szName, n);
            }
        }
        ++g_enum_index;
        *size = need;
        return TRUE;
    }
    default: SetLastError(NTE_BAD_TYPE_); return FALSE;
    }
}
__declspec(dllexport) BOOL WINAPI CryptSetProvParam(HCRYPTPROV p, DWORD param, const BYTE *data, DWORD flags)
{
    (void)data; (void)flags;
    if (!provider_ok(p)) return FALSE;
    switch (param) {
    case PP_CLIENT_HWND: case PP_KEYSET_SEC_DESCR: case PP_UI_PROMPT: case PP_DELETEKEY: return TRUE;
    default: SetLastError(NTE_BAD_TYPE_); return FALSE;
    }
}

static BOOL supported_type(DWORD type) { return type == PROV_RSA_FULL || type == PROV_RSA_AES; }
__declspec(dllexport) BOOL WINAPI CryptEnumProvidersW(DWORD index, DWORD *reserved, DWORD flags, DWORD *type, LPWSTR name, DWORD *size)
{
    DWORD need = (DWORD)(wcslen(PROVIDER_NAME) + 1) * 2;
    if (reserved || flags) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (index > 0) { SetLastError(ERROR_NO_MORE_ITEMS); return FALSE; }
    if (type) *type = PROV_RSA_AES;
    if (!name) { *size = need; return TRUE; }
    if (*size < need) { *size = need; SetLastError(ERROR_MORE_DATA); return FALSE; }
    memcpy(name, PROVIDER_NAME, need);
    *size = need;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptEnumProvidersA(DWORD index, DWORD *reserved, DWORD flags, DWORD *type, LPSTR name, DWORD *size)
{
    WCHAR wide[80];
    DWORD n = sizeof(wide), bytes;
    if (!CryptEnumProvidersW(index, reserved, flags, type, wide, &n) && GetLastError() != ERROR_MORE_DATA) return FALSE;
    CryptEnumProvidersW(index, reserved, flags, type, wide, &n);
    bytes = (DWORD)WideCharToMultiByte(CP_ACP, 0, wide, -1, NULL, 0, NULL, NULL);
    if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!name) { *size = bytes; return TRUE; }
    if (*size < bytes) { *size = bytes; SetLastError(ERROR_MORE_DATA); return FALSE; }
    WideCharToMultiByte(CP_ACP, 0, wide, -1, name, bytes, NULL, NULL);
    *size = bytes;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptEnumProviderTypesW(DWORD index, DWORD *reserved, DWORD flags, DWORD *type, LPWSTR name, DWORD *size)
{
    static const WCHAR *const names[] = {L"RSA Full (Signature and Key Exchange)", L"RSA Full and AES"};
    static const DWORD types[] = {PROV_RSA_FULL, PROV_RSA_AES};
    DWORD need;
    if (reserved || flags || !size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (index >= 2) { SetLastError(ERROR_NO_MORE_ITEMS); return FALSE; }
    need = (DWORD)(wcslen(names[index]) + 1) * 2;
    if (type) *type = types[index];
    if (!name) { *size = need; return TRUE; }
    if (*size < need) { *size = need; SetLastError(ERROR_MORE_DATA); return FALSE; }
    memcpy(name, names[index], need);
    *size = need;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptEnumProviderTypesA(DWORD index, DWORD *reserved, DWORD flags, DWORD *type, LPSTR name, DWORD *size)
{
    WCHAR wide[64];
    DWORD n = sizeof(wide), bytes;
    if (!CryptEnumProviderTypesW(index, reserved, flags, type, wide, &n)) return FALSE;
    bytes = (DWORD)WideCharToMultiByte(CP_ACP, 0, wide, -1, NULL, 0, NULL, NULL);
    if (!size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!name) { *size = bytes; return TRUE; }
    if (*size < bytes) { *size = bytes; SetLastError(ERROR_MORE_DATA); return FALSE; }
    WideCharToMultiByte(CP_ACP, 0, wide, -1, name, bytes, NULL, NULL);
    *size = bytes;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptGetDefaultProviderW(DWORD type, DWORD *reserved, DWORD flags, LPWSTR name, DWORD *size)
{
    DWORD need = (DWORD)(wcslen(PROVIDER_NAME) + 1) * 2;
    if (reserved || (flags & ~(CRYPT_USER_DEFAULT | CRYPT_MACHINE_DEFAULT)) || !size) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!supported_type(type)) { SetLastError(NTE_PROV_TYPE_NOT_DEF_); return FALSE; }
    if (!name) { *size = need; return TRUE; }
    if (*size < need) { *size = need; SetLastError(ERROR_MORE_DATA); return FALSE; }
    memcpy(name, PROVIDER_NAME, need);
    *size = need;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptGetDefaultProviderA(DWORD type, DWORD *reserved, DWORD flags, LPSTR name, DWORD *size)
{
    WCHAR wide[80];
    DWORD n = sizeof(wide), bytes;
    if (!CryptGetDefaultProviderW(type, reserved, flags, wide, &n)) return FALSE;
    bytes = (DWORD)WideCharToMultiByte(CP_ACP, 0, wide, -1, NULL, 0, NULL, NULL);
    if (!name) { *size = bytes; return TRUE; }
    if (*size < bytes) { *size = bytes; SetLastError(ERROR_MORE_DATA); return FALSE; }
    WideCharToMultiByte(CP_ACP, 0, wide, -1, name, bytes, NULL, NULL);
    *size = bytes;
    return TRUE;
}
/* Naming a different provider as the default is an administrator's change of the machine; there is one provider here, so only choosing it again can succeed. */
__declspec(dllexport) BOOL WINAPI CryptSetProviderExW(LPCWSTR name, DWORD type, DWORD *reserved, DWORD flags)
{
    if (reserved || !name) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (flags & CRYPT_DELETEKEYSET) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if (!supported_type(type)) { SetLastError(NTE_PROV_TYPE_NOT_DEF_); return FALSE; }
    if (wcsicmp(name, PROVIDER_NAME)) { SetLastError(NTE_PROV_TYPE_NO_MATCH); return FALSE; }
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CryptSetProviderW(LPCWSTR name, DWORD type) { return CryptSetProviderExW(name, type, NULL, CRYPT_USER_DEFAULT); }
__declspec(dllexport) BOOL WINAPI CryptSetProviderExA(LPCSTR name, DWORD type, DWORD *reserved, DWORD flags)
{
    WCHAR *w = strdupAW(name);
    BOOL r = CryptSetProviderExW(w, type, reserved, flags);
    DWORD e = GetLastError();
    free(w);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI CryptSetProviderA(LPCSTR name, DWORD type) { return CryptSetProviderExA(name, type, NULL, CRYPT_USER_DEFAULT); }
