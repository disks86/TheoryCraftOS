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


// Host-side tests for the Radeon RX Vega M graphics engine start-up: the microcode files (the real ones from /firmware: every field of the parsed headers, truncated and
// bit-flipped files under the sanitizers), the SMC microcode load (the table of contents the SMC would read, the images in video memory, the message order, every way
// the SMC can fail) and the engine start (golden registers, RLC, command processor ring, the ring test) against a simulated card whose command processor only runs
// packets once the SMC has loaded its microcode. Expected values are typed in from the Linux driver's tables and worked out by hand, not taken from the code under test.
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Firmware.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdSmu7Ucode.hpp"
#include "FakeAmdGfx8.hpp"

using namespace hardware;

namespace {
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Tables.inc"
constexpr const uint32_t* kMacroModes = kMacrotileModes;
}  // namespace

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static std::vector<uint8_t> read_file(const char* path) {
  std::vector<uint8_t> bytes;
  FILE* f = fopen(path, "rb");
  if (!f) return bytes;
  uint8_t buffer[4096];
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
  fclose(f);
  return bytes;
}
static uint32_t le32(const std::vector<uint8_t>& v, size_t at) { return v[at] | v[at + 1] << 8 | v[at + 2] << 16 | uint32_t{v[at + 3]} << 24; }

struct Files {
  std::vector<uint8_t> pfp, me, ce, mec, mec2, rlc, sdma0, sdma1;
  bool load() {
    pfp = read_file("firmware/vegam_pfp.bin"); me = read_file("firmware/vegam_me.bin"); ce = read_file("firmware/vegam_ce.bin");
    mec = read_file("firmware/vegam_mec.bin"); mec2 = read_file("firmware/vegam_mec2.bin"); rlc = read_file("firmware/vegam_rlc.bin");
    sdma0 = read_file("firmware/vegam_sdma.bin"); sdma1 = read_file("firmware/vegam_sdma1.bin");
    return !pfp.empty() && !me.empty() && !ce.empty() && !mec.empty() && !mec2.empty() && !rlc.empty() && !sdma0.empty() && !sdma1.empty();
  }
  bool parse(AmdSmu7Ucode::Images* images) const {
    return amd_gfx8_parse_firmware(ConstByteView(pfp.data(), pfp.size()), AmdGfx8Block::CpPfp, &images->pfp) &&
           amd_gfx8_parse_firmware(ConstByteView(me.data(), me.size()), AmdGfx8Block::CpMe, &images->me) &&
           amd_gfx8_parse_firmware(ConstByteView(ce.data(), ce.size()), AmdGfx8Block::CpCe, &images->ce) &&
           amd_gfx8_parse_firmware(ConstByteView(mec.data(), mec.size()), AmdGfx8Block::CpMec, &images->mec) &&
           amd_gfx8_parse_firmware(ConstByteView(mec2.data(), mec2.size()), AmdGfx8Block::CpMec2, &images->mec2) &&
           amd_gfx8_parse_firmware(ConstByteView(rlc.data(), rlc.size()), AmdGfx8Block::RlcG, &images->rlc) &&
           amd_gfx8_parse_firmware(ConstByteView(sdma0.data(), sdma0.size()), AmdGfx8Block::Sdma0, &images->sdma0) &&
           amd_gfx8_parse_firmware(ConstByteView(sdma1.data(), sdma1.size()), AmdGfx8Block::Sdma1, &images->sdma1);
  }
};
static Files g_files;

static bool inside(ConstByteView view, ConstByteView whole) {
  if (view.empty()) return true;
  return view.data() >= whole.data() && view.data() + view.size() <= whole.data() + whole.size();
}

static void test_names() {
  CHECK(strcmp(amd_gfx8_firmware_name(0x694C, AmdGfx8Block::CpPfp), "vegam_pfp.bin") == 0);
  CHECK(strcmp(amd_gfx8_firmware_name(0x694E, AmdGfx8Block::CpMe), "vegam_me.bin") == 0);
  CHECK(strcmp(amd_gfx8_firmware_name(0x694F, AmdGfx8Block::CpCe), "vegam_ce.bin") == 0);
  CHECK(strcmp(amd_gfx8_firmware_name(0x694C, AmdGfx8Block::CpMec), "vegam_mec.bin") == 0);
  CHECK(strcmp(amd_gfx8_firmware_name(0x694C, AmdGfx8Block::CpMec2), "vegam_mec2.bin") == 0);
  CHECK(strcmp(amd_gfx8_firmware_name(0x694C, AmdGfx8Block::RlcG), "vegam_rlc.bin") == 0);
  CHECK(strcmp(amd_gfx8_firmware_name(0x694C, AmdGfx8Block::Sdma0), "vegam_sdma.bin") == 0);
  CHECK(strcmp(amd_gfx8_firmware_name(0x694C, AmdGfx8Block::Sdma1), "vegam_sdma1.bin") == 0);
  CHECK(amd_gfx8_firmware_name(0x67DF, AmdGfx8Block::CpPfp) == nullptr && amd_gfx8_firmware_name(0, AmdGfx8Block::RlcG) == nullptr);
}

// The fields of the real files, read out of the files with Python's struct and the real kernel header definitions.
static void test_parse_real() {
  AmdSmu7Ucode::Images im;
  CHECK(g_files.parse(&im));
  CHECK(im.pfp.valid && im.pfp.version == 243 && im.pfp.ucode.size() == 16788 && im.pfp.ucode.data() == g_files.pfp.data() + 256 && im.pfp.jump_table.empty());
  CHECK(im.me.version == 164 && im.me.ucode.size() == 16788);
  CHECK(im.ce.version == 137 && im.ce.ucode.size() == 8596);
  CHECK(im.rlc.version == 16 && im.rlc.ucode.size() == 16404 && im.rlc.ucode.data() == g_files.rlc.data() + 256);
  CHECK(im.sdma0.version == 4 && im.sdma0.ucode.size() == 12436 && im.sdma1.version == 4 && im.sdma1.ucode.size() == 12436);
  CHECK(im.mec.version == 718 && im.mec.ucode.size() == 262568);
  CHECK(im.mec.jump_table_offset_bytes == 65541u * 4 && im.mec.jump_table.size() == 101u * 4 && im.mec.jump_table.data() == im.mec.ucode.data() + 65541u * 4);
  CHECK(im.mec2.valid && !im.mec2.jump_table.empty());
  // A file of the wrong kind (the RLC's header version is 2, the others 1), and the RLC file where a CP file is expected, are refused.
  AmdGfx8Image junk;
  CHECK(!amd_gfx8_parse_firmware(ConstByteView(g_files.rlc.data(), g_files.rlc.size()), AmdGfx8Block::CpPfp, &junk) && !junk.valid);
  CHECK(!amd_gfx8_parse_firmware(ConstByteView(g_files.pfp.data(), g_files.pfp.size()), AmdGfx8Block::RlcG, &junk));
  // A PFP file is not a MEC: its jump table is inside the ucode, but the parse as MEC must still work on it (same header kind) - the point is that nothing crashes.
  CHECK(amd_gfx8_parse_firmware(ConstByteView(g_files.pfp.data(), g_files.pfp.size()), AmdGfx8Block::CpMec, &junk) && junk.jump_table.size() == 96u * 4);
}

static void patch32(std::vector<uint8_t>& v, size_t at, uint32_t value) {
  for (int i = 0; i < 4; ++i) v[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

// Header fields that are wrong one at a time (common_firmware_header: size 0, header size 4, ucode size 20, array offset 24).
static void test_parse_fields() {
  AmdGfx8Image image;
  auto parses = [&](const std::vector<uint8_t>& v, AmdGfx8Block block) { return amd_gfx8_parse_firmware(ConstByteView(v.data(), v.size()), block, &image); };
  std::vector<uint8_t> f = g_files.pfp;
  CHECK(parses(f, AmdGfx8Block::CpPfp));
  patch32(f, 20, 16787);   // not whole dwords
  CHECK(!parses(f, AmdGfx8Block::CpPfp));
  f = g_files.pfp;
  patch32(f, 20, 0);
  CHECK(!parses(f, AmdGfx8Block::CpPfp));
  f = g_files.pfp;
  patch32(f, 24, 8);   // the ucode array inside the header
  CHECK(!parses(f, AmdGfx8Block::CpPfp));
  f = g_files.pfp;
  patch32(f, 24, 258);   // not on a dword
  CHECK(!parses(f, AmdGfx8Block::CpPfp));
  f = g_files.pfp;
  patch32(f, 24, 0xFFFFFFFCu);   // the offset plus the size would wrap
  CHECK(!parses(f, AmdGfx8Block::CpPfp));
  f = g_files.pfp;
  patch32(f, 20, 0xFFFFFFFCu);
  CHECK(!parses(f, AmdGfx8Block::CpPfp));
  f = g_files.pfp;
  patch32(f, 4, 40);   // a header too small to hold the jump table fields
  CHECK(!parses(f, AmdGfx8Block::CpPfp));
  f = g_files.mec;
  patch32(f, 32 + 4, 65542);   // jump table past the end of the ucode (65542 + 101 > 65642)
  CHECK(!parses(f, AmdGfx8Block::CpMec));
  f = g_files.mec;
  patch32(f, 32 + 4, 0xFFFFFFFFu);
  CHECK(!parses(f, AmdGfx8Block::CpMec));
  f = g_files.mec;
  patch32(f, 32 + 8, 0);   // no jump table
  CHECK(!parses(f, AmdGfx8Block::CpMec));
  f = g_files.rlc;
  patch32(f, 8, 1);   // the RLC header is version 2
  CHECK(!parses(f, AmdGfx8Block::RlcG));
}

static void test_parse_damage() {
  const std::vector<uint8_t>* all[] = {&g_files.pfp, &g_files.me, &g_files.ce, &g_files.mec, &g_files.mec2, &g_files.rlc, &g_files.sdma0, &g_files.sdma1};
  const AmdGfx8Block blocks[] = {AmdGfx8Block::CpPfp, AmdGfx8Block::CpMe, AmdGfx8Block::CpCe, AmdGfx8Block::CpMec, AmdGfx8Block::CpMec2, AmdGfx8Block::RlcG, AmdGfx8Block::Sdma0, AmdGfx8Block::Sdma1};
  for (int f = 0; f < 8; ++f) {
    const std::vector<uint8_t>& file = *all[f];
    // Truncated: every length up to the header and its surroundings, then every 1 KiB, then one byte short.
    for (size_t length = 0; length < file.size(); length += (length < 300 ? 1 : 1021)) {
      AmdGfx8Image image;
      CHECK(!amd_gfx8_parse_firmware(ConstByteView(file.data(), length), blocks[f], &image));
    }
    AmdGfx8Image image;
    CHECK(!amd_gfx8_parse_firmware(ConstByteView(file.data(), file.size() - 1), blocks[f], &image));
    // Bit flips in the header: whatever the parse says, the views it hands back lie inside the file.
    std::vector<uint8_t> copy = file;
    for (size_t byte = 0; byte < 56; ++byte) {
      for (int bit = 0; bit < 8; ++bit) {
        copy[byte] ^= static_cast<uint8_t>(1 << bit);
        AmdGfx8Image flipped;
        const ConstByteView whole(copy.data(), copy.size());
        if (amd_gfx8_parse_firmware(whole, blocks[f], &flipped)) CHECK(flipped.valid && inside(flipped.ucode, whole) && inside(flipped.jump_table, whole) && !flipped.ucode.empty());
        copy[byte] ^= static_cast<uint8_t>(1 << bit);
      }
    }
  }
}

constexpr uint64_t align4k(uint64_t v) { return (v + 4095) / 4096 * 4096; }

struct Block {
  std::vector<uint8_t> storage;
  uint8_t* base = nullptr;
  explicit Block(size_t bytes) : storage(bytes + 4096) { base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(storage.data()) + 4095) & ~uintptr_t{4095}); }
};

static void test_layout() {
  AmdSmu7Ucode::Images im;
  CHECK(g_files.parse(&im));
  const AmdSmu7Ucode::Layout layout = AmdSmu7Ucode::plan(im);
  CHECK(layout.ok && layout.entries == 9);
  // The order of smu7_request_smu_load_fw: RLC_G, CE, PFP, ME, MEC, JT1, JT2, SDMA0, SDMA1; ids from smu_ucode_xfer_vi.h.
  const uint32_t ids[] = {10, 3, 4, 5, 6, 7, 8, 1, 2};
  for (int i = 0; i < 9; ++i) CHECK(layout.slot[i].id == ids[i]);
  CHECK(layout.toc_offset == 0 && layout.scratch_offset == 4096);
  // The slots follow the table and the scratch area, one page-aligned slot after the other, none overlapping.
  uint64_t expected = 4096 + 200 * 4096;
  for (int i = 0; i < 9; ++i) {
    CHECK(layout.slot[i].offset == expected && layout.slot[i].offset % 4096 == 0);
    expected += align4k(layout.slot[i].image_bytes);
  }
  CHECK(layout.total_bytes == expected);
  CHECK(layout.slot[4].image_bytes == 262568 - 404 && layout.slot[4].table_bytes == 65541u * 4);
  CHECK(layout.slot[5].image_bytes == 404 && layout.slot[6].image_bytes == 404);
  CHECK(layout.slot[0].flags == 1 && layout.slot[4].flags == 1 && layout.slot[1].flags == 0 && layout.slot[2].flags == 0 && layout.slot[5].flags == 0);
  // Without the second MEC file the table has no JT2 entry.
  AmdSmu7Ucode::Images no_mec2 = im;
  no_mec2.mec2 = AmdGfx8Image();
  const AmdSmu7Ucode::Layout shorter = AmdSmu7Ucode::plan(no_mec2);
  CHECK(shorter.ok && shorter.entries == 8 && shorter.slot[6].id == 1);
  // The table says jt_offset * 4 bytes for the MEC, the slot holds the ucode without its jump table: with a gap between the two they differ (here a MEC whose header
  // claims a jump table one dword shorter than what follows the main code).
  std::vector<uint8_t> gap = g_files.mec;
  gap[32 + 8] = 100;   // gfx_firmware_header_v1_0.jt_size: 101 -> 100
  AmdSmu7Ucode::Images gapped = im;
  CHECK(amd_gfx8_parse_firmware(ConstByteView(gap.data(), gap.size()), AmdGfx8Block::CpMec, &gapped.mec));
  const AmdSmu7Ucode::Layout gap_layout = AmdSmu7Ucode::plan(gapped);
  CHECK(gap_layout.ok && gap_layout.slot[4].image_bytes == 262568 - 400 && gap_layout.slot[4].table_bytes == 65541u * 4 && gap_layout.slot[5].image_bytes == 400);
  // A missing image is refused.
  AmdSmu7Ucode::Images missing = im;
  missing.me = AmdGfx8Image();
  CHECK(!AmdSmu7Ucode::plan(missing).ok);
  missing = im;
  missing.mec.jump_table = ConstByteView();
  CHECK(!AmdSmu7Ucode::plan(missing).ok);
}

struct LoadRig {
  fakegfx::Card card;
  fakegfx::FakeClock clock;
  AmdSmu7Ucode::Images images;
  Block block;
  AmdSmu7Ucode::Layout layout;
  uint64_t gpu = 0x180000000ull;
  LoadRig() : block(1600 * 1024) {
    g_files.parse(&images);
    layout = AmdSmu7Ucode::plan(images);
    card.windows.push_back({gpu, block.base, 1600 * 1024});
    card.set_sram(0x20000 + 48, card.soft_registers);   // SMU75_Firmware_Header.SoftRegisters
  }
  AmdSmu7Ucode::Memory memory() { return {gpu, hardware::MmioRegion(reinterpret_cast<uint64_t>(block.base), 1600 * 1024)}; }
};

static void test_load() {
  LoadRig rig;
  CHECK(rig.layout.total_bytes <= 1600 * 1024);
  AmdSmu7Ucode loader(rig.card, rig.clock);
  uint32_t loaded = 0;
  CHECK(loader.load(rig.images, rig.memory(), &loaded) == AmdSmu7Ucode::Result::Ok);
  CHECK(loaded == 0x47E && rig.card.status() == 0x47E);   // RLC_G | SDMA0 | SDMA1 | CE | ME | PFP | MEC
  // The messages, in order, with the addresses of the scratch area and the table split into halves.
  const uint64_t scratch = rig.gpu + 4096, toc = rig.gpu;
  CHECK(rig.card.messages.size() == 5);
  if (rig.card.messages.size() == 5) {
    CHECK(rig.card.messages[0] == std::make_pair(uint16_t{0x252}, static_cast<uint32_t>(scratch >> 32)) && rig.card.messages[1] == std::make_pair(uint16_t{0x253}, static_cast<uint32_t>(scratch)));
    CHECK(rig.card.messages[2] == std::make_pair(uint16_t{0x250}, static_cast<uint32_t>(toc >> 32)) && rig.card.messages[3] == std::make_pair(uint16_t{0x251}, static_cast<uint32_t>(toc)));
    CHECK(rig.card.messages[4] == std::make_pair(uint16_t{0x254}, uint32_t{0x47E}));
  }
  CHECK(rig.card.scratch_address == scratch && rig.card.toc_address == toc);
  // What the CPU wrote through the memory BAR is flushed before the SMC is told anything.
  size_t first_message = rig.card.writes.size(), flush = rig.card.writes.size();
  for (size_t i = 0; i < rig.card.writes.size(); ++i) {
    if (rig.card.writes[i].first == fakegfx::kSmcMessage && first_message == rig.card.writes.size()) first_message = i;
    if (rig.card.writes[i] == std::make_pair(fakegfx::kHdpFlush, uint32_t{1}) && flush == rig.card.writes.size()) flush = i;
  }
  CHECK(flush < first_message && first_message < rig.card.writes.size());
  // The table the SMC read.
  CHECK(rig.card.toc_version == 1 && rig.card.toc_count == 9 && rig.card.toc.size() == 9);
  struct Expect { uint16_t id, version; uint32_t size; uint16_t flags; const std::vector<uint8_t>* file; uint32_t file_offset; };
  const uint32_t mec2_version = le32(g_files.mec2, 16);
  const Expect expect[] = {
      {10, 16, 16404, 1, &g_files.rlc, 256},
      {3, 137, 8596, 0, &g_files.ce, 256},
      {4, 243, 16788, 0, &g_files.pfp, 256},
      {5, 164, 16788, 0, &g_files.me, 256},
      {6, 718, 65541u * 4, 1, &g_files.mec, 256},
      {7, 718, 101u * 4, 0, &g_files.mec, 256 + 65541u * 4},
      {8, static_cast<uint16_t>(mec2_version), 101u * 4, 0, &g_files.mec2, 256 + 65541u * 4},
      {1, 4, 12436, 0, &g_files.sdma0, 256},
      {2, 4, 12436, 0, &g_files.sdma1, 256},
  };
  uint64_t offset = 4096 + 200 * 4096;
  for (size_t i = 0; i < 9 && i < rig.card.toc.size(); ++i) {
    const fakegfx::TocEntry& e = rig.card.toc[i];
    CHECK(e.id == expect[i].id && e.version == expect[i].version && e.size == expect[i].size && e.flags == expect[i].flags && e.registers == 0);
    CHECK((uint64_t{e.address_high} << 32 | e.address_low) == rig.gpu + offset && e.meta_high == 0 && e.meta_low == 0);
    CHECK(e.readable && e.image.size() == e.size && memcmp(e.image.data(), expect[i].file->data() + expect[i].file_offset, e.size) == 0);
    offset += align4k(e.size == 65541u * 4 ? 262568 - 404 : e.size);
  }
  // The unused entries of the table are zero.
  for (uint32_t w = 8 + 9 * 28; w < AmdSmu7Ucode::kTocBytes; w += 4) CHECK(rig.card.get32(rig.gpu + w) == 0);
  // The status word was cleared before the load (the first write to the SMC's memory is a zero), and a slow SMC is waited for.
  LoadRig slow;
  slow.card.status_polls_before_loaded = 7;
  AmdSmu7Ucode slow_loader(slow.card, slow.clock);
  CHECK(slow_loader.load(slow.images, slow.memory()) == AmdSmu7Ucode::Result::Ok);
  // A stale status from an earlier load must not count: the load clears it first, and an SMC that sets nothing times out.
  LoadRig stale;
  stale.card.set_sram(stale.card.soft_status_address(), 0x47E);
  stale.card.loadable_mask = 0;
  AmdSmu7Ucode stale_loader(stale.card, stale.clock);
  uint32_t seen = 0xFFFF;
  CHECK(stale_loader.load(stale.images, stale.memory(), &seen) == AmdSmu7Ucode::Result::TimedOut && seen == 0);
}

static void test_load_failures() {
  {
    LoadRig rig;
    rig.card.smc_running = false;
    AmdSmu7Ucode loader(rig.card, rig.clock);
    CHECK(loader.load(rig.images, rig.memory(), nullptr, 2000) == AmdSmu7Ucode::Result::NotAnswering);
  }
  const uint32_t bad_headers[] = {0, 0xFFFFFFFFu, 0x3E001, 0x3FFFC};
  for (uint32_t header : bad_headers) {
    LoadRig rig;
    rig.card.set_sram(0x20000 + 48, header);
    AmdSmu7Ucode loader(rig.card, rig.clock);
    CHECK(loader.load(rig.images, rig.memory()) == AmdSmu7Ucode::Result::NoSoftRegisters);
    CHECK(rig.card.messages.empty());   // nothing was asked of an SMC that has no firmware to ask
    bool untouched = true;               // and nothing was written to video memory for it
    for (size_t i = 0; i < 4096 + 4096; ++i) untouched = untouched && rig.block.base[i] == 0;
    for (size_t i = 0; i < rig.layout.total_bytes; i += 4096) untouched = untouched && rig.block.base[i] == 0;
    CHECK(untouched);
  }
  {
    LoadRig rig;
    rig.card.answer = 0xFF;   // the SMC fails the load
    AmdSmu7Ucode loader(rig.card, rig.clock);
    CHECK(loader.load(rig.images, rig.memory()) == AmdSmu7Ucode::Result::Refused);
  }
  {
    LoadRig rig;
    rig.card.loadable_mask = 0x47E & ~(1u << 10);   // the RLC does not come up
    AmdSmu7Ucode loader(rig.card, rig.clock);
    uint32_t seen = 0;
    CHECK(loader.load(rig.images, rig.memory(), &seen, 2000) == AmdSmu7Ucode::Result::TimedOut && seen == (0x47E & ~(1u << 10)));
  }
  {
    // Memory that is too small, misaligned, or missing.
    LoadRig rig;
    AmdSmu7Ucode loader(rig.card, rig.clock);
    AmdSmu7Ucode::Memory m = rig.memory();
    AmdSmu7Ucode::Memory small = m;
    small.block = hardware::MmioRegion(m.block.address(), rig.layout.total_bytes - 4096);
    CHECK(loader.load(rig.images, small) == AmdSmu7Ucode::Result::BadArgument);
    AmdSmu7Ucode::Memory skew = m;
    skew.gpu_address += 256;
    CHECK(loader.load(rig.images, skew) == AmdSmu7Ucode::Result::BadArgument);
    AmdSmu7Ucode::Memory host_skew = m;
    host_skew.block = hardware::MmioRegion(m.block.address() + 256, m.block.length() - 4096);
    CHECK(loader.load(rig.images, host_skew) == AmdSmu7Ucode::Result::BadArgument);
    CHECK(loader.load(rig.images, AmdSmu7Ucode::Memory()) == AmdSmu7Ucode::Result::BadArgument);
    AmdSmu7Ucode::Images missing = rig.images;
    missing.sdma1 = AmdGfx8Image();
    CHECK(loader.load(missing, m) == AmdSmu7Ucode::Result::BadArgument);
    CHECK(rig.card.messages.empty());
  }
}

// The engine start
struct GfxRig {
  fakegfx::Card card;
  fakegfx::FakeClock clock;
  Block ring_block, wb_block, csb_block;
  uint64_t ring_gpu = 0x2'0010'0000ull, wb_gpu = 0x2'0020'0000ull, csb_gpu = 0x2'0030'0000ull;
  GfxRig() : ring_block(AmdGfx8::kRingBytes), wb_block(4096), csb_block(8192) {
    card.windows.push_back({ring_gpu, ring_block.base, AmdGfx8::kRingBytes});
    card.windows.push_back({csb_gpu, csb_block.base, 8192});
    card.windows.push_back({wb_gpu, wb_block.base, 4096});
    card.set_sram(card.soft_status_address(), 0x47E);   // the SMC has loaded the microcode
  }
  AmdGfx8::Memory memory() {
    AmdGfx8::Memory m;
    m.ring_gpu_address = ring_gpu;
    m.ring = hardware::MmioRegion(reinterpret_cast<uint64_t>(ring_block.base), AmdGfx8::kRingBytes);
    m.writeback_gpu_address = wb_gpu;
    m.writeback = hardware::MmioRegion(reinterpret_cast<uint64_t>(wb_block.base), 16);
    m.csb_gpu_address = csb_gpu;
    m.csb = hardware::MmioRegion(reinterpret_cast<uint64_t>(csb_block.base), 8192);
    return m;
  }
  uint32_t ring_word(uint32_t i) { return card.get32(ring_gpu + uint64_t{i} * 4); }
};

// golden_settings_vegam_a11 and vegam_golden_common_all, with the register numbers of gfx_8_0_d.h (extracted from the kernel source with a script, not by the code under test).
struct Golden { uint32_t reg, and_mask, or_mask; };
static const Golden kGolden[] = {
    {0x2684, 0x0001f3cf, 0x00007208}, {0x2686, 0x0f000000, 0x0d000000}, {0x2683, 0x000001ff, 0x00000040}, {0x260D, 0xf00fffff, 0x00000400},
    {0x22FC, 0xffffffff, 0x20000001}, {0xC281, 0x0000ff0f, 0x00000000}, {0xA0D4, 0x3f3fffff, 0x3a00161a}, {0xA0D5, 0x0000003f, 0x0000002e},
    {0xEC49, 0x00000003, 0x0001003c}, {0xEC9D, 0xffffffff, 0x0001003c}, {0x2300, 0x07f80000, 0x01180000}, {0x2542, 0x000f000f, 0x000b0000},
    {0x2B80, 0x00100000, 0xf31fff7f}, {0x2B05, 0x000003ff, 0x000000f7}, {0x2B04, 0xffffffff, 0x00000000}, {0x2B03, 0xffffffff, 0x32761054},
    {0x2232, 0x00000004, 0x00000004}, {0xC200, 0xffffffff, 0xe0000000}, {0x263E, 0xffffffff, 0x22011003}, {0x31DC, 0xffffffff, 0x00000800},
    {0x31DD, 0xffffffff, 0x00000800}, {0x31E6, 0xffffffff, 0x00FF7FBF}, {0x31E7, 0xffffffff, 0x00FF7FAF},
};
constexpr uint32_t kNop = 0xFFFF1000;

static uint32_t initial_value(uint32_t reg) { return 0x12345678u ^ (reg * 0x01010101u); }

static void test_start() {
  GfxRig rig;
  rig.card.clock = &rig.clock;
  for (const Golden& g : kGolden) rig.card.set_reg(g.reg, initial_value(g.reg));
  rig.card.set_reg(fakegfx::kRlcCntl, 0x6);
  AmdGfx8 gfx(rig.card, rig.clock);
  CHECK(gfx.start(rig.memory()) == AmdGfx8::Result::Ok);
  // The golden registers: a mask of all ones writes the value, otherwise the owned bits are replaced and the value ORed in (VI family).
  for (const Golden& g : kGolden) {
    const uint32_t expected = g.and_mask == 0xFFFFFFFFu ? g.or_mask : ((initial_value(g.reg) & ~g.and_mask) | g.or_mask);
    if (g.reg != 0xC200 && g.reg != 0xA0D4 && g.reg != 0xA0D5) CHECK(rig.card.reg(g.reg) == expected);   // (the register index ends up on broadcast, below; the raster configuration is set again by the back-end set-up)
  }
  CHECK(rig.card.reg(0xC200) == 0xE0000000);
  // The order of the writes: the golden table first, in order, then the sequence of gfx_v8_0_hw_init.
  const auto& w = rig.card.writes;
  CHECK(w.size() > 23 + 200);
  for (size_t i = 0; i < 23 && i < w.size(); ++i) CHECK(w[i].first == kGolden[i].reg);
  const uint64_t rptr_addr = rig.wb_gpu, poll_addr = rig.wb_gpu + 8, base = rig.ring_gpu >> 8;
  std::vector<std::pair<uint32_t, uint32_t>> sequence;
  auto add = [&](uint32_t reg, uint32_t value) { sequence.push_back({reg, value}); };
  // gfx_v8_0_constants_init: GRBM_CNTL READ_TIMEOUT 0xFF, the address configuration three times, the 32 tile modes and the 15 macrotile modes (not 7), the render back-end
  // set-up (broadcast selections, the unharvested raster configuration), SH_STATIC_MEM_CONFIG, the shader memory set-up of VMIDs 0..7 through SRBM_GFX_CNTL, the same for
  // the compute VMIDs 8..15 followed by their GDS registers, the GDS registers of VMIDs 1..15, the FIFO sizes and the SPI arbitration priority.
  add(0x2000, 0xFF);
  add(0x263E, 0x22011003); add(0xBD2, 0x22011003); add(0x303, 0x22011003);
  for (uint32_t i = 0; i < 32; ++i) add(0x2644 + i, kTileModes[i]);
  for (uint32_t i = 0; i < 16; ++i) if (i != 7) add(0x2664 + i, kMacroModes[i]);
  add(fakegfx::kGrbmGfxIndex, 0x40000000); add(fakegfx::kGrbmGfxIndex, 0x40010000); add(fakegfx::kGrbmGfxIndex, 0xE0000000);   // setup_rb: look at every SE/SH
  add(0xA0D4, 0x3a00161a); add(0xA0D5, 0x0000002e);
  add(fakegfx::kGrbmGfxIndex, 0x40000000); add(fakegfx::kGrbmGfxIndex, 0x40010000); add(fakegfx::kGrbmGfxIndex, 0xE0000000);   // ... and cache the values
  add(0x2581, 0x1b);
  for (uint32_t vmid = 0; vmid < 8; ++vmid) {
    add(0x391, vmid << 4);
    add(0x230d, vmid == 0 ? 0x378 : 0x338);
    add(0x230a, vmid == 0 ? 0 : 0x2000);
    add(0x230b, 1); add(0x230c, 0);
  }
  add(0x391, 0);
  for (uint32_t vmid = 8; vmid < 16; ++vmid) {
    add(0x391, vmid << 4);
    add(0x230d, 0x5e); add(0x230b, 1); add(0x230c, 0); add(0x230a, 0x60006000);
  }
  add(0x391, 0);
  for (uint32_t vmid = 8; vmid < 16; ++vmid) { add(0x3300 + 2 * vmid, 0); add(0x3301 + 2 * vmid, 0); add(0x3320 + vmid, 0); add(0x3330 + vmid, 0); }
  for (uint32_t vmid = 1; vmid < 16; ++vmid) { add(0x3300 + 2 * vmid, 0); add(0x3301 + 2 * vmid, 0); add(0x3320 + vmid, 0); add(0x3330 + vmid, 0); }
  add(fakegfx::kGrbmGfxIndex, 0xE0000000);
  add(0x22f3, 0x98184020);
  add(0x31c0, 0x492);
  const std::pair<uint32_t, uint32_t> rest[] = {
      {fakegfx::kRlcCntl, 0x6 & ~1u},                      // gfx_v8_0_rlc_stop: RLC_ENABLE_F32 off (the other bits stay)
      {fakegfx::kCpIntCntlRing0, 0},                       // the GUI idle interrupt off
      {fakegfx::kGrbmGfxIndex, 0x40000000},                // wait for the RLC serdes: SE 0, SH 0, every instance
      {fakegfx::kGrbmGfxIndex, 0x40010000},                // SE 1
      {fakegfx::kGrbmGfxIndex, 0xE0000000},                // broadcast again
      {fakegfx::kGrbmSoftReset, 0x4},                      // gfx_v8_0_rlc_reset
      {fakegfx::kGrbmSoftReset, 0},
      {fakegfx::kHdpFlush, 1},                             // gfx_v8_0_init_csb: the clear state into the buffer, then where it is
      {0xeca3, static_cast<uint32_t>(rig.csb_gpu >> 32)},
      {0xeca2, static_cast<uint32_t>(rig.csb_gpu)},
      {0xeca4, 912},
      {fakegfx::kRlcCntl, 0x7},                            // gfx_v8_0_rlc_start
      {fakegfx::kCpIntCntlRing0, 0x3C0000},
      {fakegfx::kCpIntCntlRing0, 0},                       // gfx_v8_0_cp_resume
      {0x21c1, 0},                                         // CP_RB_WPTR_DELAY
      {0x3051, 0},                                         // CP_RB_VMID
      {fakegfx::kRb0Cntl, 0x0041880A},                     // RB_BUFSZ 10 (8 KiB in 8-byte units), RB_BLKSZ 8, MTYPE 3, MIN_IB_AVAILSZ 1
      {fakegfx::kRb0Cntl, 0x8041880A},                     // RB_RPTR_WR_ENA
      {fakegfx::kRb0Wptr, 0},
      {fakegfx::kRb0RptrAddr, static_cast<uint32_t>(rptr_addr)},
      {fakegfx::kRb0RptrAddrHi, static_cast<uint32_t>(rptr_addr >> 32) & 0xFF},
      {fakegfx::kWptrPollLo, static_cast<uint32_t>(poll_addr)},
      {fakegfx::kWptrPollHi, static_cast<uint32_t>(poll_addr >> 32)},
      {fakegfx::kRb0Cntl, 0x0041880A},
      {fakegfx::kRb0Base, static_cast<uint32_t>(base)},
      {fakegfx::kRb0BaseHi, static_cast<uint32_t>(base >> 32)},
      {fakegfx::kDoorbellControl, 0},
      {0x30ae, 7},                                         // CP_MAX_CONTEXT = max_hw_contexts - 1
      {0x3050, 0},                                         // CP_ENDIAN_SWAP
      {0x304b, 1},                                         // CP_DEVICE_ID
      {fakegfx::kCpMeCntl, 0},                             // cp_gfx_enable: the halt bits off
      {fakegfx::kHdpFlush, 1},                             // the start stream is committed (916 dwords, padded to 1024)
      {fakegfx::kRb0Wptr, 1024},
      {fakegfx::kScratch0, 0xCAFEDEAD},                    // the ring test
      {fakegfx::kHdpFlush, 1},
      {fakegfx::kRb0Wptr, 1280},
      {fakegfx::kCpIntCntlRing0, 0x3C0000},
  };
  for (const auto& r : rest) sequence.push_back(r);
  size_t at = 23;
  for (const auto& expected : sequence) {
    CHECK(at < w.size() && w[at] == expected);   // (a mismatch prints the position below)
    if (at < w.size() && w[at] != expected) printf("  write %zu: %04x=%08x, expected %04x=%08x\n", at, w[at].first, w[at].second, expected.first, expected.second);
    ++at;
  }
  CHECK(at == w.size());
  // The pauses the driver makes: 50 us around the RLC reset, 50 us after the RLC starts and after the CP is enabled, 1 ms between the ring's read pointer address and the
  // restored control register.
  const auto& t = rig.card.write_times;
  auto index_of = [&](size_t from, uint32_t reg, uint32_t value) { for (size_t i = from; i < w.size(); ++i) if (w[i] == std::make_pair(reg, value)) return i; return w.size(); };
  const size_t reset_on = index_of(23, fakegfx::kGrbmSoftReset, 4), reset_off = index_of(reset_on, fakegfx::kGrbmSoftReset, 0), rlc_on = index_of(reset_off, fakegfx::kRlcCntl, 7);
  CHECK(reset_on + 1 == reset_off && t[reset_off] - t[reset_on] >= 50 && t[rlc_on] - t[reset_off] >= 50 && t[rlc_on + 1] - t[rlc_on] >= 50);
  const size_t poll_hi = index_of(rlc_on, fakegfx::kWptrPollHi, static_cast<uint32_t>(poll_addr >> 32)), restore = index_of(poll_hi, fakegfx::kRb0Cntl, 0x0041880A);
  CHECK(restore == poll_hi + 1 && t[restore] - t[poll_hi] >= 1000);
  const size_t enable = index_of(restore, fakegfx::kCpMeCntl, 0);
  CHECK(t[enable + 1] - t[enable] >= 50);
  // The ring: the clear state and the CE partitions (916 dwords, padded with NOPs to 1024), then the test packet (padded to 1280).
  uint32_t expected_stream[920];
  CHECK(AmdGfx8::build_clear_state(0x3a00161a, 0x2e, ArrayView<uint32_t>(expected_stream, 920), true) == 916);
  for (uint32_t i = 0; i < 916; ++i) CHECK(rig.ring_word(i) == expected_stream[i]);
  for (uint32_t i = 916; i < 1024; ++i) CHECK(rig.ring_word(i) == kNop);
  CHECK(rig.ring_word(1024) == 0xC0017900 && rig.ring_word(1025) == 0x40 && rig.ring_word(1026) == 0xDEADBEEF);
  for (uint32_t i = 1027; i < 1280; ++i) CHECK(rig.ring_word(i) == kNop);
  CHECK(rig.ring_word(1280) == 0);   // the ring was cleared
  // The clear-state buffer of the RLC holds the same without the CE partitions.
  for (uint32_t i = 0; i < 912; ++i) CHECK(rig.card.get32(rig.csb_gpu + uint64_t{i} * 4) == expected_stream[i]);
  CHECK(rig.card.get32(rig.csb_gpu + 912 * 4) == 0);
  // The command processor executed it, and wrote its read pointer back.
  CHECK(rig.card.protocol_errors == 0 && rig.card.reg(fakegfx::kScratch0) == 0xDEADBEEF);
  CHECK(rig.card.executed.size() == 5 + 8 + 1 && rig.card.fillers == (1024 - 916) + (256 - 3));
  const uint32_t opcodes[] = {0x4A, 0x28, 0x69, 0x69, 0x69, 0x69, 0x69, 0x69, 0x69, 0x69, 0x4A, 0x12, 0x11, 0x79};
  for (size_t i = 0; i < 14 && i < rig.card.executed.size(); ++i) CHECK(rig.card.executed[i].opcode == opcodes[i]);
  CHECK(rig.card.get32(rptr_addr) == 1280 && gfx.write_pointer() == 1280);
}

static void test_start_failures() {
  {
    GfxRig rig;   // the microcode is not loaded: the command processor reads nothing
    rig.card.set_sram(rig.card.soft_status_address(), 0);
    AmdGfx8 gfx(rig.card, rig.clock);
    CHECK(gfx.start(rig.memory(), 2000) == AmdGfx8::Result::RingTestFailed);
    CHECK(rig.card.writes.back().first == fakegfx::kRb0Wptr);   // and the GUI idle interrupt was not switched back on
  }
  {
    GfxRig rig;   // only the PFP missing
    rig.card.set_sram(rig.card.soft_status_address(), 0x47E & ~(1u << 4));
    AmdGfx8 gfx(rig.card, rig.clock);
    CHECK(gfx.start(rig.memory(), 2000) == AmdGfx8::Result::RingTestFailed);
  }
  {
    GfxRig rig;
    rig.card.cp_alive = false;
    AmdGfx8 gfx(rig.card, rig.clock);
    CHECK(gfx.start(rig.memory(), 2000) == AmdGfx8::Result::RingTestFailed);
  }
  {
    GfxRig rig;   // busy serdes: waited for, then the start goes on; one that never clears gives up after the timeout and the start goes on
    rig.card.serdes_busy_reads = 3;
    AmdGfx8 gfx(rig.card, rig.clock);
    CHECK(gfx.start(rig.memory()) == AmdGfx8::Result::Ok);
    GfxRig stuck;
    stuck.card.serdes_busy_reads = 1u << 30;
    AmdGfx8 gfx2(stuck.card, stuck.clock);
    CHECK(gfx2.start(stuck.memory(), 2000) == AmdGfx8::Result::Ok);
    CHECK(stuck.card.reg(fakegfx::kGrbmGfxIndex) == 0xE0000000);   // the selection is left on broadcast
  }
  {
    GfxRig rig;
    AmdGfx8 gfx(rig.card, rig.clock);
    AmdGfx8::Memory m = rig.memory();
    AmdGfx8::Memory small_ring = m;
    small_ring.ring = hardware::MmioRegion(m.ring.address(), AmdGfx8::kRingBytes - 4);
    CHECK(gfx.start(small_ring) == AmdGfx8::Result::BadArgument);
    AmdGfx8::Memory small_wb = m;
    small_wb.writeback = hardware::MmioRegion(m.writeback.address(), 8);
    CHECK(gfx.start(small_wb) == AmdGfx8::Result::BadArgument);
    AmdGfx8::Memory skew = m;
    skew.ring_gpu_address += 4;
    CHECK(gfx.start(skew) == AmdGfx8::Result::BadArgument);
    skew = m;
    skew.writeback_gpu_address += 2;
    CHECK(gfx.start(skew) == AmdGfx8::Result::BadArgument);
    CHECK(gfx.start(AmdGfx8::Memory()) == AmdGfx8::Result::BadArgument);
    AmdGfx8::Config zero;
    zero.shader_engines = 0;
    AmdGfx8 bad_config(rig.card, rig.clock, zero);
    CHECK(bad_config.start(m) == AmdGfx8::Result::BadArgument);
    CHECK(rig.card.writes.empty());   // nothing was written for a refused start
  }
}

static void test_high_address() {
  // The read pointer address register holds bits 32..39 of the address, nothing above (the write-back address of a card with more than 1 TiB of address space does not occur).
  GfxRig rig;
  rig.wb_gpu = (uint64_t{0x1A5} << 32) | 0x200000;
  rig.card.windows.push_back({rig.wb_gpu, rig.wb_block.base, 4096});
  AmdGfx8 gfx(rig.card, rig.clock);
  CHECK(gfx.start(rig.memory()) == AmdGfx8::Result::Ok);
  bool seen = false;
  for (const auto& write : rig.card.writes) if (write.first == fakegfx::kRb0RptrAddrHi) { CHECK(write.second == 0xA5); seen = true; }
  CHECK(seen);
}


static void test_clear_state() {
  uint32_t out[920];
  CHECK(AmdGfx8::build_clear_state(0x3a00161a, 0x2e, ArrayView<uint32_t>(out, 911)) == 0);
  CHECK(AmdGfx8::build_clear_state(0x3a00161a, 0x2e, ArrayView<uint32_t>(out, 915), true) == 0);
  CHECK(AmdGfx8::build_clear_state(0x3a00161a, 0x2e, ArrayView<uint32_t>(out, 912)) == 912);
  CHECK(AmdGfx8::kClearStateDwords == 912 && AmdGfx8::kClearStateBytes == 3648);
  // The preamble, then the seven runs of vi_cs_data (first register, count) each as SET_CONTEXT_REG, then the raster configuration, the end of the preamble and CLEAR_STATE.
  CHECK(out[0] == 0xC0004A00 && out[1] == 0x20000000 && out[2] == 0xC0012800 && out[3] == 0x80000000 && out[4] == 0x80000000);
  const uint32_t runs[7][2] = {{0xa000, 212}, {0xa0d6, 274}, {0xa1f5, 6}, {0xa200, 157}, {0xa2a0, 2}, {0xa2a3, 1}, {0xa2a5, 233}};
  uint32_t at = 5;
  for (const auto& run : runs) {
    CHECK(out[at] == (0xC0000000u | 0x6900 | (run[1] << 16)) && out[at + 1] == run[0] - 0xA000);
    at += 2 + run[1];
  }
  CHECK(at == 5 + 14 + 885);
  CHECK(out[at] == 0xC0026900 && out[at + 1] == 0xA0D4 - 0xA000 && out[at + 2] == 0x3a00161a && out[at + 3] == 0x2e);
  CHECK(out[at + 4] == 0xC0004A00 && out[at + 5] == 0x30000000 && out[at + 6] == 0xC0001200 && out[at + 7] == 0 && at + 8 == 912);
  // Values of the defaults that can be read off AMD's table: the 14th register of the first run is PA_SC_SCREEN_SCISSOR_BR (0xa00d) = 0x40004000, the first is DB_RENDER_CONTROL = 0.
  CHECK(out[5 + 2 + 13] == 0x40004000 && out[5 + 2] == 0);
  // With the CE partitions (the ring start stream): SET_BASE, the partition index, two sizes.
  CHECK(AmdGfx8::build_clear_state(1, 2, ArrayView<uint32_t>(out, 920), true) == 916 && out[912] == 0xC0021100 && out[913] == 3 && out[914] == 0x8000 && out[915] == 0x8000);
  CHECK(out[at + 2] == 1 && out[at + 3] == 2);
}

static void test_tables() {
  // The first tile modes by hand: ARRAY_2D_TILED_THIN1 (4) at bit 2, P16_32x32_16x16 (0x11) at bit 6, the tile split (64B..2KB = 0,1,2,3,5) at bit 11, depth micro tiling (2) at bit 22.
  const uint32_t first[] = {0x00800450, 0x00800C50, 0x00801450, 0x00801C50, 0x00802C50};
  for (int i = 0; i < 5; ++i) CHECK(kTileModes[i] == first[i]);
  // The tables are long: all 32 and 15 entries must be programmed, mode 7 of the macrotile table left alone (checked in test_start).
  uint32_t used = 0;
  for (uint32_t v : kTileModes) used += v != 0;
  CHECK(used >= 28);
}

static void test_raster_configs() {
  uint32_t config = 0, config_1 = 0;
  AmdGfx8::default_raster_config(&config, &config_1);
  CHECK(config == 0x3a00161a && config_1 == 0x2e);
  // No back-end harvested: every engine keeps the configuration.
  uint32_t per_se[4] = {0, 0, 0, 0}, c1 = 0;
  AmdGfx8::harvested_raster_configs(config, config_1, 0xF, 4, 2, 1, &c1, ArrayView<uint32_t>(per_se, 4));
  CHECK(c1 == 0x2e && per_se[0] == 0x3a00161a && per_se[1] == 0x3a00161a);
  // Back-end 3 harvested (mask 0b0111): the second engine maps its second back-end to the first (RB_MAP_PKR0 = 0).
  AmdGfx8::harvested_raster_configs(config, config_1, 0x7, 4, 2, 1, &c1, ArrayView<uint32_t>(per_se, 4));
  CHECK(c1 == 0x2e && per_se[0] == 0x3a00161a && per_se[1] == 0x3a001618);
  // The first engine's back-ends both gone (mask 0b1100): SE_MAP 3 and RB_MAP_PKR0 3 for it, and the second engine follows the SE map (the driver's mask arithmetic gives
  // the second engine an empty mask too).
  AmdGfx8::harvested_raster_configs(config, config_1, 0xC, 4, 2, 1, &c1, ArrayView<uint32_t>(per_se, 4));
  CHECK(per_se[0] == 0x3b00161b && per_se[1] == 0x3b00161a);
  // Four engines, the first pair gone: the pair map is 3.
  AmdGfx8::harvested_raster_configs(config, config_1, 0xF0, 8, 4, 1, &c1, ArrayView<uint32_t>(per_se, 4));
  CHECK(c1 == 0x2f);
  // Only the first engine's back-ends left: the second engine's mask is empty, SE_MAP is cleared for both and the second one maps RB_MAP_PKR0 to 3.
  AmdGfx8::harvested_raster_configs(config, config_1, 0x3, 4, 2, 1, &c1, ArrayView<uint32_t>(per_se, 4));
  CHECK(per_se[0] == 0x3800161a && per_se[1] == 0x3800161b);
  // Three back-ends per engine and two shader arrays per engine: one back-end per packer. Back-end 3 harvested (mask 0b110111).
  AmdGfx8::harvested_raster_configs(config, config_1, 0x37, 6, 2, 2, &c1, ArrayView<uint32_t>(per_se, 4));
  CHECK(per_se[0] == 0x3a00161a && per_se[1] == 0x3a00171b);
  // Too small an output array only fills what fits.
  uint32_t one[1] = {0};
  AmdGfx8::harvested_raster_configs(config, config_1, 0xF, 4, 2, 1, &c1, ArrayView<uint32_t>(one, 1));
  CHECK(one[0] == 0x3a00161a);
}

static void test_harvested_start() {
  GfxRig rig;
  rig.card.user_backend_disable[1] = 0x20000;   // GC_USER_RB_BACKEND_DISABLE.BACKEND_DISABLE bit 1 on the second shader engine
  AmdGfx8 gfx(rig.card, rig.clock);
  CHECK(gfx.start(rig.memory()) == AmdGfx8::Result::Ok);
  CHECK(gfx.active_render_backends() == 0x7 && gfx.raster_config() == 0x3a00161a && gfx.raster_config_1() == 0x2e);
  // The writes of the harvested set-up: each engine selected in turn, then the broadcast selection.
  const auto& w = rig.card.writes;
  size_t first = 0;
  for (size_t i = 1; i < w.size(); ++i) if (w[i].first == 0xA0D4 && w[i - 1] == std::make_pair(fakegfx::kGrbmGfxIndex, uint32_t{0x60000000}) && first == 0) first = i;
  // (setup_rb's harvested path selects one engine with every SH and instance broadcast: 0x60000000 | engine << 16)
  CHECK(first > 1 && w[first - 1] == std::make_pair(fakegfx::kGrbmGfxIndex, uint32_t{0x60000000}) && w[first + 1].first == 0xA0D5 && w[first].second == 0x3a00161a);
  CHECK(w[first + 2] == std::make_pair(fakegfx::kGrbmGfxIndex, uint32_t{0x60010000}) && w[first + 3] == std::make_pair(uint32_t{0xA0D4}, uint32_t{0x3a001618}));
}

static void test_backend_discovery() {
  // The disable bits of CC_RB_BACKEND_DISABLE count as well as those of GC_USER_RB_BACKEND_DISABLE: back-end 0 of each engine off (bit 16).
  {
    GfxRig rig;
    rig.card.set_reg(0x263d, 0x10000);
    AmdGfx8 gfx(rig.card, rig.clock);
    CHECK(gfx.start(rig.memory()) == AmdGfx8::Result::Ok && gfx.active_render_backends() == 0xA);
  }
  // Two shader arrays per engine: the bits of each array are placed after the previous one's (width 2). Engine 1 has back-end 0 off.
  {
    GfxRig rig;
    rig.card.user_backend_disable[1] = 0x10000;
    AmdGfx8::Config c;
    c.sh_per_se = 2;
    c.max_backends_per_se = 4;
    AmdGfx8 gfx(rig.card, rig.clock, c);
    CHECK(gfx.start(rig.memory()) == AmdGfx8::Result::Ok && gfx.active_render_backends() == 0xAF);
  }
  // More than 16 back-ends: the count to compare with is capped at 16, so 20 active of 32 is not harvested (one plain broadcast write of the raster configuration).
  {
    GfxRig rig;
    for (auto& d : rig.card.user_backend_disable) d = 0x70 << 16;
    AmdGfx8::Config c;
    c.shader_engines = 4;
    c.max_backends_per_se = 8;
    AmdGfx8 gfx(rig.card, rig.clock, c);
    CHECK(gfx.start(rig.memory()) == AmdGfx8::Result::Ok && gfx.active_render_backends() == 0x8F8F8F8Fu);
    size_t raster_writes = 0;
    for (const auto& w : rig.card.writes) raster_writes += w.first == 0xA0D4;
    CHECK(raster_writes == 2);   // the golden table's, and the plain write
  }
}

static void test_config_refusals() {
  GfxRig rig;
  AmdGfx8::Memory m = rig.memory();
  AmdGfx8 gfx(rig.card, rig.clock);
  AmdGfx8::Memory no_csb = m;
  no_csb.csb = hardware::MmioRegion();
  CHECK(gfx.start(no_csb) == AmdGfx8::Result::BadArgument);
  AmdGfx8::Memory small_csb = m;
  small_csb.csb = hardware::MmioRegion(m.csb.address(), AmdGfx8::kClearStateBytes - 4);
  CHECK(gfx.start(small_csb) == AmdGfx8::Result::BadArgument);
  AmdGfx8::Memory skew_csb = m;
  skew_csb.csb_gpu_address += 2;
  CHECK(gfx.start(skew_csb) == AmdGfx8::Result::BadArgument);
  const uint32_t bad_values[][3] = {{0, 2, 1}, {5, 2, 1}, {2, 3, 2}, {2, 0, 1}, {2, 2, 3}, {2, 33, 1}};   // shader engines, back-ends per engine, SHs per engine
  for (const auto& v : bad_values) {
    AmdGfx8::Config c;
    c.shader_engines = v[0];
    c.max_backends_per_se = v[1];
    c.sh_per_se = v[2];
    AmdGfx8 bad(rig.card, rig.clock, c);
    CHECK(bad.start(m) == AmdGfx8::Result::BadArgument);
  }
  AmdGfx8::Config c;
  c.first_kfd_vmid = 0;
  AmdGfx8 bad_vmid(rig.card, rig.clock, c);
  CHECK(bad_vmid.start(m) == AmdGfx8::Result::BadArgument);
  c.first_kfd_vmid = 17;
  AmdGfx8 bad_vmid2(rig.card, rig.clock, c);
  CHECK(bad_vmid2.start(m) == AmdGfx8::Result::BadArgument);
  CHECK(rig.card.writes.empty());
}

static void test_ring_wrap_and_stop() {
  GfxRig rig;
  AmdGfx8 gfx(rig.card, rig.clock);
  CHECK(gfx.start(rig.memory()) == AmdGfx8::Result::Ok);
  // The ring holds 2048 dwords: 4 more 256-dword tests go round once, 12 more twice.
  for (int i = 0; i < 12; ++i) {
    CHECK(gfx.test_ring());
    CHECK(gfx.write_pointer() == ((1280u + 256u * (i + 1)) & 2047));
    CHECK(rig.card.get32(rig.wb_gpu) == gfx.write_pointer());
  }
  CHECK(rig.card.protocol_errors == 0);
  // Stopping halts the three graphics processors, both compute pipes and the RLC.
  gfx.stop();
  CHECK((rig.card.reg(fakegfx::kCpMeCntl) & (fakegfx::kMeHalt | fakegfx::kPfpHalt | fakegfx::kCeHalt)) == (fakegfx::kMeHalt | fakegfx::kPfpHalt | fakegfx::kCeHalt));
  CHECK(rig.card.reg(fakegfx::kCpMecCntl) == (fakegfx::kMecMe1Halt | fakegfx::kMecMe2Halt));
  CHECK((rig.card.reg(fakegfx::kRlcCntl) & 1) == 0);
  // A stopped engine does not run the ring.
  CHECK(!gfx.test_ring(1000));
}

// From a card with nothing loaded to a running ring: the SMC loads the microcode, then the engine starts.
static void test_end_to_end() {
  fakegfx::Card card;
  fakegfx::FakeClock clock;
  AmdSmu7Ucode::Images images;
  CHECK(g_files.parse(&images));
  Block block(1600 * 1024), ring(AmdGfx8::kRingBytes), wb(4096), csb(8192);
  card.windows.push_back({0x180000000ull, block.base, 1600 * 1024});
  card.windows.push_back({0x200100000ull, ring.base, AmdGfx8::kRingBytes});
  card.windows.push_back({0x200200000ull, wb.base, 4096});
  card.windows.push_back({0x200300000ull, csb.base, 8192});
  card.set_sram(0x20000 + 48, card.soft_registers);
  // Without the load the ring test fails; after it, it passes.
  AmdGfx8::Memory m;
  m.ring_gpu_address = 0x200100000ull;
  m.ring = hardware::MmioRegion(reinterpret_cast<uint64_t>(ring.base), AmdGfx8::kRingBytes);
  m.writeback_gpu_address = 0x200200000ull;
  m.writeback = hardware::MmioRegion(reinterpret_cast<uint64_t>(wb.base), 16);
  m.csb_gpu_address = 0x200300000ull;
  m.csb = hardware::MmioRegion(reinterpret_cast<uint64_t>(csb.base), 8192);
  AmdGfx8 gfx(card, clock);
  CHECK(gfx.start(m, 2000) == AmdGfx8::Result::RingTestFailed);
  AmdSmu7Ucode loader(card, clock);
  CHECK(loader.load(images, {0x180000000ull, hardware::MmioRegion(reinterpret_cast<uint64_t>(block.base), 1600 * 1024)}) == AmdSmu7Ucode::Result::Ok);
  AmdGfx8 again(card, clock);
  CHECK(again.start(m) == AmdGfx8::Result::Ok);
}

int main() {
  if (!g_files.load()) {
    printf("the firmware files are missing (run from the repository root)\n");
    return 1;
  }
  test_names();
  test_parse_real();
  test_parse_fields();
  test_parse_damage();
  test_layout();
  test_load();
  test_load_failures();
  test_start();
  test_start_failures();
  test_high_address();
  test_clear_state();
  test_tables();
  test_raster_configs();
  test_harvested_start();
  test_backend_discovery();
  test_config_refusals();
  test_ring_wrap_and_stop();
  test_end_to_end();
  printf(g_failed ? "AmdGfx8Test: %d FAILED\n" : "AmdGfx8Test: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
