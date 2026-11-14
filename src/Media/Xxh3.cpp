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

#include "Media/Xxh3.hpp"

namespace media {

namespace {

constexpr uint64_t kPrime32_1 = 0x9E3779B1u, kPrime32_2 = 0x85EBCA77u, kPrime32_3 = 0xC2B2AE3Du;
constexpr uint64_t kPrime64_1 = 0x9E3779B185EBCA87ull, kPrime64_2 = 0xC2B2AE3D27D4EB4Full, kPrime64_3 = 0x165667B19E3779F9ull,
                   kPrime64_4 = 0x85EBCA77C2B2AE63ull, kPrime64_5 = 0x27D4EB2F165667C5ull;
constexpr uint64_t kPrimeMx1 = 0x165667919E3779F9ull, kPrimeMx2 = 0x9FB21C651E98DF25ull;

constexpr uint64_t kSecretSize = 192;
constexpr uint8_t kSecret[kSecretSize] = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c, 0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb,
    0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f, 0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
    0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c, 0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb,
    0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3, 0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
    0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d, 0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31,
    0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64, 0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
    0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e, 0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc,
    0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce, 0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
};

// Little-endian loads. Every caller passes an offset with `bytes` bytes after it in `data` (the lengths are checked by the size classes below),
// so these go through the bounds-checked view without failing.
uint64_t load(ConstByteView data, uint64_t at, uint32_t bytes) {
  const ConstByteView part = data.subview(at, bytes);
  uint64_t value = 0;
  for (uint32_t i = 0; i < bytes; ++i) value |= uint64_t(part[i]) << (8 * i);
  return value;
}
uint64_t le64(ConstByteView data, uint64_t at) { return load(data, at, 8); }
uint64_t le32(ConstByteView data, uint64_t at) { return load(data, at, 4); }
uint64_t secret64(uint64_t at) { return le64(ConstByteView(kSecret), at); }
uint64_t secret32(uint64_t at) { return le32(ConstByteView(kSecret), at); }

uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
uint64_t swap64(uint64_t x) {
  uint64_t r = 0;
  for (int i = 0; i < 8; ++i) r |= ((x >> (8 * i)) & 0xFF) << (56 - 8 * i);
  return r;
}

uint64_t mul128_fold64(uint64_t a, uint64_t b) {
  const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
  return static_cast<uint64_t>(product) ^ static_cast<uint64_t>(product >> 64);
}
uint64_t xxh64_avalanche(uint64_t h) {
  h ^= h >> 33;
  h *= kPrime64_2;
  h ^= h >> 29;
  h *= kPrime64_3;
  return h ^ (h >> 32);
}
uint64_t avalanche(uint64_t h) {
  h ^= h >> 37;
  h *= kPrimeMx1;
  return h ^ (h >> 32);
}
uint64_t rrmxmx(uint64_t h, uint64_t length) {
  h ^= rotl(h, 49) ^ rotl(h, 24);
  h *= kPrimeMx2;
  h ^= (h >> 35) + length;
  h *= kPrimeMx2;
  return h ^ (h >> 28);
}
uint64_t mix16(ConstByteView data, uint64_t at, uint64_t secret_at) {
  return mul128_fold64(le64(data, at) ^ secret64(secret_at), le64(data, at + 8) ^ secret64(secret_at + 8));
}

uint64_t hash_0_to_16(ConstByteView data) {
  const uint64_t n = data.size();
  if (n > 8) {
    const uint64_t low = le64(data, 0) ^ (secret64(24) ^ secret64(32));
    const uint64_t high = le64(data, n - 8) ^ (secret64(40) ^ secret64(48));
    return avalanche(n + swap64(low) + high + mul128_fold64(low, high));
  }
  if (n >= 4) {
    const uint64_t first = le32(data, 0), last = le32(data, n - 4);
    const uint64_t keyed = (last + (first << 32)) ^ (secret64(8) ^ secret64(16));
    return rrmxmx(keyed, n);
  }
  if (n > 0) {
    const uint32_t combined = (uint32_t(data[0]) << 16) | (uint32_t(data[n >> 1]) << 24) | uint32_t(data[n - 1]) | (uint32_t(n) << 8);
    return xxh64_avalanche(uint64_t(combined) ^ (secret32(0) ^ secret32(4)));
  }
  return xxh64_avalanche(secret64(56) ^ secret64(64));
}

uint64_t hash_17_to_128(ConstByteView data) {
  const uint64_t n = data.size();
  uint64_t acc = n * kPrime64_1;
  if (n > 32) {
    if (n > 64) {
      if (n > 96) {
        acc += mix16(data, 48, 96);
        acc += mix16(data, n - 64, 112);
      }
      acc += mix16(data, 32, 64);
      acc += mix16(data, n - 48, 80);
    }
    acc += mix16(data, 16, 32);
    acc += mix16(data, n - 32, 48);
  }
  acc += mix16(data, 0, 0);
  acc += mix16(data, n - 16, 16);
  return avalanche(acc);
}

uint64_t hash_129_to_240(ConstByteView data) {
  constexpr uint64_t kMinSecret = 136, kLastOffset = 17, kStartOffset = 3;
  const uint64_t n = data.size(), rounds = n / 16;
  uint64_t acc = n * kPrime64_1;
  for (uint64_t i = 0; i < 8; ++i) acc += mix16(data, 16 * i, 16 * i);
  uint64_t acc_end = mix16(data, n - 16, kMinSecret - kLastOffset);
  acc = avalanche(acc);
  for (uint64_t i = 8; i < rounds; ++i) acc_end += mix16(data, 16 * i, 16 * (i - 8) + kStartOffset);
  return avalanche(acc + acc_end);
}

constexpr uint64_t kStripe = 64, kStripesPerBlock = (kSecretSize - kStripe) / 8, kBlock = kStripe * kStripesPerBlock;

void accumulate_512(uint64_t acc[8], ConstByteView data, uint64_t at, uint64_t secret_at) {
  const ConstByteView stripe = data.subview(at, kStripe);
  for (uint64_t i = 0; i < 8; ++i) {
    const uint64_t value = le64(stripe, 8 * i), key = value ^ secret64(secret_at + 8 * i);
    acc[i ^ 1] += value;
    acc[i] += (key & 0xFFFFFFFFu) * (key >> 32);
  }
}
void scramble(uint64_t acc[8]) {
  for (uint64_t i = 0; i < 8; ++i) {
    uint64_t a = acc[i];
    a ^= a >> 47;
    a ^= secret64(kSecretSize - kStripe + 8 * i);
    acc[i] = a * kPrime32_1;
  }
}

uint64_t hash_long(ConstByteView data) {
  const uint64_t n = data.size();
  uint64_t acc[8] = {kPrime32_3, kPrime64_1, kPrime64_2, kPrime64_3, kPrime64_4, kPrime32_2, kPrime64_5, kPrime32_1};
  const uint64_t blocks = (n - 1) / kBlock;
  for (uint64_t b = 0; b < blocks; ++b) {
    for (uint64_t s = 0; s < kStripesPerBlock; ++s) accumulate_512(acc, data, b * kBlock + s * kStripe, s * 8);
    scramble(acc);
  }
  const uint64_t stripes = ((n - 1) - kBlock * blocks) / kStripe;
  for (uint64_t s = 0; s < stripes; ++s) accumulate_512(acc, data, blocks * kBlock + s * kStripe, s * 8);
  accumulate_512(acc, data, n - kStripe, kSecretSize - kStripe - 7);      // the last stripe
  uint64_t result = n * kPrime64_1;
  for (uint64_t i = 0; i < 4; ++i) result += mul128_fold64(acc[2 * i] ^ secret64(11 + 16 * i), acc[2 * i + 1] ^ secret64(11 + 16 * i + 8));
  return avalanche(result);
}

}  // namespace

uint64_t xxh3_64(ConstByteView data) {
  const uint64_t n = data.size();
  if (n <= 16) return hash_0_to_16(data);
  if (n <= 128) return hash_17_to_128(data);
  if (n <= 240) return hash_129_to_240(data);
  return hash_long(data);
}

}  // namespace media
