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

#pragma once
#include <stdint.h>
#include "Crypto/Crypto.hpp"

namespace crypto {

// Where unpredictable bytes come from: the CPU's hardware generator (RDSEED / RDRAND), or whatever the platform provides.
class IEntropySource {
 public:
  // Fills `out` with unpredictable bytes. False if the source is not available or failed (callers must not go on with weak randomness).
  virtual bool read(ByteView out) = 0;

 protected:
  IEntropySource() = default;
  ~IEntropySource() = default;
};

// RDSEED, falling back to RDRAND, on x86-64 CPUs that have them (false from read() on others, or if the instruction keeps failing).
// Not exercised on real hardware by the host tests.
class CpuEntropySource final : public IEntropySource {
 public:
  bool read(ByteView out) override;
  static bool available();
};

// A source of random bytes for protocols (keys, nonces, client randoms).
class IRandom {
 public:
  virtual bool fill(ByteView out) = 0;

 protected:
  IRandom() = default;
  ~IRandom() = default;
};

// HMAC-DRBG with SHA-256 (NIST SP 800-90A) seeded from an entropy source and reseeded every 1 MiB of output.
class DrbgRandom final : public IRandom {
 public:
  explicit DrbgRandom(IEntropySource& entropy) : entropy_(entropy) {}
  // Seeds from 48 bytes of entropy (and `personalization`, up to 64 bytes). False if the source failed.
  bool initialize(ConstByteView personalization = ConstByteView());
  bool fill(ByteView out) override;
  bool ready() const { return ready_; }

 private:
  void update(ConstByteView provided);
  IEntropySource& entropy_;
  uint8_t k_[32], v_[32];
  uint64_t since_reseed_ = 0;
  bool ready_ = false;
};

}  // namespace crypto
