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

// AmdIrq::classify against a table written from the Linux driver's handlers (gfx_v8_0_eop_irq / _fault, sdma_v3_0_process_trap_irq / _illegal_inst_irq, the gmc_v8_0 fault
// sources), for every source id and ring id a vector can hold; AmdIrq::process with the interrupt ring itself is in AmdVegaMTest (it needs the whole card).
#include <stdio.h>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdIrq.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// The expected kind, spelled out per source rather than computed the way classify does it.
static AmdIrq::Kind expected(uint32_t src, uint32_t ring) {
  const uint32_t me = (ring >> 2) & 3;
  switch (src) {
    case 181: return me == 0 ? AmdIrq::Kind::GraphicsFence : (me == 1 || me == 2) ? AmdIrq::Kind::ComputeFence : AmdIrq::Kind::Unknown;
    case 184: return AmdIrq::Kind::GfxPrivilegedRegister;
    case 185: return AmdIrq::Kind::GfxPrivilegedInstruction;
    case 197: return AmdIrq::Kind::GfxEccError;
    case 224: return ((ring & 3) < 2 && ((ring >> 2) & 3) == 0) ? AmdIrq::Kind::SdmaTrap : AmdIrq::Kind::Unknown;
    case 241:
    case 247: return (ring & 3) < 2 ? AmdIrq::Kind::SdmaIllegalInstruction : AmdIrq::Kind::Unknown;
    case 146:
    case 147: return AmdIrq::Kind::VmFault;
    default: return AmdIrq::Kind::Unknown;
  }
}

static void test_classify_all() {
  uint32_t mismatches = 0;
  for (uint32_t src = 0; src < 256; ++src) {
    for (uint32_t ring = 0; ring < 256; ++ring) {
      AmdIh3::Vector v;
      v.src_id = src;
      v.ring_id = ring;
      v.src_data = 0x0ABCDEF;
      const AmdIrq::Event e = AmdIrq::classify(v);
      if (e.kind != expected(src, ring)) ++mismatches;
      if (e.vector.src_id != src || e.vector.ring_id != ring || e.vector.src_data != 0x0ABCDEF) ++mismatches;
      if (e.me != ((ring >> 2) & 3) || e.pipe != (ring & 3) || e.queue != ((ring >> 4) & 7)) ++mismatches;
    }
  }
  CHECK(mismatches == 0);
  // A few by hand: a graphics fence; compute queue 5 of pipe 2 on ME 1; SDMA engine 1; illegal instruction on engine 0.
  AmdIh3::Vector v;
  v.src_id = 181; v.ring_id = 0;
  CHECK(AmdIrq::classify(v).kind == AmdIrq::Kind::GraphicsFence);
  v.ring_id = (5u << 4) | (1u << 2) | 2;
  AmdIrq::Event e = AmdIrq::classify(v);
  CHECK(e.kind == AmdIrq::Kind::ComputeFence && e.me == 1 && e.pipe == 2 && e.queue == 5);
  v.src_id = 224; v.ring_id = 1;
  e = AmdIrq::classify(v);
  CHECK(e.kind == AmdIrq::Kind::SdmaTrap && e.engine == 1);
  v.src_id = 241; v.ring_id = 0;
  e = AmdIrq::classify(v);
  CHECK(e.kind == AmdIrq::Kind::SdmaIllegalInstruction && e.engine == 0);
  CHECK(AmdIrq::kind_name(AmdIrq::Kind::VmFault) && AmdIrq::kind_name(AmdIrq::Kind::Unknown));
}

int main() {
  test_classify_all();
  printf(g_failed ? "AmdIrqTest: %d FAILED\n" : "AmdIrqTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
