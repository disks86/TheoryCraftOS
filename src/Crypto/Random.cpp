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

#include "Crypto/Random.hpp"

namespace crypto {

namespace {
constexpr uint64_t kReseedBytes = 1u << 20;
}

bool CpuEntropySource::available() {
#if defined(__x86_64__)
  uint32_t eax = 1, ebx, ecx, edx;
  asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
  return (ecx >> 30) & 1;   // RDRAND
#else
  return false;
#endif
}

bool CpuEntropySource::read(ByteView out) {
#if defined(__x86_64__)
  if (!available()) return false;
  // RDSEED is on CPUs from about 2014 on (CPUID.7.EBX bit 18); where it is missing RDRAND's output is conditioned by the CPU as well.
  uint32_t eax = 7, ebx = 0, ecx = 0, edx;
  asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
  const bool seed = (ebx >> 18) & 1;
  uint64_t pos = 0;
  while (pos < out.size()) {
    uint64_t value = 0;
    unsigned char ok = 0;
    for (int attempt = 0; attempt < 100 && !ok; ++attempt) {
      if (seed) asm volatile("rdseed %0; setc %1" : "=r"(value), "=qm"(ok) : : "cc");
      else asm volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok) : : "cc");
    }
    if (!ok) return false;
    for (int i = 0; i < 8 && pos < out.size(); ++i, ++pos) out[pos] = static_cast<uint8_t>(value >> (8 * i));
  }
  return true;
#else
  (void)out;
  return false;
#endif
}

// SP 800-90A 10.1.2.2: K = HMAC(K, V | 0x00 | data); V = HMAC(K, V); and again with 0x01 if there was data.
void DrbgRandom::update(ConstByteView provided) {
  uint8_t message[32 + 1 + 128];
  for (int round = 0; round < (provided.empty() ? 1 : 2); ++round) {
    uint32_t n = 0;
    for (int i = 0; i < 32; ++i) message[n++] = v_[i];
    message[n++] = static_cast<uint8_t>(round);
    for (uint64_t i = 0; i < provided.size() && i < 128; ++i) message[n++] = provided[i];
    hmac(HashKind::Sha256, ConstByteView(k_, 32), ConstByteView(message, n), k_);
    hmac(HashKind::Sha256, ConstByteView(k_, 32), ConstByteView(v_, 32), v_);
  }
}

bool DrbgRandom::initialize(ConstByteView personalization) {
  uint8_t seed[48 + 64];
  if (personalization.size() > 64 || !entropy_.read(ByteView(seed, 48))) return false;
  uint32_t n = 48;
  for (uint64_t i = 0; i < personalization.size(); ++i) seed[n++] = personalization[i];
  for (int i = 0; i < 32; ++i) {
    k_[i] = 0;
    v_[i] = 1;
  }
  update(ConstByteView(seed, n));
  for (uint32_t i = 0; i < n; ++i) seed[i] = 0;
  since_reseed_ = 0;
  ready_ = true;
  return true;
}

bool DrbgRandom::fill(ByteView out) {
  if (!ready_) return false;
  if (since_reseed_ >= kReseedBytes) {
    uint8_t fresh[48];
    if (!entropy_.read(ByteView(fresh, 48))) return false;
    update(ConstByteView(fresh, 48));
    since_reseed_ = 0;
  }
  for (uint64_t pos = 0; pos < out.size();) {
    hmac(HashKind::Sha256, ConstByteView(k_, 32), ConstByteView(v_, 32), v_);
    const uint64_t take = out.size() - pos < 32 ? out.size() - pos : 32;
    for (uint64_t i = 0; i < take; ++i) out[pos + i] = v_[i];
    pos += take;
  }
  update(ConstByteView());
  since_reseed_ += out.size();
  return true;
}

}  // namespace crypto
