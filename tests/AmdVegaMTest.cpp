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


// Host-side tests for the Vega M bring-up (AmdVegaM): all the pieces of the card at once, each against its own simulated hardware behind a router that gives every register to
// the model that owns it, all sharing one simulated video memory. A bring-up must give a memory controller with its GART on, a running interrupt ring, the microcode loaded by
// the SMC, a graphics engine with eight compute queues, working SDMA engines, and a framebuffer the display can be switched to; every step that can fail must stop the earlier
// ones. The pieces themselves are covered by their own tests; this checks the glue: the layout of the block, the firmware files, the addresses given to each piece, the order.
#include <stdio.h>
#include <functional>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdVegaM.hpp"
#include "FakeAmdDce11.hpp"
#include "FakeAmdGfx8.hpp"
#include "FakeAmdGmc8.hpp"
#include "FakeAmdIh3.hpp"
#include "FakeAmdSdma3.hpp"
#include "AtomBiosImage.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    const uint64_t al = alignment < 16 ? 16 : alignment;
    return aligned_alloc(al, (size + al - 1) / al * al);
  }
  void deallocate(void* p) override { if (p) free(p); }
};

static std::vector<uint8_t> read_host_file(const char* path) {
  std::vector<uint8_t> bytes;
  FILE* f = fopen(path, "rb");
  if (!f) return bytes;
  uint8_t chunk[4096];
  size_t n;
  while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
  fclose(f);
  return bytes;
}

static const char* const kFiles[] = {"vegam_pfp.bin", "vegam_me.bin", "vegam_ce.bin", "vegam_mec.bin", "vegam_mec2.bin", "vegam_rlc.bin", "vegam_sdma.bin", "vegam_sdma1.bin"};

static bool put_file(filesystem::RamFileSystem& fs, const char* name) {
  const std::string host = std::string("firmware/") + name, path = std::string("/firmware/") + name;
  const std::vector<uint8_t> image = read_host_file(host.c_str());
  UniquePointer<filesystem::IFile> file;
  if (image.empty() || fs.open_file(StringView(path.c_str()), filesystem::OpenMode::CreateTruncate, &file) != filesystem::Status::Ok) return false;
  uint64_t put = 0;
  return file->write(0, ConstByteView(image.data(), image.size()), &put) == filesystem::Status::Ok && put == image.size();
}

// A mapper that hands out the simulated video memory: the memory BAR starts at kAperture.
constexpr uint64_t kVideoMemory = 64ull << 20, kGpuBase = 64ull << 20;   // the memory controller puts video memory at 64 MiB of the GPU's address space   // (the memory BAR is at the simulated memory's own address: the OS runs identity mapped, and the firmware's framebuffer is reached through it)
struct Mapper final : IRegisterMapper {
  uint8_t* host = nullptr;
  bool refuse = false;
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    const uint64_t base = reinterpret_cast<uint64_t>(host);
    if (refuse || physical < base || physical - base + length > kVideoMemory) return false;
    *region = MmioRegion(physical, length);
    return true;
  }
};

// The whole card
struct Card final : IVcnRegisters {
  fakegfx::Card gfx;
  fakegmc::Card gmc;
  fakeih::Card ih;
  fakesdma::Card sdma;
  fakedce::FakeDisplay dce;
  size_t other_writes = 0;
  std::function<void()> on_marker;   // called when the POST writes register 0x7777 (the simulated card then has its memory trained)
  uint32_t memsize_ones = 0;        // CONFIG_MEMSIZE reads all ones this many more times (the card is in reset)
  bool memsize_never = false;
  static bool in(uint32_t r, uint32_t lo, uint32_t hi) { return r >= lo && r <= hi; }
  IVcnRegisters& owner(uint32_t r) {
    if (in(r, 0x3400, 0x37ff)) return sdma;
    if (in(r, 0xe30, 0xe4b) || r == 0x151a || r == 0x151b) return ih;
    if (in(r, 0x500, 0x57f) || in(r, 0x800, 0x82f) || in(r, 0xb00, 0xbff) || r == 0x394 || r == 0x1524 || r == 0x1528 || r == 0x150a) return gmc;
    if (in(r, 0x1a00, 0x1fff) || in(r, 0x4000, 0x47ff)) return dce;
    return gfx;
  }
  uint32_t read(uint32_t r) override {
    if (r == 0x150a && (memsize_never || memsize_ones > 0)) {
      if (memsize_ones > 0) --memsize_ones;
      return 0xFFFFFFFFu;
    }
    return owner(r).read(r);
  }
  void write(uint32_t r, uint32_t v) override {
    owner(r).write(r, v);
    if (r == 0x7777 && on_marker) on_marker();
    if (r == 0x398) sdma.write(r, v);   // SRBM_SOFT_RESET reaches both the graphics and the DMA engines
  }
  size_t writes() const { return gfx.writes.size() + gmc.writes.size() + ih.writes.size() + sdma.writes.size() + dce.writes.size(); }
};

struct Clock final : IClock {
  mutable uint64_t now = 0;
  Card* card = nullptr;
  uint64_t now_us() const override {
    now += 50;
    if (card) {
      card->sdma.poll();
      card->gfx.poll_mec();
    }
    return now;
  }
  const char* name() const override { return "clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

struct Rig {
  TestAllocator alloc;
  Card card;
  Clock clock;
  Mapper mapper;
  std::vector<uint8_t> storage, doorbells;
  uint8_t* vram = nullptr;
  std::unique_ptr<filesystem::RamFileSystem> fs;
  explicit Rig(bool with_files = true) : storage(kVideoMemory + 4096), doorbells(0x800 * 4 + 4096) {
    vram = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(storage.data()) + 4095) & ~uintptr_t{4095});
    mapper.host = vram;
    clock.card = &card;
    uint8_t* db = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(doorbells.data()) + 4095) & ~uintptr_t{4095});
    doorbell_base = db;
    // the memory controller as the firmware left it: 64 MiB at GPU address 0
    card.gmc.set_reg(0x150a, 64);
    card.gmc.set_reg(0x809, (7u << 16) | 4u);   // MC_VM_FB_LOCATION: base 4 * 16 MiB, top 7
    const fakegmc::Window w{kGpuBase, vram, kVideoMemory};
    card.gmc.windows.push_back(w);
    card.gfx.windows.push_back({kGpuBase, vram, kVideoMemory});
    card.sdma.windows.push_back({kGpuBase, vram, kVideoMemory});
    card.ih.windows.push_back({kGpuBase, vram, kVideoMemory});
    card.gfx.doorbell_host = db;
    card.gfx.doorbell_bytes = 0x800 * 4;
    card.sdma.doorbell_host = db;
    card.sdma.doorbell_bytes = 0x800 * 4;
    card.ih.doorbell_host = db;
    card.ih.doorbell_bytes = 0x800 * 4;
    card.gfx.set_smc_header();
    fs.reset(new filesystem::RamFileSystem(alloc));
    fs->create_directory("/firmware");
    if (with_files) for (const char* name : kFiles) put_file(*fs, name);
  }
  uint8_t* doorbell_base = nullptr;
  AmdVegaM::Inputs inputs() {
    AmdVegaM::Inputs in;
    in.registers = &card;
    in.clock = &clock;
    in.file_system = fs.get();
    in.allocator = &alloc;
    in.mapper = &mapper;
    in.device_id = 0x694C;
    in.aperture_physical = reinterpret_cast<uint64_t>(vram);
    in.aperture_bytes = 256ull << 20;
    in.doorbells = MmioRegion(reinterpret_cast<uint64_t>(doorbell_base), 0x800 * 4);
    in.max_framebuffer_bytes = 4ull << 20;
    return in;
  }
};

constexpr uint64_t kRegion = AmdVegaM::kRegionOffset;

static void test_plan() {
  const AmdVegaM::Layout l = AmdVegaM::plan(0x140000, 512 * 1024, 8, 4ull << 20);
  CHECK(l.ok && l.dummy_page == 0 && l.scratch_page == 4096 && l.gart_table == 8192 && l.smu == 8192 + 512 * 1024);
  CHECK(l.gfx == l.smu + 0x140000 && l.compute == l.gfx + 16384 && l.sdma == l.compute + 9 * 20480 && l.ih == l.sdma + 24576 && l.ib == l.ih + 69632 && l.framebuffer % (1 << 20) == 0 && l.framebuffer >= l.ib + 8192);
  CHECK(l.total == l.framebuffer + (4ull << 20) && l.total < AmdVegaM::kRegionLimit - AmdVegaM::kRegionOffset);
  CHECK(!AmdVegaM::plan(0, 512 * 1024, 8, 0).ok && !AmdVegaM::plan(0x140000, 0, 8, 0).ok && !AmdVegaM::plan(0x140000, 512 * 1024, 0, 0).ok && !AmdVegaM::plan(0x140000, 512 * 1024, 9, 0).ok);
  CHECK(!AmdVegaM::plan(0x140000, 512 * 1024, 8, 80ull << 20).ok && !AmdVegaM::plan(~0ull, 512 * 1024, 8, 0).ok);
  // The block ends where the video engine's memory may start (half of the visible part).
  const uint64_t room = AmdVegaM::kRegionLimit - AmdVegaM::kRegionOffset - AmdVegaM::plan(0x140000, 512 * 1024, 8, 0).framebuffer;   // what is left for the framebuffer
  CHECK(AmdVegaM::plan(0x140000, 512 * 1024, 8, room).ok && !AmdVegaM::plan(0x140000, 512 * 1024, 8, room + 1).ok && room % (1 << 20) == 0 && room > (60ull << 20));
}

static void test_bring_up() {
  Rig rig;
  rig.card.gfx.set_reg(0x2315, 1);   // SQ_INTERRUPT_MSG_CTRL.STALL: the messages are held back until the driver lets them through
  AmdVegaM vega(rig.alloc);
  const bool up = vega.bring_up(rig.inputs());
  if (!up) printf("  %s (step %s)\n", vega.status(), AmdVegaM::step_name(vega.step()));
  CHECK(up && vega.up() && vega.step() == AmdVegaM::Step::Done);
  if (!up) return;
  CHECK(strstr(vega.status(), "GART 256 MiB") && strstr(vega.status(), "8 compute queues"));
  const AmdVegaM::Layout& l = vega.layout();
  const uint64_t block_gpu = kGpuBase + kRegion;
  // The memory controller: the GART at the top of the address space, its table in the block, the dummy and scratch pages in the block.
  CHECK(vega.gart_plan().ok && vega.gart_plan().gart_start == 0xFF00000000ull);
  CHECK(rig.card.gmc.reg(0x54F) == (block_gpu + l.gart_table) >> 12 && rig.card.gmc.reg(0x546) == (block_gpu + l.dummy_page) >> 12 && rig.card.gmc.reg(0x80f) == (block_gpu + l.scratch_page) >> 12);
  CHECK(rig.card.gmc.reg(0x504) == (0x11 | 0x249248) && rig.card.gmc.reg(0x80d) == kGpuBase >> 12 && rig.card.gmc.reg(0x80e) == ((kGpuBase + kVideoMemory) >> 12) - 1);
  // The interrupt ring is in the block and is not snooped; a vector posted by the card is read back.
  CHECK(vega.interrupts() && vega.interrupts()->running() && rig.card.ih.reg(0xe31) == (block_gpu + l.ih) >> 8 && (rig.card.ih.reg(0x151a) & 0x8));
  rig.card.ih.post(181, 0x1234, 7, 0, 0);
  AmdIh3::Vector v[2];
  CHECK(vega.interrupts()->drain(ArrayView<AmdIh3::Vector>(v, 2)) == 1 && v[0].src_id == 181 && v[0].src_data == 0x1234 && v[0].ring_id == 7);
  // The microcode: the SMC was told to load the images, which sit in the block.
  CHECK(rig.card.gfx.status() == 0x47E && rig.card.gfx.toc.size() == 9 && rig.card.gfx.toc_address == block_gpu + l.smu && rig.card.gfx.scratch_address == block_gpu + l.smu + 4096);
  // The graphics engine, its ring and clear state in the block, and the eight compute queues.
  CHECK(vega.graphics() && rig.card.gfx.protocol_errors == 0 && rig.card.gfx.compute_protocol_errors == 0 && rig.card.gfx.mapped.size() == 8);
  CHECK(rig.card.gfx.reg(0x3040) == (block_gpu + l.gfx) >> 8 && rig.card.gfx.reg(0xeca2) == static_cast<uint32_t>(block_gpu + l.gfx + 8192 + 4096));
  CHECK(rig.card.gfx.mapped[0].mqd_address == block_gpu + l.compute + AmdGfx8Compute::kSlotBytes);
  // The SDMA engines work in the block: copy a pattern inside the framebuffer area through both.
  const uint64_t fb = block_gpu + l.framebuffer;
  uint8_t* fbhost = rig.vram + kRegion + l.framebuffer;
  for (int i = 0; i < 256; ++i) fbhost[i] = static_cast<uint8_t>(i * 3 + 1);
  CHECK(vega.sdma() && vega.sdma()->running() && vega.sdma()->copy(0, fb, fb + 1024, 256) && memcmp(fbhost + 1024, fbhost, 256) == 0);
  CHECK(vega.sdma()->fill(1, fb + 2048, 0xA5A5A5A5, 64) && fbhost[2048] == 0xA5 && fbhost[2111] == 0xA5 && fbhost[2112] == 0);
  CHECK(rig.card.sdma.protocol_errors == 0 && rig.card.sdma.copies == 1 && rig.card.sdma.fills == 1);
  // The interrupt sources are on, so the blocks post to the interrupt ring: the graphics ring's three enables, the compute pipe's end of pipe (pipe 0 of MEC 1), the SDMA traps,
  // and the memory controller's seven fault enables in both contexts. Each is the last write to its register, after the engine came up.
  constexpr uint32_t kCpIntBits = 0x04000000 | 0x00800000 | 0x00400000;
  CHECK((rig.card.gfx.reg(0x306a) & kCpIntBits) == kCpIntBits && (rig.card.gfx.reg(0x3085) & 0x04000000) && !(rig.card.gfx.reg(0x3086) & 0x04000000));
  CHECK((rig.card.gfx.reg(0x3049) & 0x4000) && (rig.card.gfx.reg(0x308c) & 0x4000) && (rig.card.gfx.reg(0x2315) & 1) == 0);   // ECC errors on, SQ messages let through
  CHECK((rig.card.sdma.reg(0x3404) & 1) && (rig.card.sdma.reg(0x3604) & 1) && rig.card.sdma.writes.back().first == 0x3604 && rig.card.sdma.writes.back().second == rig.card.sdma.reg(0x3604));
  CHECK((rig.card.gmc.reg(0x504) & 0x249248) == 0x249248 && (rig.card.gmc.reg(0x505) & 0x249248) == 0x249248);
  // The burst NOP follows the firmware's feature version (31 for the first engine's file, 0 for the second's): the padding after the ring test packet.
  const uint8_t* sdma_ring0 = rig.vram + kRegion + l.sdma;
  const uint8_t* sdma_ring1 = sdma_ring0 + AmdSdma3::kSlotBytes;
  CHECK(sdma_ring0[5 * 4 + 2] == 0x0A && sdma_ring1[5 * 4 + 2] == 0x00);
  // Command buffers through the three kinds of engine, on the one staging area: a register write on the graphics ring and on a compute queue, a copy on each DMA engine.
  {
    const uint64_t data = block_gpu + l.framebuffer + 8192;
    uint8_t* data_host = rig.vram + kRegion + l.framebuffer + 8192;
    const uint32_t gfx_ib[] = {0xC0017900, 0x40, 0x0A0A, 0xC0043700, 0x500, static_cast<uint32_t>(data), static_cast<uint32_t>(data >> 32), 0x11110000};
    CHECK(vega.submit_graphics(ArrayView<const uint32_t>(gfx_ib, 8)) && rig.card.gfx.reg(0xc040) == 0x0A0A && memcmp(data_host, "\0\0\x11\x11", 4) == 0 && rig.card.gfx.ib_runs.size() == 1);
    const uint32_t compute_ib[] = {0xC0017900, 0x40, 0x0B0B, 0xC0043700, 0x500, static_cast<uint32_t>(data + 4), static_cast<uint32_t>((data + 4) >> 32), 0x22220000};
    CHECK(vega.submit_compute(6, ArrayView<const uint32_t>(compute_ib, 8)) && rig.card.gfx.reg(0xc040) == 0x0B0B && memcmp(data_host + 4, "\0\0\x22\x22", 4) == 0 && rig.card.gfx.ib_runs.size() == 2 &&
          rig.card.gfx.ib_runs[1].compute);
    for (int i = 0; i < 16; ++i) data_host[64 + i] = static_cast<uint8_t>(0x90 + i);
    for (uint32_t engine : {0u, 1u}) {
      const uint32_t sdma_ib[] = {1, 16, 0, static_cast<uint32_t>(data + 64), static_cast<uint32_t>((data + 64) >> 32), static_cast<uint32_t>(data + 256 + engine * 64), static_cast<uint32_t>((data + 256 + engine * 64) >> 32)};
      CHECK(vega.submit_sdma(engine, ArrayView<const uint32_t>(sdma_ib, 7)) && memcmp(data_host + 256 + engine * 64, data_host + 64, 16) == 0);
    }
    CHECK(rig.card.sdma.ib_runs.size() == 2 && rig.card.sdma.ib_errors == 0 && rig.card.gfx.ib_protocol_errors == 0);
    // No commands, too many, a queue or an engine that does not exist.
    uint32_t big[AmdVegaM::kIbDwords + 1] = {};
    CHECK(!vega.submit_graphics(ArrayView<const uint32_t>(gfx_ib, 0)) && !vega.submit_graphics(ArrayView<const uint32_t>(big, AmdVegaM::kIbDwords + 1)) &&
          !vega.submit_compute(8, ArrayView<const uint32_t>(compute_ib, 8)) &&
          !vega.submit_sdma(2, ArrayView<const uint32_t>(gfx_ib, 8)) && !vega.submit_sdma(0, ArrayView<const uint32_t>(big, 0)));
  }
  // Shutting down stops it all in reverse: SDMA halted, compute queues unmapped and the engines halted, the interrupt ring off, the GART off.
  vega.shutdown();
  CHECK(!vega.up() && rig.card.sdma.reg(0x3412) == 1 && rig.card.sdma.reg(0x3612) == 1);
  CHECK(!(rig.card.gfx.reg(0x3049) & 0x4000) && !(rig.card.gfx.reg(0x308c) & 0x4000) && (rig.card.gfx.reg(0x2315) & 1));
  CHECK(!(rig.card.gfx.reg(0x306a) & kCpIntBits) && !(rig.card.gfx.reg(0x3085) & 0x04000000) && !(rig.card.sdma.reg(0x3404) & 1) && !(rig.card.sdma.reg(0x3604) & 1));
  CHECK(rig.card.gfx.unmapped_doorbells.size() == 8 && (rig.card.gfx.reg(fakegfx::kCpMeCntl) & fakegfx::kMeHalt));
  CHECK(!(rig.card.ih.reg(0xe30) & 1) && rig.card.gmc.reg(0x504) == 0 && rig.card.gmc.reg(0x505) == 0);
  // And it can be brought up again.
  memset(rig.doorbell_base, 0, 0x800 * 4);   // (the doorbell BAR reads back what was last written; the simulated microcode watches for changes, so a new life starts clean)
  const bool again = vega.bring_up(rig.inputs());
  if (!again) printf("  second bring-up: %s\n", vega.status());
  CHECK(again && vega.up());
}

// System memory for the transfers: one pool, "physical" addresses from 1 GiB (the host's own addresses are beyond the card's 40 bits).
struct Pool final : ISystemAddresses {
  static constexpr uint64_t kBase = 1ull << 30;
  std::vector<uint8_t> memory;
  explicit Pool(size_t bytes) : memory(bytes) {}
  uint64_t physical_address(const void* p) const override { return kBase + (static_cast<const uint8_t*>(p) - memory.data()); }
};

static void test_transfer() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  Pool pool(10u << 20);
  AmdVegaM::Inputs in = rig.inputs();
  in.system_addresses = &pool;
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  const AmdVegaM::Layout& l = vega.layout();
  uint8_t* area = rig.vram + kRegion + l.framebuffer;
  const uint8_t* table = rig.vram + kRegion + l.gart_table;
  rig.card.sdma.gart_start = vega.gart_plan().gart_start;
  rig.card.sdma.gart_size = vega.gart_plan().gart_size;
  rig.card.sdma.gart_table = table;
  rig.card.sdma.system_host = pool.memory.data();
  rig.card.sdma.system_base = Pool::kBase;
  rig.card.sdma.system_bytes = pool.memory.size();
  auto table_empty = [&]() {
    for (uint32_t i = 0; i < AmdVegaM::kTransferPages * 8; ++i) if (table[i] != 0) return false;
    return true;
  };
  auto pattern = [](const auto& v, uint32_t seed) { for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint8_t>((i * 7 + seed + (i >> 8)) & 0xFF); };
  // A small buffer that starts in the middle of a page, to an offset that does not: one copy, through the GART, with the flags of a snooped system page.
  // (carved from the pool, one after the other)
  size_t used = 0;
  auto carve = [&](size_t bytes) { uint8_t* p = pool.memory.data() + used; used += (bytes + 4095) / 4096 * 4096 + 4096; return p; };
  struct Buf { uint8_t* p; size_t n; uint8_t* data() const { return p; } size_t size() const { return n; } uint8_t& operator[](size_t i) const { return p[i]; } };
  Buf small{carve(300), 300};
  pattern(small, 5);
  CHECK(vega.upload(ConstByteView(small.data(), small.size()), 4097 + 5));
  CHECK(memcmp(area + 4097 + 5, small.data(), 300) == 0 && area[4097 + 4] == 0 && area[4097 + 5 + 300] == 0);
  CHECK(rig.card.sdma.copies == 1 && rig.card.sdma.gart_faults == 0 && rig.card.sdma.protocol_errors == 0 && table_empty());
  CHECK((rig.card.sdma.pte_and & 0x77) == 0x77);   // valid, system, snooped, readable, writeable, executable
  // The same back, into a buffer that is not page aligned.
  Buf back{carve(364), 364};
  memset(back.p, 0xEE, back.n);
  CHECK(vega.download(ByteView(back.data() + 3, 300), 4097 + 5));
  CHECK(memcmp(back.data() + 3, small.data(), 300) == 0 && back[2] == 0xEE && back[303] == 0xEE && table_empty());
  // More than a window (2 MiB) at once, on the second engine: one copy a window, the data intact, the end of the area reached exactly.
  const uint64_t big_bytes = 3ull * 1024 * 1024 + 37;
  Buf big{carve(big_bytes + 4096), big_bytes + 4096};
  pattern(big, 11);
  const uint64_t offset = l.framebuffer_bytes - big_bytes;
  const uint32_t copies_before = rig.card.sdma.copies, engine1_before = rig.card.sdma.executed[1];
  CHECK(vega.upload(ConstByteView(big.data() + 13, big_bytes), offset, 1));
  CHECK(memcmp(area + offset, big.data() + 13, big_bytes) == 0 && rig.card.sdma.copies == copies_before + 2 && table_empty());
  CHECK(rig.card.sdma.copy_sizes[copies_before] == 2u * 1024 * 1024 - 13 && rig.card.sdma.copy_sizes[copies_before + 1] == big_bytes - (2u * 1024 * 1024 - 13));
  CHECK(rig.card.sdma.executed[1] > engine1_before);
  Buf big_back{carve(big_bytes + 8), big_bytes + 8};
  memset(big_back.p, 0, big_back.n);
  CHECK(vega.download(ByteView(big_back.data() + 8, big_bytes), offset, 1) && memcmp(big_back.data() + 8, big.data() + 13, big_bytes) == 0 && table_empty());
  CHECK(rig.card.sdma.gart_faults == 0 && rig.card.sdma.protocol_errors == 0);
  // The video memory outside the range is not touched, and what is refused changes nothing.
  const uint32_t copies_now = rig.card.sdma.copies;
  CHECK(!vega.upload(ConstByteView(small.data(), 0), 0) && !vega.upload(ConstByteView(small.data(), 300), l.framebuffer_bytes - 299) && !vega.upload(ConstByteView(small.data(), 300), l.framebuffer_bytes + 1) &&
        !vega.upload(ConstByteView(small.data(), 300), ~0ull - 100) && !vega.download(ByteView(back.data(), 300), l.framebuffer_bytes - 299) && !vega.upload(ConstByteView(small.data(), 300), 0, 2));
  CHECK(rig.card.sdma.copies == copies_now);
  // A system address beyond the card's 40 bits.
  CHECK(!vega.upload(ConstByteView(pool.memory.data() + (1ull << 40), 16), 0) && table_empty());
  // An engine that does not answer: false, and the window stays mapped (the engine may still be reading the pages).
  rig.card.sdma.stall[0] = true;
  CHECK(!vega.upload(ConstByteView(small.data(), 300), 0, 0, 1000) && !table_empty() && vega.sdma()->wedged(0));
  CHECK(!vega.upload(ConstByteView(back.data(), 300), 0, 0) && !table_empty());   // (a wedged engine is not given more, and the pages it may still use stay as they were)
  CHECK(table[1] == ((Pool::kBase >> 8) & 0xFF) && table[2] == ((Pool::kBase >> 16) & 0xFF) && table[3] == (Pool::kBase >> 24) && (table[0] & 1));
  rig.card.sdma.stall[0] = false;
  // Not up: nothing.
  vega.shutdown();
  CHECK(!vega.upload(ConstByteView(small.data(), 300), 0) && !vega.download(ByteView(back.data(), 16), 0));
}

static void test_failures() {
  {
    Rig rig(false);   // no firmware files
    AmdVegaM vega(rig.alloc);
    CHECK(!vega.bring_up(rig.inputs()) && strstr(vega.status(), "firmware: vegam_pfp.bin not found") && vega.step() == AmdVegaM::Step::MemoryMap);
    CHECK(rig.card.writes() == 0);   // the card was only read
  }
  {
    Rig rig;   // one file missing
    UniquePointer<filesystem::IFile> none;
    rig.fs->remove("/firmware/vegam_mec2.bin");
    AmdVegaM vega(rig.alloc);
    CHECK(!vega.bring_up(rig.inputs()) && strstr(vega.status(), "vegam_mec2.bin not found") && rig.card.writes() == 0);
  }
  {
    Rig rig;   // a file that is not a microcode file
    UniquePointer<filesystem::IFile> file;
    CHECK(rig.fs->open_file("/firmware/vegam_ce.bin", filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok);
    uint8_t junk[64] = {1, 2, 3};
    uint64_t put = 0;
    file->write(0, ConstByteView(junk, 64), &put);
    AmdVegaM vega(rig.alloc);
    CHECK(!vega.bring_up(rig.inputs()) && strstr(vega.status(), "not a microcode file") && rig.card.writes() == 0);
  }
  {
    Rig rig;   // the memory controller does not answer
    rig.card.gmc.set_reg(0x150a, 0);
    AmdVegaM vega(rig.alloc);
    CHECK(!vega.bring_up(rig.inputs()) && strstr(vega.status(), "memory map") && rig.card.writes() == 0);
  }
  {
    Rig rig;   // not a Vega M, or no access to the card
    AmdVegaM vega(rig.alloc);
    AmdVegaM::Inputs in = rig.inputs();
    in.device_id = 0x67DF;
    CHECK(!vega.bring_up(in) && vega.step() == AmdVegaM::Step::None);
    in = rig.inputs();
    in.registers = nullptr;
    CHECK(!vega.bring_up(in));
    in = rig.inputs();
    in.doorbells = MmioRegion();
    CHECK(!vega.bring_up(in));
    in = rig.inputs();
    in.aperture_physical = 0;
    CHECK(!vega.bring_up(in) && rig.card.writes() == 0);
  }
  {
    Rig rig;   // the memory BAR is too small for the block, or cannot be mapped
    AmdVegaM vega(rig.alloc);
    AmdVegaM::Inputs in = rig.inputs();
    in.aperture_bytes = 32ull << 20;
    CHECK(!vega.bring_up(in) && strstr(vega.status(), "memory: ") && rig.card.writes() == 0);
    in = rig.inputs();
    in.max_framebuffer_bytes = 90ull << 20;
    CHECK(!vega.bring_up(in) && strstr(vega.status(), "does not fit") && rig.card.writes() == 0);
    Rig small;   // video memory smaller than the block reaches (the memory BAR is not)
    small.card.gmc.set_reg(0x150a, 52);
    AmdVegaM v2(small.alloc);
    CHECK(!v2.bring_up(small.inputs()) && strstr(v2.status(), "too small") && small.card.writes() == 0);
    rig.mapper.refuse = true;
    CHECK(!vega.bring_up(rig.inputs()) && strstr(vega.status(), "cannot be mapped") && rig.card.writes() == 0);
  }
  {
    Rig rig;   // the SMC is not running: the memory controller and the interrupt ring that came up are stopped again
    rig.card.gfx.smc_running = false;
    AmdVegaM vega(rig.alloc);
    CHECK(!vega.bring_up(rig.inputs()) && strstr(vega.status(), "microcode: SMC not answering") && vega.step() == AmdVegaM::Step::InterruptHandler);
    CHECK(rig.card.gmc.reg(0x504) == 0 && !(rig.card.ih.reg(0xe30) & 1));
  }
  {
    Rig rig;   // the command processor does not run: graphics engine failure, everything stopped
    rig.card.gfx.cp_alive = false;
    AmdVegaM vega(rig.alloc);
    CHECK(!vega.bring_up(rig.inputs()) && strstr(vega.status(), "graphics engine: ring test failed") && vega.step() == AmdVegaM::Step::Microcode);
    CHECK(rig.card.gmc.reg(0x504) == 0 && !(rig.card.ih.reg(0xe30) & 1) && !vega.up());
  }
  {
    Rig rig;   // an SDMA engine that does not run
    rig.card.sdma.stall[1] = true;
    AmdVegaM vega(rig.alloc);
    CHECK(!vega.bring_up(rig.inputs()) && strstr(vega.status(), "SDMA: ring test failed") && vega.step() == AmdVegaM::Step::GraphicsEngine);
    CHECK(rig.card.gfx.unmapped_doorbells.size() == 8 && rig.card.gmc.reg(0x504) == 0);
  }
}

static void test_display() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  CHECK(vega.bring_up(rig.inputs()));
  // The firmware's framebuffer at the start of video memory, shown by CRTC 1; the memory BAR starts at kAperture.
  const uint32_t width = 64, height = 8;
  amd_dce11::FirmwareFramebuffer fw;
  fw.physical = reinterpret_cast<uint64_t>(rig.vram);
  fw.bytes = width * height * 4;
  fw.width = width;
  fw.height = height;
  fw.pitch_pixels = width;
  for (uint32_t i = 0; i < width * height; ++i) reinterpret_cast<uint32_t*>(rig.vram)[i] = 0xFF000000u | i;
  rig.card.dce.crtc(1, kGpuBase, width);
  fakedce::setup_aperture(rig.card.dce);
  rig.card.dce.set(0x80d, kGpuBase >> 12);          // the aperture covers the block: all of video memory
  rig.card.dce.set(0x80e, ((kGpuBase + kVideoMemory) >> 12) - 1);
  // The mapped window for the CPU is where the block is; the firmware's framebuffer lives at the start of the memory BAR.
  AmdVegaM::Inputs in = rig.inputs();
  (void)in;
  ByteView memory;
  amd_dce11::FirmwareFramebuffer wrong = fw;
  wrong.pitch_pixels = width + 8;
  CHECK(!vega.engage_display(wrong, &memory) && !vega.display_engaged());
  CHECK(vega.engage_display(fw, &memory) && vega.display_engaged() && memory.size() == width * height * 4);
  CHECK(memory.data() == rig.vram + kRegion + vega.layout().framebuffer && memcmp(memory.data(), rig.vram, width * height * 4) == 0);
  CHECK(rig.card.dce.get(0x1a04 + 0x200) == static_cast<uint32_t>(kGpuBase + kRegion + vega.layout().framebuffer));
  memory[0] = 0x55;
  vega.disengage_display();
  CHECK(!vega.display_engaged() && rig.card.dce.get(0x1a04 + 0x200) == static_cast<uint32_t>(kGpuBase) && rig.vram[0] == 0x55);
  // Shutting down with the display engaged puts the firmware's framebuffer back.
  CHECK(vega.engage_display(fw, &memory) && vega.display_engaged());
  vega.shutdown();
  CHECK(rig.card.dce.get(0x1a04 + 0x200) == static_cast<uint32_t>(kGpuBase));
  // A picture bigger than the block's framebuffer, or a card that is not up, refuses.
  amd_dce11::FirmwareFramebuffer big = fw;
  big.pitch_pixels = 4096;
  big.height = 4096;
  big.bytes = 4096ull * 4096 * 4;
  CHECK(!vega.engage_display(fw, &memory));
  memset(rig.doorbell_base, 0, 0x800 * 4);
  const bool up2 = vega.bring_up(rig.inputs());
  if (!up2) printf("  display test, second bring-up: %s\n", vega.status());
  CHECK(up2 && !vega.engage_display(big, &memory));
}

// The card's addresses for host memory: the low 40 bits of the pointer (the host's pointers are wider than the card's address space).
struct LowBits final : ISystemAddresses {
  uint64_t physical_address(const void* p) const override { return reinterpret_cast<uint64_t>(p) & 0xFFFFFFFFFFull; }
};

// The OS's picture in system memory sent to the display by Presenter: through the SDMA engine, and by the CPU when the engine cannot.
static void test_present() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  LowBits low;
  AmdVegaM::Inputs in = rig.inputs();
  in.system_addresses = &low;
  CHECK(vega.bring_up(in));
  const uint32_t width = 64, height = 16, pitch = width * 4;
  amd_dce11::FirmwareFramebuffer fw;
  fw.physical = reinterpret_cast<uint64_t>(rig.vram);
  fw.bytes = pitch * height;
  fw.width = width;
  fw.height = height;
  fw.pitch_pixels = width;
  for (uint32_t i = 0; i < width * height; ++i) reinterpret_cast<uint32_t*>(rig.vram)[i] = 0xFF000000u | i;
  rig.card.dce.crtc(1, kGpuBase, width);
  fakedce::setup_aperture(rig.card.dce);
  rig.card.dce.set(0x80d, kGpuBase >> 12);
  rig.card.dce.set(0x80e, ((kGpuBase + kVideoMemory) >> 12) - 1);
  ByteView video;
  CHECK(vega.engage_display(fw, &video));
  if (!vega.display_engaged()) return;
  rig.card.sdma.gart_start = vega.gart_plan().gart_start;
  rig.card.sdma.gart_size = vega.gart_plan().gart_size;
  rig.card.sdma.gart_table = rig.vram + kRegion + vega.layout().gart_table;
  amd_dce11::Presenter presenter(rig.alloc);
  CHECK(presenter.start(vega, video, pitch, height) && presenter.buffer().size() == pitch * height && memcmp(presenter.buffer().data(), video.data(), pitch * height) == 0);
  rig.card.sdma.system_by_high = true;
  rig.card.sdma.system_high = reinterpret_cast<uint64_t>(presenter.buffer().data()) & ~0xFFFFFFFFFFull;
  // Draw in the back buffer: video memory has not changed until the rows are presented.
  uint8_t* back = presenter.buffer().data();
  for (uint32_t y = 4; y < 8; ++y) memset(back + y * pitch, 0x30 + y, pitch);
  CHECK(memcmp(video.data() + 4 * pitch, back + 4 * pitch, pitch) != 0);
  const uint32_t copies = rig.card.sdma.copies;
  CHECK(presenter.present(4, 4) && presenter.dma_presents() == 1 && presenter.cpu_presents() == 0 && rig.card.sdma.copies == copies + 1);
  CHECK(memcmp(video.data() + 4 * pitch, back + 4 * pitch, 4 * pitch) == 0 && memcmp(video.data(), back, 4 * pitch) == 0);
  CHECK(video.data()[3 * pitch] == back[3 * pitch] && video.data()[8 * pitch] == back[8 * pitch] && rig.card.sdma.gart_faults == 0);
  // Rows outside the picture are refused.
  CHECK(!presenter.present(16, 1) && !presenter.present(20, 1) && !presenter.present(15, 2) && !presenter.present(0, 0) && rig.card.sdma.copies == copies + 1);
  // The engine stalls: the CPU copies instead, and the picture is still right.
  rig.card.sdma.stall[0] = true;
  memset(back + 10 * pitch, 0x77, 2 * pitch);
  CHECK(presenter.present(10, 2) && presenter.cpu_presents() == 1 && memcmp(video.data() + 10 * pitch, back + 10 * pitch, 2 * pitch) == 0);
  // Once the engine is wedged it is not tried again (no wait for it each time), and the next presents go by the CPU.
  memset(back + 12 * pitch, 0x78, pitch);
  CHECK(presenter.present(12, 1) && presenter.cpu_presents() == 2 && presenter.dma_presents() == 1 && memcmp(video.data() + 12 * pitch, back + 12 * pitch, pitch) == 0);
  // A picture of another size than the display's does not start.
  amd_dce11::Presenter other(rig.alloc);
  CHECK(!other.start(vega, video, pitch, height + 1) && !other.start(vega, video, 0, height) && !other.present(0, 1));
  // Giving the screen back: what was drawn goes to the firmware's framebuffer.
  memset(back, 0x99, pitch);
  CHECK(presenter.present_all());
  vega.disengage_display();
  CHECK(rig.vram[0] == 0x99 && rig.vram[pitch - 1] == 0x99);
}

// Vectors the card posts to the interrupt ring are dispatched by source: counted, faults wedge the ring they came from, VM faults are read from the memory controller and cleared.
struct Recorder final : AmdIrq::ISink {
  std::vector<AmdIrq::Event> events;
  void on_event(const AmdIrq::Event& e) override { events.push_back(e); }
};

static void test_interrupts() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  CHECK(vega.bring_up(rig.inputs()));
  if (!vega.up()) return;
  CHECK(vega.irq() && vega.service_interrupts() == 0);
  const uint32_t nop_early[] = {0xC0001000};
  // Fences and an unknown source: counted, nothing wedged.
  rig.card.ih.post(181, 0, 0, 0, 0);                 // graphics fence
  rig.card.ih.post(181, 0, (2u << 4) | (1u << 2), 0, 0);   // compute fence, ME 1 queue 2
  rig.card.ih.post(224, 0, 1, 0, 0);                 // SDMA 1 trap
  rig.card.ih.post(99, 0x123, 0, 0, 0);              // not one of ours
  CHECK(vega.service_interrupts() == 4);
  const AmdIrq& irq = *vega.irq();
  CHECK(irq.total() == 4 && irq.count(AmdIrq::Kind::GraphicsFence) == 1 && irq.count(AmdIrq::Kind::ComputeFence) == 1 && irq.count(AmdIrq::Kind::SdmaTrap) == 1 &&
        irq.count(AmdIrq::Kind::Unknown) == 1 && irq.unknown_source(99) == 1);
  CHECK(!vega.graphics()->wedged() && !vega.sdma()->wedged(0) && !vega.sdma()->wedged(1) && vega.service_interrupts() == 0);
  // Faults wedge the ring: a privileged register on the graphics ring, an illegal instruction on SDMA 1, a privileged instruction on compute queue 2 (and one from a queue that is
  // not ours changes nothing).
  rig.card.ih.post(184, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && vega.graphics()->wedged() && !vega.sdma()->wedged(1));
  rig.card.ih.post(241, 0, 1, 0, 0);
  CHECK(vega.service_interrupts() == 1 && vega.sdma()->wedged(1) && !vega.sdma()->wedged(0));
  rig.card.ih.post(185, 0, (7u << 4) | (1u << 2) | 3, 0, 0);   // pipe 3 queue 7: the KIQ is pipe 3 queue 0, not this
  CHECK(vega.service_interrupts() == 1);
  rig.card.ih.post(185, 0, (2u << 4) | (1u << 2), 0, 0);
  CHECK(vega.service_interrupts() == 1);
  rig.card.ih.post(185, 0, (5u << 4) | (1u << 2) | 1, 0, 0);   // ME 1 pipe 1 queue 5: queue 5 of pipe 0 is ours, this is not
  CHECK(vega.service_interrupts() == 1 && vega.submit_compute(5, ArrayView<const uint32_t>(nop_early, 1)));
  // (a wedged ring refuses command buffers)
  const uint32_t nop[] = {0xC0001000};
  CHECK(!vega.submit_graphics(ArrayView<const uint32_t>(nop, 1)) && !vega.submit_sdma(1, ArrayView<const uint32_t>(nop, 1)) && !vega.submit_compute(2, ArrayView<const uint32_t>(nop, 1)));
  CHECK(vega.submit_compute(1, ArrayView<const uint32_t>(nop, 1)) && vega.submit_sdma(0, ArrayView<const uint32_t>(nop, 1)));
  CHECK(irq.count(AmdIrq::Kind::GfxPrivilegedRegister) == 1 && irq.count(AmdIrq::Kind::SdmaIllegalInstruction) == 1 && irq.count(AmdIrq::Kind::GfxPrivilegedInstruction) == 3);
  // A VM fault: the registers are read and cleared (bit 0 of VM_CONTEXT1_CNTL2).
  rig.card.gmc.set_reg(0x53f, 0x12345);                                 // ADDR: page
  rig.card.gmc.set_reg(0x537, 0x1000000 | (5u << 25) | (0x2A << 12) | 0x3);   // STATUS: write, vmid 5, client 0x2A, protections 3
  rig.card.gmc.set_reg(0x539, 0x43425F30);                              // MCCLIENT: "CB_0"
  rig.card.ih.post(146, 0, 0, 5, 77);
  CHECK(vega.service_interrupts() == 1);
  const AmdIrq::VmFaultInfo& f = irq.last_vm_fault();
  CHECK(f.valid && f.page == 0x12345 && f.vmid == 5 && f.client_id == 0x2A && f.protections == 3 && f.write && f.mc_client == 0x43425F30 && irq.count(AmdIrq::Kind::VmFault) == 1);
  CHECK((rig.card.gmc.reg(0x50d) & 1) == 1);
  // A fault interrupt with both registers zero reports nothing and is not counted; but the registers are cleared all the same.
  rig.card.gmc.set_reg(0x53f, 0);
  rig.card.gmc.set_reg(0x537, 0);
  rig.card.gmc.set_reg(0x50d, 0);
  rig.card.ih.post(147, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.count(AmdIrq::Kind::VmFault) == 1 && (rig.card.gmc.reg(0x50d) & 1) == 1);
  // A fault at page 0 (the status alone says there was one) is reported; so is one with the status zero.
  rig.card.gmc.set_reg(0x53f, 0);
  rig.card.gmc.set_reg(0x537, 0x3);
  rig.card.ih.post(147, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.count(AmdIrq::Kind::VmFault) == 2 && irq.last_vm_fault().page == 0 && irq.last_vm_fault().protections == 3);
  rig.card.gmc.set_reg(0x53f, 0x77);
  rig.card.gmc.set_reg(0x537, 0);
  rig.card.ih.post(147, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.count(AmdIrq::Kind::VmFault) == 3 && irq.last_vm_fault().page == 0x77);
  // A limit on the vectors handled in one call.
  for (int i = 0; i < 10; ++i) rig.card.ih.post(181, 0, 0, 0, 0);
  CHECK(vega.service_interrupts(4) == 4 && vega.service_interrupts(4) == 4 && vega.service_interrupts() == 2 && vega.service_interrupts() == 0);
  // After a restart the engines work again and a card that is not up handles nothing.
  vega.shutdown();
  CHECK(vega.service_interrupts() == 0 && !vega.irq());
}

// The sink sees every event in order, with the ring id decoded.
static void test_interrupt_sink() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  CHECK(vega.bring_up(rig.inputs()));
  if (!vega.up()) return;
  AmdIh3* ih = vega.interrupts();
  CHECK(ih != nullptr);
  AmdIrq irq(rig.card);
  Recorder rec;
  rig.card.ih.post(181, 0x77, (3u << 4) | (2u << 2) | 1, 4, 9);
  rig.card.ih.post(247, 0, 1, 0, 0);
  CHECK(irq.process(*ih, &rec) == 2 && rec.events.size() == 2);
  CHECK(rec.events[0].kind == AmdIrq::Kind::ComputeFence && rec.events[0].me == 2 && rec.events[0].pipe == 1 && rec.events[0].queue == 3 && rec.events[0].vector.src_data == 0x77 &&
        rec.events[0].vector.vmid == 4 && rec.events[0].vector.pasid == 9);
  CHECK(rec.events[1].kind == AmdIrq::Kind::SdmaIllegalInstruction && rec.events[1].engine == 1);
}

// With MSI on (Inputs::msi) the interrupt ring is started the way the driver starts it with adev->irq.msi_enabled: the read pointer re-arms the message. Nothing else differs.
static void test_msi_input() {
  uint32_t cntl[2], other_writes[2];
  for (int msi = 0; msi < 2; ++msi) {
    Rig rig;
    AmdVegaM vega(rig.alloc);
    AmdVegaM::Inputs in = rig.inputs();
    in.msi = msi != 0;
    CHECK(vega.bring_up(in));
    cntl[msi] = rig.card.ih.reg(0xe30);
    other_writes[msi] = static_cast<uint32_t>(rig.card.ih.writes.size());
    CHECK(vega.service_interrupts() == 0);
  }
  const uint32_t diff = cntl[0] ^ cntl[1];
  CHECK(diff != 0 && (diff & (diff - 1)) == 0 && (cntl[1] & diff) && other_writes[0] == other_writes[1]);
}

// SQ messages: the word in the vector's data is decoded, and for a wave's report the error source is read from SQ_EDC_INFO of that compute unit (GRBM_GFX_INDEX selected for it and
// put back to broadcasting).
static void test_sq_messages() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  CHECK(vega.bring_up(rig.inputs()));
  if (!vega.up()) return;
  const AmdIrq& irq = *vega.irq();
  // EDC/ECC error (encoding 2) of SE 1, SH 1, CU 5, SIMD 2, wave 7, VM 9, privileged; the source in SQ_EDC_INFO is VGPR (3).
  const uint32_t wave = (2u << 26) | (1u << 24) | (5u << 20) | (2u << 18) | (7u << 14) | (9u << 10) | 0x200 | 0x100 | 0x55;
  rig.card.gfx.set_reg(0x23a3, (3u << 6) | 0x1E01);
  const size_t before = rig.card.gfx.writes.size();
  rig.card.ih.post(239, wave, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.count(AmdIrq::Kind::SqMessage) == 1);
  const AmdIrq::SqInfo& q = irq.last_sq();
  CHECK(q.encoding == 2 && q.se_id == 1 && q.sh_id == 1 && q.cu_id == 5 && q.simd_id == 2 && q.wave_id == 7 && q.vm_id == 9 && q.priv && q.edc_valid && q.edc_source == 3);
  CHECK(AmdIrq::sq_edc_source_name(q.edc_source)[0] == 'V' && AmdIrq::sq_edc_source_name(7)[0] == 'i');
  // GRBM_GFX_INDEX: SE 1 << 16 | SH 1 << 8 | instance (CU) 5, then everything broadcast again.
  CHECK(rig.card.gfx.writes.size() == before + 2 && rig.card.gfx.writes[before].first == 0xc200 && rig.card.gfx.writes[before].second == 0x00010105 &&
        rig.card.gfx.writes[before + 1].first == 0xc200 && rig.card.gfx.writes[before + 1].second == 0xE0000000u);
  // An instruction interrupt (encoding 1) reads the same way; the source is whatever the register says.
  rig.card.gfx.set_reg(0x23a3, 6u << 6);
  rig.card.ih.post(239, (1u << 26) | (3u << 24) | (0xF << 20), 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.last_sq().encoding == 1 && irq.last_sq().se_id == 3 && irq.last_sq().cu_id == 15 && irq.last_sq().edc_source == 6 && !irq.last_sq().priv);
  // A general purpose event (encoding 0): the flags, no register is touched.
  const size_t writes = rig.card.gfx.writes.size();
  rig.card.ih.post(239, (2u << 24) | 0x8A, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.last_sq().encoding == 0 && irq.last_sq().se_id == 2 && irq.last_sq().general == 0x8A && !irq.last_sq().edc_valid && rig.card.gfx.writes.size() == writes);
  // An encoding that does not exist is counted and left alone.
  rig.card.ih.post(239, 3u << 26, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.last_sq().encoding == 3 && !irq.last_sq().edc_valid && rig.card.gfx.writes.size() == writes && irq.count(AmdIrq::Kind::SqMessage) == 4);
  // The CP's ECC error is counted too, and wedges nothing.
  rig.card.ih.post(197, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.count(AmdIrq::Kind::GfxEccError) == 1 && !vega.graphics()->wedged());
}

// The display engine's interrupts: page flips of the six controllers and the hot plug pins on from bring-up (vertical blank only when asked), dispatched with the driver's
// acknowledgements, a pin that will not stop interrupting switched off, everything off again at shutdown.
constexpr uint32_t kCrtcOff[6] = {0x0, 0x200, 0x400, 0x2600, 0x2800, 0x2a00};   // CRTCn_REGISTER_OFFSET (vid.h)

static void test_display_interrupts() {
  Rig rig;
  rig.card.gfx.set_reg(0x1898 + 8, 0x2);    // pin 1 senses something connected
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.hpd_pins = 0x0F;                       // pins 0..3
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  const AmdIrq& irq = *vega.irq();
  // Page flip interrupts on every controller (GRPH_INTERRUPT_CONTROL, bit 0).
  for (uint32_t c = 0; c < 6; ++c) CHECK(rig.card.dce.get(0x1a17 + kCrtcOff[c]) == 1);
  // The pins asked for: DC_HPD_EN (bit 28), the filter (connect 50 ms, disconnect 10 ms << 20), the interrupt on with the polarity for the next change; the others untouched.
  for (uint32_t p = 0; p < 4; ++p) {
    const uint32_t polarity = p == 1 ? 0 : 0x100;   // pin 1 is connected: the next change is a disconnect
    CHECK(rig.card.gfx.reg(0x189a + 8 * p) == 0x10000000 && rig.card.gfx.reg(0x189c + 8 * p) == (50u | 10u << 20) && rig.card.gfx.reg(0x1899 + 8 * p) == (0x10000 | polarity | 1));
  }
  for (uint32_t p = 4; p < 6; ++p) CHECK(rig.card.gfx.reg(0x189a + 8 * p) == 0 && rig.card.gfx.reg(0x1899 + 8 * p) == 0 && rig.card.gfx.reg(0x189c + 8 * p) == 0);
  // The order for a pin: enable, filter, acknowledge, polarity, interrupt on.
  {
    std::vector<uint32_t> regs;
    for (const auto& w : rig.card.gfx.writes) if (w.first >= 0x1898 && w.first <= 0x189f) regs.push_back(w.first);
    const uint32_t expect[] = {0x189a, 0x189c, 0x1899, 0x1899, 0x1899};
    CHECK(regs.size() >= 5 && memcmp(regs.data(), expect, sizeof(expect)) == 0);
  }
  // The vertical blank is off until asked for, and cannot be asked for without a display.
  CHECK(rig.card.dce.get(0x1ac8 + 0x200) == 0 && !vega.set_vblank_interrupt(true) && vega.set_vblank_interrupt(false));

  // A page flip of controller 1 (source 10): acknowledged when the status says it happened, counted either way.
  rig.card.dce.set(0x1a16 + 0x200, 1);
  rig.card.ih.post(10, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.pageflip_count(1) == 1 && irq.count(AmdIrq::Kind::DisplayPageFlip) == 1 && rig.card.dce.get(0x1a16 + 0x200) == 0x100);
  rig.card.dce.set(0x1a16 + 0x200, 0);
  rig.card.ih.post(18, 0, 0, 0, 0);   // controller 5
  CHECK(vega.service_interrupts() == 1 && irq.pageflip_count(5) == 1 && rig.card.dce.get(0x1a16 + 0x2a00) == 0 && irq.pageflip_count(1) == 1);

  // Hot plug: pin 0 is asserted in DISP_INTERRUPT_STATUS (bit 17); a device has been plugged in since the polarity was set.
  rig.card.gfx.set_reg(0x1857, 0x20000);
  rig.card.gfx.set_reg(0x1898, 0x2);
  rig.card.ih.post(42, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.count(AmdIrq::Kind::DisplayHotplug) == 1 && irq.hotplug_count(0) == 1 && irq.hpd_connected(0) && !irq.hpd_connected(1));
  CHECK((rig.card.gfx.reg(0x1899) & 0x100) == 0 && (rig.card.gfx.reg(0x1899) & 1) == 1 && (rig.card.gfx.reg(0x1899) & 0x10000));   // acknowledged, polarity now "disconnect", still on
  // Not asserted (pin 1 in the second status register): the driver does nothing, and nothing is counted.
  const size_t writes = rig.card.gfx.writes.size();
  rig.card.ih.post(42, 1, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.hotplug_count(1) == 0 && irq.count(AmdIrq::Kind::DisplayHotplug) == 1 && rig.card.gfx.writes.size() == writes);
  // Unplugged again: pin 0 reports the change the other way.
  rig.card.gfx.set_reg(0x1898, 0x0);
  rig.card.ih.post(42, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.hotplug_count(0) == 2 && !irq.hpd_connected(0) && (rig.card.gfx.reg(0x1899) & 0x100));
  // A pin that does not exist.
  rig.card.ih.post(42, 6, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.count(AmdIrq::Kind::DisplayHotplug) == 2 && irq.count(AmdIrq::Kind::Unknown) == 1);

  // A pin that keeps interrupting is switched off after kHotplugStormLimit events (pin 2: DISP_INTERRUPT_STATUS_CONTINUE2 bit 17).
  rig.card.gfx.set_reg(0x1859, 0x20000);
  for (uint32_t i = 0; i < AmdIrq::kHotplugStormLimit + 4; ++i) {
    rig.card.ih.post(42, 2, 0, 0, 0);
    vega.service_interrupts();
    if (i + 1 == AmdIrq::kHotplugStormLimit) CHECK(!irq.hpd_throttled(2) && (rig.card.gfx.reg(0x1899 + 16) & 0x10000));
  }
  CHECK(irq.hpd_throttled(2) && !(rig.card.gfx.reg(0x1899 + 16) & 0x10000) && !irq.hpd_throttled(0) && (rig.card.gfx.reg(0x1899) & 0x10000));

  // Shutdown: pins off (DC_HPD_EN and the interrupt), page flip interrupts off.
  vega.shutdown();
  for (uint32_t p = 0; p < 4; ++p) CHECK(!(rig.card.gfx.reg(0x189a + 8 * p) & 0x10000000) && !(rig.card.gfx.reg(0x1899 + 8 * p) & 0x10000));
  for (uint32_t c = 0; c < 6; ++c) CHECK(rig.card.dce.get(0x1a17 + kCrtcOff[c]) == 0);
}

static void test_vblank_interrupt() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  CHECK(vega.bring_up(rig.inputs()));
  const uint32_t width = 64, height = 8;
  amd_dce11::FirmwareFramebuffer fw;
  fw.physical = reinterpret_cast<uint64_t>(rig.vram);
  fw.bytes = width * height * 4;
  fw.width = width;
  fw.height = height;
  fw.pitch_pixels = width;
  for (uint32_t i = 0; i < width * height; ++i) reinterpret_cast<uint32_t*>(rig.vram)[i] = 0xFF000000u | i;
  rig.card.dce.crtc(2, kGpuBase, width);   // controller 2 (the third) shows the firmware's framebuffer
  fakedce::setup_aperture(rig.card.dce);
  rig.card.dce.set(0x80d, kGpuBase >> 12);
  rig.card.dce.set(0x80e, ((kGpuBase + kVideoMemory) >> 12) - 1);
  ByteView memory;
  CHECK(vega.engage_display(fw, &memory) && vega.display_engaged());
  const AmdIrq& irq = *vega.irq();
  // On for the controller that shows the framebuffer (LB_INTERRUPT_MASK bit 0), only that one; asking twice moves nothing.
  rig.card.dce.set(0x1ac8 + 0x400, 0x110);   // (other bits are kept)
  CHECK(vega.set_vblank_interrupt(true) && rig.card.dce.get(0x1ac8 + 0x400) == 0x111 && rig.card.dce.get(0x1ac8) == 0 && rig.card.dce.get(0x1ac8 + 0x200) == 0);
  CHECK(vega.set_vblank_interrupt(true) && rig.card.dce.get(0x1ac8 + 0x400) == 0x111);
  // A vertical blank of controller 2 (source 3, data 0): DISP_INTERRUPT_STATUS_CONTINUE2 (0x1859) bit 3 says it is pending; acknowledged with LB_VBLANK_STATUS.VBLANK_ACK (0x10).
  rig.card.gfx.set_reg(0x1859, 0x8);
  rig.card.ih.post(3, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.vblank_count(2) == 1 && irq.count(AmdIrq::Kind::DisplayVblank) == 1 && rig.card.dce.get(0x1acb + 0x400) == 0x10);
  // The status does not say so: counted, nothing acknowledged.
  rig.card.dce.set(0x1acb + 0x400, 0);
  rig.card.gfx.set_reg(0x1859, 0);
  rig.card.ih.post(3, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.vblank_count(2) == 2 && rig.card.dce.get(0x1acb + 0x400) == 0);
  // The line interrupt (data 1) is acknowledged with LB_VLINE_STATUS.VLINE_ACK (0x10) when pending, and is not a vblank.
  rig.card.gfx.set_reg(0x1859, 0x4);
  rig.card.ih.post(3, 1, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.count(AmdIrq::Kind::DisplayVline) == 1 && irq.vblank_count(2) == 2 && rig.card.dce.get(0x1ac9 + 0x400) == 0x10);
  // Giving the display back turns it off, and it can be asked for again afterwards only with a display.
  vega.disengage_display();
  CHECK(rig.card.dce.get(0x1ac8 + 0x400) == 0x110 && !vega.set_vblank_interrupt(true));
  CHECK(vega.engage_display(fw, &memory) && vega.set_vblank_interrupt(true) && (rig.card.dce.get(0x1ac8 + 0x400) & 1));
  vega.shutdown();   // (also off at shutdown)
  CHECK(!(rig.card.dce.get(0x1ac8 + 0x400) & 1));
}

// The video engines' fences: UVD's decode ring (124), its two encode rings (119, 120) and VCE's three rings (167, data 0..2) are classified and counted; a VCE fence is acknowledged in
// VCE_SYS_INT_STATUS (bit 3) before anything else; an observer sees every event, in order, after the card dealt with it.
static void test_video_engine_interrupts() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  CHECK(vega.bring_up(rig.inputs()));
  if (!vega.up()) return;
  const AmdIrq& irq = *vega.irq();
  Recorder rec;
  vega.set_event_observer(&rec);
  rig.card.ih.post(124, 0, 0, 0, 0);
  rig.card.ih.post(119, 0, 0, 0, 0);
  rig.card.ih.post(120, 0, 0, 0, 0);
  rig.card.ih.post(120, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 4 && irq.count(AmdIrq::Kind::UvdFence) == 1 && irq.count(AmdIrq::Kind::UvdEncodeFence) == 3 && irq.uvd_encode_fences(0) == 1 && irq.uvd_encode_fences(1) == 2);
  CHECK(rig.card.gfx.reg(0x8541) == 0 && irq.uvd_encode_fences(2) == 0);   // (UVD has nothing to acknowledge here)
  rig.card.ih.post(167, 2, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.vce_fences(2) == 1 && irq.vce_fences(0) == 0 && irq.count(AmdIrq::Kind::VceFence) == 1);
  CHECK(rig.card.gfx.reg(0x8541) == 0x8);
  rig.card.gfx.set_reg(0x8541, 0x1);   // the other bits are kept
  rig.card.ih.post(167, 0, 0, 0, 0);
  rig.card.ih.post(167, 1, 0, 0, 0);
  CHECK(vega.service_interrupts() == 2 && irq.vce_fences(0) == 1 && irq.vce_fences(1) == 1 && rig.card.gfx.reg(0x8541) == 0x9);
  // A ring that does not exist: not a fence, not acknowledged.
  rig.card.gfx.set_reg(0x8541, 0);
  rig.card.ih.post(167, 3, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.count(AmdIrq::Kind::VceFence) == 3 && irq.unknown_source(167) == 1 && rig.card.gfx.reg(0x8541) == 0);
  // The observer saw all eight events in the order they were posted.
  CHECK(rec.events.size() == 8);
  const AmdIrq::Kind order[] = {AmdIrq::Kind::UvdFence, AmdIrq::Kind::UvdEncodeFence, AmdIrq::Kind::UvdEncodeFence, AmdIrq::Kind::UvdEncodeFence,
                                AmdIrq::Kind::VceFence, AmdIrq::Kind::VceFence, AmdIrq::Kind::VceFence, AmdIrq::Kind::Unknown};
  for (size_t i = 0; i < rec.events.size() && i < 8; ++i) CHECK(rec.events[i].kind == order[i]);
  CHECK(rec.events[1].ring == 0 && rec.events[2].ring == 1 && rec.events[4].ring == 2 && rec.events[5].ring == 0 && rec.events[6].ring == 1);
  // Stopped observing.
  vega.set_event_observer(nullptr);
  rig.card.ih.post(124, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && rec.events.size() == 8);
}

// Recovery of the whole card: a fault or a timeout wedges a ring, recover() resets the engines that are wedged and only those.
static void test_recovery() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  CHECK(vega.bring_up(rig.inputs()));
  if (!vega.up()) return;
  const uint32_t nop[] = {0xC0001000};
  const ArrayView<const uint32_t> ib(nop, 1);
  // Nothing wedged: nothing is done.
  const size_t writes = rig.card.writes();
  AmdVegaM::Recovery r = vega.recover();
  CHECK(!vega.wedged() && !r.graphics && !r.sdma && !r.failed && rig.card.writes() == writes && vega.recoveries() == 0);
  // A privileged-register fault on the graphics ring: wedged; recovering resets graphics (and the compute queues with it) and not the DMA engines.
  rig.card.ih.post(184, 0, 0, 0, 0);
  vega.service_interrupts();
  CHECK(vega.wedged() && !vega.submit_graphics(ib));
  auto sdma_resets = [&]() { size_t n = 0; for (uint32_t v : rig.card.sdma.srbm_resets) n += (v & 0x100040) != 0; return n; };   // (SRBM_SOFT_RESET reaches both fakes: count the SDMA bits)
  const size_t sdma_before = sdma_resets();
  r = vega.recover();
  CHECK(r.graphics && !r.sdma && !r.failed && vega.recoveries() == 1 && !vega.wedged() && sdma_resets() == sdma_before);
  CHECK(vega.submit_graphics(ib) && vega.submit_compute(3, ib) && vega.submit_sdma(1, ib));
  // The interrupt sources the reset took are back: the graphics ring's, the compute pipe's, ECC and SQ.
  CHECK((rig.card.gfx.reg(0x306a) & 0x04C00000) == 0x04C00000 && (rig.card.gfx.reg(0x3085) & 0x04000000) && (rig.card.gfx.reg(0x3049) & 0x4000) && !(rig.card.gfx.reg(0x2315) & 1));
  // A fault of one compute queue: all the compute queues are reset with the graphics engine, as the driver's soft reset of the IP does.
  const size_t mapped = rig.card.gfx.mapped.size();
  rig.card.ih.post(185, 0, (2u << 4) | (1u << 2), 0, 0);   // ME 1, pipe 0, queue 2
  vega.service_interrupts();
  CHECK(vega.wedged() && !vega.submit_compute(2, ib) && vega.submit_compute(3, ib));
  r = vega.recover();
  CHECK(r.graphics && !r.sdma && vega.recoveries() == 2 && rig.card.gfx.mapped.size() == mapped + 8 && vega.submit_compute(2, ib));
  // An SDMA illegal instruction: the DMA engines are reset (both), the graphics engine is left alone, TRAP_ENABLE is back.
  const size_t gfx_resets = rig.card.gfx.grbm_resets.size();
  rig.card.ih.post(241, 0, 1, 0, 0);
  vega.service_interrupts();
  CHECK(vega.wedged() && !vega.submit_sdma(1, ib));
  r = vega.recover();
  CHECK(!r.graphics && r.sdma && !r.failed && vega.recoveries() == 3 && rig.card.gfx.grbm_resets.size() == gfx_resets && sdma_resets() == sdma_before + 1);
  CHECK(vega.submit_sdma(1, ib) && vega.submit_sdma(0, ib) && (rig.card.sdma.reg(0x3404) & 1) && (rig.card.sdma.reg(0x3604) & 1));
  // Both at once.
  rig.card.ih.post(184, 0, 0, 0, 0);
  rig.card.ih.post(247, 0, 0, 0, 0);
  vega.service_interrupts();
  r = vega.recover();
  CHECK(r.graphics && r.sdma && !r.failed && vega.recoveries() == 5 && !vega.wedged());
  // service_interrupts() does not recover by itself unless asked to (Inputs::auto_recover).
  rig.card.ih.post(184, 0, 0, 0, 0);
  vega.service_interrupts();
  CHECK(vega.wedged());
  vega.recover();
  // The card is shut down: nothing to recover.
  vega.shutdown();
  r = vega.recover();
  CHECK(!r.graphics && !r.sdma && !vega.wedged());
}

static void test_auto_recovery() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.auto_recover = true;
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  const uint32_t nop[] = {0xC0001000};
  const ArrayView<const uint32_t> ib(nop, 1);
  // A fault is dealt with in the same pass.
  rig.card.ih.post(184, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && !vega.wedged() && vega.recoveries() == 1 && vega.submit_graphics(ib));
  rig.card.ih.post(241, 0, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && !vega.wedged() && vega.recoveries() == 2 && vega.submit_sdma(0, ib));
  // A card that keeps hanging is left alone after kMaxRecoveries.
  for (uint32_t i = 2; i < AmdVegaM::kMaxRecoveries; ++i) {
    rig.card.ih.post(184, 0, 0, 0, 0);
    vega.service_interrupts();
  }
  CHECK(vega.recoveries() == AmdVegaM::kMaxRecoveries && !vega.wedged());
  rig.card.ih.post(184, 0, 0, 0, 0);
  vega.service_interrupts();
  CHECK(vega.wedged() && vega.recoveries() == AmdVegaM::kMaxRecoveries && !vega.submit_graphics(ib));
  // Asking for it by hand still works.
  const AmdVegaM::Recovery r = vega.recover();
  CHECK(r.graphics && !r.failed && !vega.wedged() && vega.recoveries() == AmdVegaM::kMaxRecoveries + 1 && vega.submit_graphics(ib));
}

static void test_recovery_failure() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  CHECK(vega.bring_up(rig.inputs()));
  if (!vega.up()) return;
  const uint32_t nop[] = {0xC0001000};
  const ArrayView<const uint32_t> ib(nop, 1);
  // The command processor hangs and the reset does not help: the engine comes back stopped and the card says so.
  rig.card.gfx.hung = true;
  rig.card.gfx.reset_fails = true;
  CHECK(!vega.submit_graphics(ib, 2000) && vega.wedged());
  AmdVegaM::Recovery r = vega.recover();
  CHECK(r.graphics && r.failed && !vega.graphics()->running() && !vega.submit_graphics(ib));
  // The DMA engines are independent: a stalled one resets fine, and the graphics engine is tried again (it is stopped, not wedged) and stays down.
  rig.card.sdma.stall[0] = true;
  CHECK(!vega.submit_sdma(0, ib, 2000) && vega.wedged());
  r = vega.recover();
  CHECK(r.graphics && r.sdma && r.failed && !vega.graphics()->running() && vega.sdma()->running() && vega.submit_sdma(0, ib));
  // When the hardware lets the reset work, the next recovery brings the graphics engine back.
  rig.card.gfx.reset_fails = false;
  CHECK(vega.wedged());
  r = vega.recover();
  CHECK(r.graphics && !r.sdma && !r.failed && !vega.wedged() && vega.graphics()->running() && vega.submit_graphics(ib) && vega.submit_compute(0, ib));
}

static void test_recovery_failure_sdma() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  CHECK(vega.bring_up(rig.inputs()));
  if (!vega.up()) return;
  const uint32_t nop[] = {0xC0001000};
  const ArrayView<const uint32_t> ib(nop, 1);
  // A DMA engine whose reset does not work: failed (and only the DMA engines were reset), tried again next time.
  rig.card.sdma.stall[1] = true;
  rig.card.sdma.reset_fails = true;
  CHECK(!vega.submit_sdma(1, ib, 2000));
  AmdVegaM::Recovery r = vega.recover();
  CHECK(!r.graphics && r.sdma && r.failed && !vega.sdma()->running() && vega.wedged());
  r = vega.recover();   // (stopped, so tried again - and fails again)
  CHECK(!r.graphics && r.sdma && r.failed && vega.recoveries() == 2);
  rig.card.sdma.reset_fails = false;
  r = vega.recover();
  CHECK(!r.graphics && r.sdma && !r.failed && vega.sdma()->running() && !vega.wedged() && vega.submit_sdma(0, ib) && vega.submit_sdma(1, ib));
  // Nothing wedged and running: nothing to do.
  r = vega.recover();
  CHECK(!r.graphics && !r.sdma && !r.failed);
}

// The card that reset itself: the PCI configuration reset, the way the card sees it from its configuration space.
struct FakePci final : IPciBus {
  Rig& rig;
  uint32_t command = 0x0407, status = 0x0010, resets = 0, bm_off = 0;
  bool needs_post = false;      // after the reset the video memory is not trained: the memory controller's registers read as they do at power on
  bool never_returns = false;   // the card does not answer after the reset
  bool clears_hang = true;      // the reset brings a hung engine back
  explicit FakePci(Rig& r) : rig(r) {}
  const char* name() const override { return "pci"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t device_count() const override { return 1; }
  bool pci_device(uint32_t, PciDeviceInfo*) const override { return false; }
  uint32_t read32(PciAddress, uint16_t offset) const override { return offset == 4 ? (command | (status << 16)) : 0; }
  void write32(PciAddress, uint16_t offset, uint32_t value) const override {
    FakePci& self = const_cast<FakePci&>(*this);   // (the interface's writes are const; the fake keeps state)
    if (offset == 4) {
      if ((command & 4) && !(value & 4)) ++self.bm_off;
      self.command = value & 0xFFFF;
    } else if (offset == 0x7c && value == 0x39d5e86b) {
      ++self.resets;
      rig.card.memsize_ones = 4;
      rig.card.memsize_never = never_returns;
      if (clears_hang) {
        rig.card.gfx.hung = false;
        rig.card.gfx.reset_fails = false;
        rig.card.sdma.stall[0] = rig.card.sdma.stall[1] = false;
        rig.card.sdma.reset_fails = false;
        rig.card.sdma.ignore_fences = false;
      }
      memset(rig.doorbell_base, 0, 0x800 * 4);   // (the doorbell BAR is part of the card)
      if (needs_post) {
        rig.card.gmc.set_reg(0x150a, 0);
        rig.card.gmc.set_reg(0x809, 0);
      }
    }
  }
  uint64_t memory_bar(PciAddress, uint8_t) const override { return 0; }
  uint16_t find_capability(PciAddress, uint8_t) const override { return 0; }
};

// The full reset as the fallback when the soft reset does not bring an engine back.
static void test_card_reset() {
  const uint32_t nop[] = {0xC0001000};
  const ArrayView<const uint32_t> ib(nop, 1);
  {
    // A command processor that hangs and does not come back from its soft reset: the card is reset and brought up again, and everything works.
    Rig rig;
    FakePci pci(rig);
    AmdVegaM vega(rig.alloc);
    AmdVegaM::Inputs in = rig.inputs();
    in.pci = &pci;
    in.pci_address = PciAddress{1, 0, 0};
    in.allow_asic_reset = true;
    CHECK(vega.bring_up(in));
    rig.card.gfx.hung = true;
    rig.card.gfx.reset_fails = true;
    CHECK(!vega.submit_graphics(ib, 2000) && vega.wedged());
    const AmdVegaM::Recovery r = vega.recover();
    CHECK(r.graphics && r.card_reset && !r.failed && vega.up() && pci.resets == 1 && vega.asic_resets() == 1 && !vega.wedged());
    CHECK(pci.bm_off == 1 && (pci.command & 4) && pci.status == 0x0010);   // (bus mastering off for the reset, on again after)
    CHECK(vega.submit_graphics(ib) && vega.submit_compute(1, ib) && vega.submit_sdma(0, ib) && vega.submit_sdma(1, ib));
    // It does not try again for a card that is fine.
    CHECK(!vega.recover().card_reset && pci.resets == 1);
  }
  {
    // Not allowed (the default): the soft reset's failure is the answer, and the card's PCI configuration is not touched.
    Rig rig;
    FakePci pci(rig);
    AmdVegaM vega(rig.alloc);
    AmdVegaM::Inputs in = rig.inputs();
    in.pci = &pci;
    CHECK(vega.bring_up(in));
    rig.card.gfx.hung = true;
    rig.card.gfx.reset_fails = true;
    CHECK(!vega.submit_graphics(ib, 2000));
    const AmdVegaM::Recovery r = vega.recover();
    CHECK(r.graphics && !r.card_reset && r.failed && pci.resets == 0 && vega.up() && vega.asic_resets() == 0);
  }
  {
    // Allowed but nowhere to write: the same.
    Rig rig;
    AmdVegaM vega(rig.alloc);
    AmdVegaM::Inputs in = rig.inputs();
    in.allow_asic_reset = true;
    CHECK(vega.bring_up(in));
    rig.card.gfx.hung = true;
    rig.card.gfx.reset_fails = true;
    CHECK(!vega.submit_graphics(ib, 2000));
    const AmdVegaM::Recovery r = vega.recover();
    CHECK(r.failed && !r.card_reset && !vega.reset_card() && vega.asic_resets() == 0);
  }
  {
    // A card that needs its video BIOS to POST it (what a real one does): the reset works, the card answers, but it cannot be brought up. The card is left down and says why.
    Rig rig;
    FakePci pci(rig);
    pci.needs_post = true;
    AmdVegaM vega(rig.alloc);
    AmdVegaM::Inputs in = rig.inputs();
    in.pci = &pci;
    in.allow_asic_reset = true;
    CHECK(vega.bring_up(in));
    rig.card.gfx.hung = true;
    rig.card.gfx.reset_fails = true;
    CHECK(!vega.submit_graphics(ib, 2000));
    const AmdVegaM::Recovery r = vega.recover();
    CHECK(r.card_reset && r.failed && !vega.up() && pci.resets == 1 && strstr(vega.status(), "after a card reset: "));
    CHECK((pci.command & 4) && !vega.submit_graphics(ib) && !vega.wedged() && !vega.recover().graphics);
  }
  {
    // A card that does not answer after the reset: down, bus mastering left off.
    Rig rig;
    FakePci pci(rig);
    pci.never_returns = true;
    AmdVegaM vega(rig.alloc);
    AmdVegaM::Inputs in = rig.inputs();
    in.pci = &pci;
    in.allow_asic_reset = true;
    CHECK(vega.bring_up(in));
    rig.card.gfx.hung = true;
    rig.card.gfx.reset_fails = true;
    CHECK(!vega.submit_graphics(ib, 2000));
    const AmdVegaM::Recovery r = vega.recover();
    CHECK(r.card_reset && r.failed && !vega.up() && !(pci.command & 4) && strstr(vega.status(), "card reset: the card did not answer"));
  }
  {
    // The reset that does not help either: tried at most kMaxAsicResets times, then left alone.
    Rig rig;
    FakePci pci(rig);
    pci.clears_hang = false;
    AmdVegaM vega(rig.alloc);
    AmdVegaM::Inputs in = rig.inputs();
    in.pci = &pci;
    in.allow_asic_reset = true;
    CHECK(vega.bring_up(in));
    rig.card.gfx.hung = true;
    rig.card.gfx.reset_fails = true;
    CHECK(!vega.submit_graphics(ib, 2000));
    AmdVegaM::Recovery r = vega.recover();
    CHECK(r.card_reset && r.failed && !vega.up() && pci.resets == 1);   // (brought up again, but the ring test of the still hung card fails: down)
    // Bring it up by hand for the next try; the engine hangs again.
    rig.card.gfx.hung = false;
    rig.card.gfx.reset_fails = false;
    memset(rig.doorbell_base, 0, 0x800 * 4);   // (a new life of the card starts with clean doorbells)
    CHECK(vega.bring_up(in));
    rig.card.gfx.hung = true;
    rig.card.gfx.reset_fails = true;
    CHECK(!vega.submit_graphics(ib, 2000));
    r = vega.recover();
    CHECK(r.card_reset && pci.resets == 2 && vega.asic_resets() == 2);
    rig.card.gfx.hung = false;
    rig.card.gfx.reset_fails = false;
    memset(rig.doorbell_base, 0, 0x800 * 4);   // (a new life of the card starts with clean doorbells)
    CHECK(vega.bring_up(in));
    rig.card.gfx.hung = true;
    rig.card.gfx.reset_fails = true;
    CHECK(!vega.submit_graphics(ib, 2000));
    r = vega.recover();
    CHECK(r.failed && !r.card_reset && pci.resets == 2 && vega.asic_resets() == AmdVegaM::kMaxAsicResets);
  }
  {
    // By hand, on a healthy card: reset and brought up again.
    Rig rig;
    FakePci pci(rig);
    AmdVegaM vega(rig.alloc);
    AmdVegaM::Inputs in = rig.inputs();
    in.pci = &pci;
    CHECK(vega.bring_up(in));
    CHECK(vega.reset_card() && vega.up() && pci.resets == 1 && vega.submit_graphics(ib) && vega.submit_sdma(1, ib));
  }
}

// The POST after a card reset: the video BIOS's ASIC_Init table, run by the AtomBIOS interpreter, brings back what the reset took (here: the memory the card cannot be brought up without).
static std::vector<uint8_t> make_vbios(bool with_clocks = true) {
  using namespace atomimage;
  Image img;
  Code init;
  init.move(REG, 0x7777, PS, 0);       // the default engine clock, the first parameter: the marker the simulated card waits for
  init.move(REG, 0x7778, PS, 1);       // the default memory clock
  init.eot();
  img.tables.push_back({0, 0, 0, init});
  img.sclk = with_clocks ? 0x1234 : 0;
  img.mclk = 0x5678;
  return img.build();
}

static void test_post_after_reset() {
  const uint32_t nop[] = {0xC0001000};
  const ArrayView<const uint32_t> ib(nop, 1);
  const std::vector<uint8_t> vbios = make_vbios();
  auto setup = [&](Rig& rig, FakePci& pci, AmdVegaM::Inputs& in) {
    pci.needs_post = true;
    in = rig.inputs();
    in.pci = &pci;
    in.allow_asic_reset = true;
    // the simulated card is trained when the video BIOS writes the engine clock to 0x7777
    rig.card.on_marker = [&rig]() { rig.card.gmc.set_reg(0x150a, 64); rig.card.gmc.set_reg(0x809, (7u << 16) | 4u); };
  };
  {
    // Not POSTed after the reset, no video BIOS: down (as before).
    Rig rig;
    FakePci pci(rig);
    AmdVegaM::Inputs in;
    setup(rig, pci, in);
    AmdVegaM vega(rig.alloc);
    CHECK(vega.bring_up(in));
    rig.card.gfx.hung = true;
    rig.card.gfx.reset_fails = true;
    CHECK(!vega.submit_graphics(ib, 2000));
    const AmdVegaM::Recovery r = vega.recover();
    CHECK(r.card_reset && r.failed && !vega.up() && strstr(vega.status(), "after a card reset: "));
  }
  {
    // With the video BIOS: the card is reset, POSTed (the table gets the firmware info's clocks as its parameters), brought up, and works.
    Rig rig;
    FakePci pci(rig);
    AmdVegaM::Inputs in;
    setup(rig, pci, in);
    std::vector<uint8_t> callers_copy = vbios;
    in.vbios = ConstByteView(callers_copy.data(), callers_copy.size());
    AmdVegaM vega(rig.alloc);
    CHECK(vega.bring_up(in));
    std::fill(callers_copy.begin(), callers_copy.end(), 0xFF);   // (the caller's buffer is gone: the card has its own copy of the image)
    rig.card.gfx.hung = true;
    rig.card.gfx.reset_fails = true;
    CHECK(!vega.submit_graphics(ib, 2000));
    size_t marker_at = ~size_t{0}, clock_at = ~size_t{0};
    const AmdVegaM::Recovery r = vega.recover();
    CHECK(r.graphics && r.card_reset && !r.failed && vega.up() && pci.resets == 1);
    CHECK(vega.submit_graphics(ib) && vega.submit_compute(0, ib) && vega.submit_sdma(1, ib));
    // the POST wrote the clocks it was given (0x1234 engine, 0x5678 memory) to the registers the table names, after the reset and before the bring-up
    const auto& w = rig.card.gfx.writes;
    for (size_t i = 0; i < w.size(); ++i) {
      if (w[i].first == 0x7777 && w[i].second == 0x1234) marker_at = i;
      if (w[i].first == 0x7778 && w[i].second == 0x5678) clock_at = i;
    }
    CHECK(marker_at != ~size_t{0} && clock_at == marker_at + 1);
  }
  {
    // A video BIOS that is not an AtomBIOS image, or whose firmware info has no clocks: the card stays down and says why.
    for (int variant = 0; variant < 3; ++variant) {
      Rig rig;
      FakePci pci(rig);
      AmdVegaM::Inputs in;
      setup(rig, pci, in);
      std::vector<uint8_t> bad = variant == 0 ? std::vector<uint8_t>(vbios.size(), 0x5A) : variant == 1 ? make_vbios(false) : std::vector<uint8_t>(vbios.begin(), vbios.begin() + 0x200);
      in.vbios = ConstByteView(bad.data(), bad.size());
      AmdVegaM vega(rig.alloc);
      CHECK(vega.bring_up(in));
      rig.card.gfx.hung = true;
      rig.card.gfx.reset_fails = true;
      CHECK(!vega.submit_graphics(ib, 2000));
      const AmdVegaM::Recovery r = vega.recover();
      CHECK(r.card_reset && r.failed && !vega.up() && strstr(vega.status(), "after a card reset: POST: "));
      if (variant == 1) CHECK(strstr(vega.status(), "no default clocks"));
      else CHECK(strstr(vega.status(), "not an AtomBIOS image"));
    }
  }
  {
    // By hand, on a card that was not POSTed: post() alone, then the bring-up.
    Rig rig;
    FakePci pci(rig);
    AmdVegaM::Inputs in;
    setup(rig, pci, in);
    rig.card.gmc.set_reg(0x150a, 0);   // not trained
    rig.card.gmc.set_reg(0x809, 0);
    AmdVegaM vega(rig.alloc);
    CHECK(!vega.bring_up(in));
    in.vbios = ConstByteView(vbios.data(), vbios.size());
    CHECK(vega.post(in) && vega.bring_up(in) && vega.up());
    in.vbios = ConstByteView();
    CHECK(!vega.post(in) && strstr(vega.status(), "no video BIOS image"));
  }
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_plan();
  test_bring_up();
  test_transfer();
  test_failures();
  test_display();
  test_present();
  test_interrupts();
  test_interrupt_sink();
  test_msi_input();
  test_sq_messages();
  test_display_interrupts();
  test_vblank_interrupt();
  test_video_engine_interrupts();
  test_recovery();
  test_auto_recovery();
  test_recovery_failure();
  test_recovery_failure_sdma();
  test_card_reset();
  test_post_after_reset();
  printf(g_failed ? "AmdVegaMTest: %d FAILED\n" : "AmdVegaMTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
