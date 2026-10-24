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

#include "Hardware/VideoCodec/AMD/AmdMediaEngineHw.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

namespace {
// Appends `bytes` bytes of the window, from its start, as little-endian words.
bool fetch(const MmioRegion& window, uint32_t bytes, ByteBuffer& out) {
  for (uint32_t offset = 0; offset < bytes; offset += 4)
    if (!out.put32(window.read32(offset))) return false;
  return true;
}
}  // namespace

bool AmdMediaEngineHw::supported(AmdGpuArchitecture architecture) {
  switch (architecture) {
    case AmdGpuArchitecture::Rdna1:
    case AmdGpuArchitecture::Rdna2:
    case AmdGpuArchitecture::Rdna3:
    case AmdGpuArchitecture::Rdna3_5:
    case AmdGpuArchitecture::Rdna4: return true;
    default: return false;
  }
}

namespace {
// Reads and parses the table at the start of `window`; `probed` is false if there is none there.
AmdMediaEngineStatus read_table(allocators::IAllocator& allocator, const MmioRegion& window) {
  AmdMediaEngineStatus status;
  if (window.length() < kAmdIpDiscoveryHeaderBytes) return status;
  ByteBuffer table(allocator);
  if (!fetch(window, kAmdIpDiscoveryHeaderBytes, table)) return status;
  const uint32_t declared = amd_ip_discovery_declared_size(table.view());
  if (declared == 0 || declared > window.length() || declared > AmdMediaEngineHw::kDiscoveryWindow) return status;   // no table here
  const uint32_t rounded = (declared + 3) & ~3u;
  if (rounded > window.length()) return status;
  table.clear();
  if (!fetch(window, rounded, table)) return status;
  status.discovery = amd_parse_ip_discovery(table.view());
  status.probed = status.discovery.valid;
  return status;
}
}  // namespace

bool AmdMmIndexVramReader::read32(uint64_t offset, uint32_t* value) {
  if (!ready() || (offset & 3) || (offset >> 31) > 0xFFFFFFFFull) return false;
  registers_.write32(kMmIndex, static_cast<uint32_t>(offset & 0x7FFFFFFFu) | kVramFlag);
  registers_.write32(kMmIndexHi, static_cast<uint32_t>(offset >> 31));
  *value = registers_.read32(kMmData);
  return true;
}

void AmdMmIndexVramReader::park() {
  if (!ready()) return;
  registers_.write32(kMmIndex, 0);
  registers_.write32(kMmIndexHi, 0);
}

AmdMediaEngineStatus AmdMediaEngineHw::probe(allocators::IAllocator& allocator, const MmioRegion& window, AmdGpuArchitecture architecture) {
  if (!supported(architecture)) return AmdMediaEngineStatus();
  return read_table(allocator, window);
}

AmdMediaEngineStatus AmdMediaEngineHw::probe_video_memory(allocators::IAllocator& allocator, IAmdVramReader& reader, AmdGpuArchitecture architecture, uint64_t max_bytes) {
  constexpr uint64_t kMiB = 1ull << 20;
  constexpr uint32_t kSignature = 0x28211407;
  AmdMediaEngineStatus status;
  if (!supported(architecture)) return status;
  const uint64_t limit = max_bytes < kMaxVideoMemory ? max_bytes : kMaxVideoMemory;
  for (uint64_t size = kMiB; size <= limit; size += kMiB) {
    const uint64_t offset = size - kDiscoveryWindow;
    uint32_t word = 0;
    if (!reader.read32(offset, &word)) return AmdMediaEngineStatus();
    if (word != kSignature) continue;
    ByteBuffer table(allocator);
    bool ok = true;
    for (uint32_t i = 0; ok && i < kAmdIpDiscoveryHeaderBytes; i += 4) {
      uint32_t w = 0;
      ok = reader.read32(offset + i, &w) && table.put32(w);
    }
    const uint32_t declared = ok ? amd_ip_discovery_declared_size(table.view()) : 0;
    if (declared == 0 || declared > kDiscoveryWindow) continue;
    for (uint32_t i = kAmdIpDiscoveryHeaderBytes; ok && i < ((declared + 3) & ~3u); i += 4) {
      uint32_t w = 0;
      ok = reader.read32(offset + i, &w) && table.put32(w);
    }
    if (!ok) continue;
    status.discovery = amd_parse_ip_discovery(table.view());
    status.probed = status.discovery.valid && status.discovery.checksum_ok;
    if (status.probed) {
      status.video_memory_bytes = size;
      return status;
    }
    status = AmdMediaEngineStatus();
  }
  return status;
}

AmdMediaEngineStatus AmdMediaEngineHw::probe_aperture(allocators::IAllocator& allocator, const MmioRegion& aperture, AmdGpuArchitecture architecture) {
  constexpr uint64_t kMiB = 1ull << 20;
  constexpr uint32_t kSignature = 0x28211407;   // the first word of the table
  if (!supported(architecture) || aperture.length() < kDiscoveryWindow) return AmdMediaEngineStatus();
  for (uint64_t boundary = aperture.length() / kMiB * kMiB; boundary >= kMiB; boundary -= kMiB) {
    const uint64_t offset = boundary - kDiscoveryWindow;
    if (aperture.read32(offset) != kSignature) continue;
    AmdMediaEngineStatus status = read_table(allocator, aperture.subregion(offset, kDiscoveryWindow));
    if (!status.probed || !status.discovery.checksum_ok) continue;   // a stale or damaged copy: keep looking lower
    status.carve_out_bytes = boundary;
    return status;
  }
  return AmdMediaEngineStatus();
}

}  // namespace hardware
