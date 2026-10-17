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

// Entropy that is the same every time for a given seed: lets a TLS client produce the same ClientHello, key shares and so on in every run,
// so that a server's answer recorded once (tests/data/make_tls_vectors.py) can be played back to it later. Never use this for real keys.
#pragma once
#include "Crypto/Random.hpp"

struct SeededEntropy final : crypto::IEntropySource {
  uint32_t state;
  explicit SeededEntropy(uint32_t seed) : state(seed * 2654435761u + 1) {}
  bool read(ByteView out) override {
    for (uint64_t i = 0; i < out.size(); ++i) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      out[i] = static_cast<uint8_t>(state >> 11);
    }
    return true;
  }
};
