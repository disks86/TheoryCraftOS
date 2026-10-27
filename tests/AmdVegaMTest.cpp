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
  static bool in(uint32_t r, uint32_t lo, uint32_t hi) { return r >= lo && r <= hi; }
  IVcnRegisters& owner(uint32_t r) {
    if (in(r, 0x3400, 0x37ff)) return sdma;
    if (in(r, 0xe30, 0xe4b) || r == 0x151a || r == 0x151b) return ih;
    if (in(r, 0x500, 0x57f) || in(r, 0x800, 0x82f) || in(r, 0xb00, 0xbff) || r == 0x394 || r == 0x1524 || r == 0x1528 || r == 0x150a) return gmc;
    if (in(r, 0x1a00, 0x1fff) || in(r, 0x4000, 0x47ff)) return dce;
    return gfx;
  }
  uint32_t read(uint32_t r) override { return owner(r).read(r); }
  void write(uint32_t r, uint32_t v) override { owner(r).write(r, v); }
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
  printf(g_failed ? "AmdVegaMTest: %d FAILED\n" : "AmdVegaMTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
