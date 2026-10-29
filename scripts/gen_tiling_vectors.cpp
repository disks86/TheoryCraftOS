// Prints reference surface sizes and pixel addresses for the VEGAM's golden tile mode table, computed by AMD's address library (the copy in Mesa, src/amd/addrlib; MIT license).
// tests/data/gcn_tiling_vectors.txt is its output; tests/GcnTextureTest.cpp compares GcnTiling against it.
//
// usage (M is Mesa's src/amd/addrlib; run from the repository root):
//   g++ -std=c++17 -w -DLITTLEENDIAN_CPU -I$M/inc -I$M/src -I$M/src/core -I$M/src/r800 -I$M/src/gfx9 -I$M/src/gfx10 -I$M/src/gfx11 -I$M/src/gfx12 \
//       -I$M/src/chip/r800 -I$M/src/chip/gfx9 -I$M/src/chip/gfx10 -I$M/src/chip/gfx11 -I$M/src/chip/gfx12 -Isrc/Hardware/GraphicsProcessingUnit/AMD \
//       scripts/gen_tiling_vectors.cpp $(find $M -name '*.cpp') -o genvec && ./genvec > tests/data/gcn_tiling_vectors.txt
// (-Isrc/Hardware/GraphicsProcessingUnit/AMD finds AmdGfx8Tables.inc)
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "addrinterface.h"
#include "amdgpu_asic_addr.h"
#include "AmdGfx8Tables.inc"

static void* ADDR_API alloc_cb(const ADDR_ALLOCSYSMEM_INPUT* in) { return malloc(in->sizeInBytes); }
static ADDR_E_RETURNCODE ADDR_API free_cb(const ADDR_FREESYSMEM_INPUT* in) { free(in->pVirtAddr); return ADDR_OK; }

int main() {
  ADDR_CREATE_INPUT ci;
  memset(&ci, 0, sizeof ci);
  ci.size = sizeof ci;
  ci.chipEngine = 0xA;                       // CIASICIDGFXENGINE_SOUTHERNISLAND
  ci.chipFamily = FAMILY_VI;
  ci.chipRevision = 0x6E;                    // VEGAM_P_A0 (AMDGPU_VEGAM_RANGE starts here)
  ci.callbacks.allocSysMem = alloc_cb;
  ci.callbacks.freeSysMem = free_cb;
  ci.createFlags.value = 0;
  ci.createFlags.useTileIndex = 1;
  ci.regValue.gbAddrConfig = 0x22011003;
  ci.regValue.pTileConfig = kTileModes;
  ci.regValue.noOfEntries = 32;
  ci.regValue.pMacroTileConfig = kMacrotileModes;
  ci.regValue.noOfMacroEntries = 16;
  ADDR_CREATE_OUTPUT co;
  memset(&co, 0, sizeof co);
  co.size = sizeof co;
  if (AddrCreate(&ci, &co) != ADDR_OK) { fprintf(stderr, "AddrCreate failed\n"); return 1; }
  const uint32_t indices[] = {0, 1, 2, 3, 10, 14};          // the 2D tiled entries GcnTiling implements
  const uint32_t bpps[] = {8, 32, 128};
  const uint32_t dims[][2] = {{40, 30}, {300, 200}, {17, 5}};
  for (const uint32_t idx : indices)
    for (const uint32_t bpp : bpps)
      for (const auto& d : dims) {
        const uint32_t reg = kTileModes[idx];
        ADDR_COMPUTE_SURFACE_INFO_INPUT in;
        memset(&in, 0, sizeof in);
        in.size = sizeof in;
        in.tileMode = static_cast<AddrTileMode>((reg >> 2) & 0xF);
        in.tileType = static_cast<AddrTileType>((reg >> 22) & 7);
        in.tileIndex = idx;
        in.bpp = bpp;
        in.numSamples = 1;
        in.width = d[0];
        in.height = d[1];
        in.numSlices = 1;
        in.flags.color = in.tileType != ADDR_DEPTH_SAMPLE_ORDER;
        in.flags.depth = in.tileType == ADDR_DEPTH_SAMPLE_ORDER;
        in.flags.display = in.tileType == ADDR_DISPLAYABLE;
        ADDR_TILEINFO ti;
        memset(&ti, 0, sizeof ti);
        in.pTileInfo = &ti;
        ADDR_COMPUTE_SURFACE_INFO_OUTPUT out;
        memset(&out, 0, sizeof out);
        out.size = sizeof out;
        ADDR_TILEINFO ti_out;
        memset(&ti_out, 0, sizeof ti_out);
        out.pTileInfo = &ti_out;
        if (AddrComputeSurfaceInfo(co.hLib, &in, &out) != ADDR_OK) { fprintf(stderr, "info failed idx %u bpp %u\n", idx, bpp); continue; }
        if (out.tileMode != in.tileMode) continue;          // the library reduced the mode (a small surface): not what the index names
        printf("info %u %u %u %u %u %u %llu %u\n", idx, bpp, d[0], d[1], out.pitch, out.height, static_cast<unsigned long long>(out.surfSize), out.baseAlign);
        for (uint32_t k = 0; k < 40; ++k) {
          const uint32_t x = (k * 37u + k * k) % d[0], y = (k * 53u + 3 * k) % d[1];
          ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT ai;
          memset(&ai, 0, sizeof ai);
          ai.size = sizeof ai;
          ai.x = x; ai.y = y; ai.bpp = bpp;
          ai.pitch = out.pitch; ai.height = out.height;
          ai.numSlices = 1; ai.numSamples = 1;
          ai.tileMode = out.tileMode;
          ai.isDepth = in.flags.depth;
          ai.tileType = in.tileType;
          ai.tileIndex = idx;
          ai.pTileInfo = &ti_out;
          ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT ao;
          memset(&ao, 0, sizeof ao);
          ao.size = sizeof ao;
          if (AddrComputeSurfaceAddrFromCoord(co.hLib, &ai, &ao) != ADDR_OK) { fprintf(stderr, "addr failed\n"); continue; }
          printf("addr %u %u %u %u %u %u %llu\n", idx, bpp, d[0], d[1], x, y, static_cast<unsigned long long>(ao.addr));
        }
      }

  // The mip chains and slices of the 1D tiled layouts (entries 9 and 13): the size of every level as the library gives it, the way Mesa's drivers ask for it (the base level and, for the
  // levels, the width and height shifted down, `basePitch` the base level's pitch, pow2Pad on when there is more than one level). Lines: "level idx bpp type w h depth levels k pitch height
  // slices size align" with type 0 2D, 1 3D, 2 cube, 3 array, and slices the number of slices the library was asked for.
  const uint32_t mip_indices[] = {9, 13};
  const struct { uint32_t type, w, h, depth, levels; } chains[] = {{0, 8, 8, 1, 4}, {0, 20, 12, 1, 5}, {0, 37, 5, 1, 6}, {0, 64, 64, 1, 7}, {1, 4, 4, 4, 3}, {1, 16, 8, 6, 5}, {2, 8, 8, 6, 4},
                                                                  {2, 32, 32, 6, 6}, {3, 10, 7, 3, 4}, {3, 16, 16, 5, 1}, {0, 17, 9, 1, 1}, {1, 9, 9, 9, 1}};
  for (const uint32_t idx : mip_indices)
    for (const uint32_t bpp : {8u, 32u, 128u})
      for (const auto& c : chains) {
        uint32_t base_pitch = 0;
        for (uint32_t k = 0; k < c.levels; ++k) {
          const uint32_t reg = kTileModes[idx];
          ADDR_COMPUTE_SURFACE_INFO_INPUT in;
          memset(&in, 0, sizeof in);
          in.size = sizeof in;
          in.tileMode = static_cast<AddrTileMode>((reg >> 2) & 0xF);
          in.tileType = static_cast<AddrTileType>((reg >> 22) & 7);
          in.tileIndex = idx;
          in.bpp = bpp;
          in.numSamples = 1;
          in.width = (c.w >> k) > 0 ? c.w >> k : 1;
          in.height = (c.h >> k) > 0 ? c.h >> k : 1;
          in.numSlices = c.type == 1 ? ((c.depth >> k) > 0 ? c.depth >> k : 1) : c.depth;
          in.mipLevel = k;
          in.basePitch = base_pitch;
          in.flags.color = 1;                       // (not the display flag: that is for scanout surfaces and aligns the pitch to 32; a texture only has the display micro tile order)
          in.flags.cube = c.type == 2;
          in.flags.volume = c.type == 1;
          in.flags.pow2Pad = c.levels > 1;
          ADDR_TILEINFO ti;
          memset(&ti, 0, sizeof ti);
          in.pTileInfo = &ti;
          ADDR_COMPUTE_SURFACE_INFO_OUTPUT out;
          memset(&out, 0, sizeof out);
          out.size = sizeof out;
          ADDR_TILEINFO ti_out;
          memset(&ti_out, 0, sizeof ti_out);
          out.pTileInfo = &ti_out;
          if (AddrComputeSurfaceInfo(co.hLib, &in, &out) != ADDR_OK) { fprintf(stderr, "level info failed idx %u bpp %u\n", idx, bpp); break; }
          if (k == 0) base_pitch = out.pitch;
          printf("level %u %u %u %u %u %u %u %u %u %u %u %llu %u\n", idx, bpp, c.type, c.w, c.h, c.depth, c.levels, k, out.pitch, out.height, in.numSlices, static_cast<unsigned long long>(out.surfSize),
                 out.baseAlign);
        }
      }
  return 0;
}
