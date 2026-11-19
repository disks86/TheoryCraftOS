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

// The vendor probes a GpuPool gives the GPUs it has no driver for. Apart from GpuPool.cpp so that the pool's bookkeeping builds and is tested
// without the driver code.
#include "Hardware/GraphicsProcessingUnit/Multi/GpuProbe.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpuProbe.hpp"
#include "Hardware/GraphicsProcessingUnit/Intel/IntelGpuProbe.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaNative.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

namespace {

constexpr uint16_t kNvidiaVendor = 0x10DE;
constexpr uint16_t kAmdVendor = 0x1002;
constexpr uint16_t kIntelVendor = 0x8086;

// Appends `text` to the string in `out` at `*n`, keeping it terminated.
void append(ArrayView<char> out, uint32_t* n, const char* text) {
  for (const char* p = text; *p && *n + 1 < out.size(); ++p) out[(*n)++] = *p;
  out[*n] = 0;
}

// A discrete NVIDIA GPU: the native driver's read-only start (Nvkm: chip, VRAM size, VBIOS and board tables, sensors, the connectors' EDIDs),
// nothing that writes to the display or memory engines.
class NvidiaGpuProbe final : public IGpuProbe {
 public:
  NvidiaGpuProbe(const GpuContext& context, allocators::IAllocator& allocator) : native_(context, allocator) {
    native_.configure_secondary(nullptr);   // (the ACPI _ROM method is not reachable yet when the pool starts: the VBIOS comes from the card)
  }
  const char* name() const override { return name_[0] ? name_ : "NVIDIA GPU"; }
  const char* status() const override {
    // (What the devinit look found matters more than "identified" for a GPU that needed a POST.)
    switch (native_.post_report().outcome) {
      case nv::PostOutcome::DryRun: return "not POSTed: init scripts walked, not run (TCOS_NVIDIA_NATIVE)";
      case nv::PostOutcome::Ran: return "POSTed by this OS";
      case nv::PostOutcome::Failed: return native_.post_report().reason;
      default: return native_.status();
    }
  }
  bool start() override {
    if (!native_.start()) return false;
    uint32_t n = 0;
    const ArrayView<char> out(name_);
    append(out, &n, "NVIDIA ");
    append(out, &n, native_.chip().name);
    append(out, &n, " (");
    append(out, &n, nv::family_name(native_.chip().family));
    append(out, &n, ")");
    return true;
  }
  bool resume() override { return native_.resume(); }
  uint64_t vram_bytes() const override { return native_.vram_bytes(); }
  int32_t temperature_millicelsius() override { return native_.temperature_millicelsius(); }
  uint32_t connected_outputs() const override {
    uint32_t connected = 0;
    for (const nv::NvidiaNative::Connector& c : native_.connectors())
      if (c.connected) ++connected;
    return connected;
  }
  bool reads_outputs() const override { return !native_.connectors().empty(); }
  // The connectors whose hot-plug line changed are read again (their EDID over DDC / AUX).
  bool refresh_outputs() override { return native_.refresh_connectors(); }

 private:
  nv::NvidiaNative native_;
  char name_[48] = {};
};

}  // namespace

UniquePointer<IGpuProbe> make_vendor_gpu_probe(const GpuContext& context, allocators::IAllocator& allocator) {
  if (context.device.vendor_id == kNvidiaVendor && context.registers) return UniquePointer<IGpuProbe>::make_as<NvidiaGpuProbe>(allocator, context, allocator);
  // AMD and Intel: only the discrete chips the identification knows (AmdGpuProbe / IntelGpuProbe: chip, memory size, temperature; for AMD the VBIOS strings).
  const uint16_t device = context.device.device_id;
  if (context.device.vendor_id == kAmdVendor && context.registers && context.pci && AmdGpuProbe::supported(device))
    return UniquePointer<IGpuProbe>::make_as<AmdGpuProbe>(allocator, context, allocator);
  if (context.device.vendor_id == kIntelVendor && context.registers && context.pci && IntelGpuProbe::supported(device))
    return UniquePointer<IGpuProbe>::make_as<IntelGpuProbe>(allocator, context);
  return UniquePointer<IGpuProbe>();
}

}  // namespace hardware
