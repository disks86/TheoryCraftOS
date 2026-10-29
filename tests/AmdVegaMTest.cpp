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
#include "Hardware/GraphicsProcessingUnit/AMD/AmdVegaDraw.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnCp.hpp"
#include "FakeAmdDce11.hpp"
#include "FakeAuxSink.hpp"
#include "FakeAzalia.hpp"
#include "FakeHda.hpp"
#include "FakeI2cMonitor.hpp"
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
constexpr uint64_t kVideoMemory = 96ull << 20, kGpuBase = 64ull << 20;   // the memory controller puts video memory at 64 MiB of the GPU's address space   // (the memory BAR is at the simulated memory's own address: the OS runs identity mapped, and the firmware's framebuffer is reached through it)
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
  FakeI2cMonitor* monitor = nullptr;   // a DDC bus on the pad registers 0x7000..0x7007
  FakeAuxCard* aux = nullptr;          // an AUX channel on the registers 0x7010..0x7033
  FakeAzalia* azalia = nullptr;        // the audio endpoints on the registers 0x17a8..0x17c5
  static bool in(uint32_t r, uint32_t lo, uint32_t hi) { return r >= lo && r <= hi; }
  IVcnRegisters& owner(uint32_t r) {
    if (monitor && in(r, 0x7000, 0x7007)) return *monitor;
    if (aux && (in(r, 0x7010, 0x7061) || in(r, 0x7100, 0x718F))) return *aux;
    if (azalia && FakeAzalia::owns(r)) return *azalia;
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
    card.gmc.set_reg(0x150a, 96);
    card.gmc.set_reg(0x809, (9u << 16) | 4u);   // MC_VM_FB_LOCATION: base 4 * 16 MiB, top 7
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
    small.card.gmc.set_reg(0x809, (7u << 16) | 4u);
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
    CHECK(rig.card.gfx.reg(0x189a + 8 * p) == 0x10000000 && rig.card.gfx.reg(0x189c + 8 * p) == (50u | 10u << 20) && rig.card.gfx.reg(0x1899 + 8 * p) == (0x10000 | polarity | 1 | 0x1000000 | 0x100000));   // (and the short pulse of the pin on, its stale one acknowledged)
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
  rig.card.ih.post(42, 12, 0, 0, 0);
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

// The vertical line interrupt: the same rules as the vertical blank, on its own bit (LB_INTERRUPT_MASK bit 4, literal here), counted on its own.
static void test_vline_interrupt() {
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
  rig.card.dce.crtc(2, kGpuBase, width);
  fakedce::setup_aperture(rig.card.dce);
  rig.card.dce.set(0x80d, kGpuBase >> 12);
  rig.card.dce.set(0x80e, ((kGpuBase + kVideoMemory) >> 12) - 1);
  // not without a display, and nothing is written for it
  CHECK(!vega.set_vline_interrupt(true) && vega.set_vline_interrupt(false) && rig.card.dce.get(0x1ac8 + 0x400) == 0);
  ByteView memory;
  CHECK(vega.engage_display(fw, &memory) && vega.display_engaged());
  const AmdIrq& irq = *vega.irq();
  rig.card.dce.set(0x1ac8 + 0x400, 0x101);   // (the vblank bit and another are set already: kept)
  CHECK(vega.set_vline_interrupt(true) && rig.card.dce.get(0x1ac8 + 0x400) == 0x111 && rig.card.dce.get(0x1ac8) == 0 && rig.card.dce.get(0x1ac8 + 0x200) == 0);
  CHECK(vega.set_vline_interrupt(true) && rig.card.dce.get(0x1ac8 + 0x400) == 0x111);
  // The vertical blank bit is not touched by the line's, nor the line's by the blank's.
  CHECK(vega.set_vblank_interrupt(true) && rig.card.dce.get(0x1ac8 + 0x400) == 0x111);
  CHECK(vega.set_vblank_interrupt(false) && rig.card.dce.get(0x1ac8 + 0x400) == 0x110);
  CHECK(vega.set_vblank_interrupt(true) && rig.card.dce.get(0x1ac8 + 0x400) == 0x111);
  // A line interrupt of controller 2 (source 3, data 1) with the status (DISP_INTERRUPT_STATUS_CONTINUE2, bit 2) set: acknowledged (LB_VLINE_STATUS.VLINE_ACK), counted for the line only.
  rig.card.gfx.set_reg(0x1859, 0x4);
  rig.card.ih.post(3, 1, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.vline_count(2) == 1 && irq.vline_count(1) == 0 && irq.vblank_count(2) == 0 && rig.card.dce.get(0x1ac9 + 0x400) == 0x10);
  // The status does not say so: counted, not acknowledged.
  rig.card.dce.set(0x1ac9 + 0x400, 0);
  rig.card.gfx.set_reg(0x1859, 0);
  rig.card.ih.post(3, 1, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.vline_count(2) == 2 && rig.card.dce.get(0x1ac9 + 0x400) == 0 && irq.vline_count(6) == 0);
  // Off on its own, with the blank left on.
  CHECK(vega.set_vline_interrupt(false) && rig.card.dce.get(0x1ac8 + 0x400) == 0x101);
  rig.card.dce.set(0x1ac8 + 0x400, 0x111);   // (switched on behind its back: asking for off again has nothing to switch off)
  CHECK(vega.set_vline_interrupt(false) && rig.card.dce.get(0x1ac8 + 0x400) == 0x111);
  rig.card.dce.set(0x1ac8 + 0x400, 0x101);
  // Giving the display back or stopping the card switches it off.
  CHECK(vega.set_vline_interrupt(true) && (rig.card.dce.get(0x1ac8 + 0x400) & 0x10));
  vega.disengage_display();
  CHECK(!(rig.card.dce.get(0x1ac8 + 0x400) & 0x10) && !vega.set_vline_interrupt(true));
  CHECK(vega.engage_display(fw, &memory) && vega.set_vline_interrupt(true) && (rig.card.dce.get(0x1ac8 + 0x400) & 0x10));
  vega.shutdown();
  CHECK(!(rig.card.dce.get(0x1ac8 + 0x400) & 0x10));
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
    rig.card.on_marker = [&rig]() { rig.card.gmc.set_reg(0x150a, 96); rig.card.gmc.set_reg(0x809, (9u << 16) | 4u); };
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

// Hot plug events as the OS sees them: the state at bring-up, an event for each plug and unplug in order, a bounded queue, the pin that interrupts too often reported once.
static void test_hotplug_events() {
  Rig rig;
  rig.card.gfx.set_reg(0x1898 + 8, 0x2);        // pin 1 senses a display at bring-up
  rig.card.gfx.set_reg(0x1898 + 8 * 5, 0x2);    // so does pin 5, which is not watched
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.hpd_pins = 0x0F;
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  AmdVegaM::HotplugEvent e;
  CHECK(vega.connected_pins() == 0x2 && !vega.take_hotplug(&e) && vega.hotplug_events_dropped() == 0);
  // A display plugged in on pin 0 (asserted in DISP_INTERRUPT_STATUS bit 17, the pin senses it now), then pulled out of pin 1 (DISP_INTERRUPT_STATUS_CONTINUE).
  rig.card.gfx.set_reg(0x1857, 0x20000);
  rig.card.gfx.set_reg(0x1898, 0x2);
  rig.card.ih.post(42, 0, 0, 0, 0);
  rig.card.gfx.set_reg(0x1858, 0x20000);
  rig.card.gfx.set_reg(0x1898 + 8, 0x0);
  rig.card.ih.post(42, 1, 0, 0, 0);
  CHECK(vega.service_interrupts() == 2);
  CHECK(vega.connected_pins() == 0x1);
  CHECK(vega.take_hotplug(&e) && e.pin == 0 && e.connected && !e.throttled);
  CHECK(vega.take_hotplug(&e) && e.pin == 1 && !e.connected && !e.throttled);
  CHECK(!vega.take_hotplug(&e) && vega.take_hotplug(nullptr) == false);
  // An interrupt for a pin that is not asserted is not an event.
  rig.card.gfx.set_reg(0x1859, 0);
  rig.card.ih.post(42, 2, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && !vega.take_hotplug(&e) && vega.connected_pins() == 0x1);
  // More events than the queue holds: the oldest go, and the number lost is counted. Pins 0, 1 and 2 take turns (three does not divide the queue's sixteen), so the survivors can be told
  // from the first sixteen.
  rig.card.gfx.set_reg(0x1857, 0x20000);
  rig.card.gfx.set_reg(0x1858, 0x20000);
  rig.card.gfx.set_reg(0x1859, 0x20000);
  rig.card.gfx.set_reg(0x1898, 0x2);
  rig.card.gfx.set_reg(0x1898 + 8, 0x2);
  rig.card.gfx.set_reg(0x1898 + 16, 0x2);
  for (uint32_t i = 0; i < 20; ++i) rig.card.ih.post(42, i % 3, 0, 0, 0);
  CHECK(vega.service_interrupts(20) == 20);
  CHECK(vega.hotplug_events_dropped() == 4);
  for (uint32_t i = 4; i < 20; ++i) CHECK(vega.take_hotplug(&e) && e.pin == i % 3);
  CHECK(!vega.take_hotplug(&e));
  // The pin that will not stop: after AmdIrq::kHotplugStormLimit events it is switched off, and the event that does it says so, once.
  const uint32_t before = vega.irq()->hotplug_count(2);
  uint32_t throttled_events = 0, taken = 0;
  for (uint32_t i = 0; i < AmdIrq::kHotplugStormLimit + 10; ++i) {
    rig.card.ih.post(42, 2, 0, 0, 0);
    vega.service_interrupts();
    while (vega.take_hotplug(&e)) { ++taken; throttled_events += e.throttled; }
  }
  CHECK(taken == AmdIrq::kHotplugStormLimit + 10 && throttled_events == 1 && vega.irq()->hpd_throttled(2) && before + taken == vega.irq()->hotplug_count(2));
  // Shutting down forgets all of it; a new bring-up starts from the pins as they are.
  vega.shutdown();
  CHECK(vega.connected_pins() == 0 && !vega.take_hotplug(&e));
  memset(rig.doorbell_base, 0, 0x800 * 4);
  rig.card.gfx.set_reg(0x1898 + 8, 0x0);
  CHECK(vega.bring_up(in) && vega.connected_pins() == 0x1 + 0x4 && vega.hotplug_events_dropped() == 0);
  // (a new life of the card reports a pin that stops interrupting once more)
  throttled_events = 0;
  for (uint32_t i = 0; i < AmdIrq::kHotplugStormLimit + 10; ++i) {
    rig.card.ih.post(42, 2, 0, 0, 0);
    vega.service_interrupts();
    while (vega.take_hotplug(&e)) throttled_events += e.throttled;
  }
  CHECK(throttled_events == 1);
}

// The DisplayPort short pulse (source 42, data 6..11 for pins 0..5): switched on for the watched pins (DC_HPD_INT_CONTROL bit 24), pending in DC_HPD_INT_STATUS bit 8, acknowledged with
// DC_HPD_INT_CONTROL bit 20, counted per pin, not a hot plug; a pin that will not stop has it switched off; off at shutdown. (Register bits are literal.)
static void test_hpd_rx() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.hpd_pins = 0x0F;
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  const AmdIrq& irq = *vega.irq();
  for (uint32_t pin = 0; pin < 6; ++pin) CHECK(((rig.card.gfx.reg(0x1899 + 8 * pin) & 0x1000000) != 0) == (pin < 4));
  // The classification: data 6..11 are the pulses of pins 0..5, 0..5 the plugs, the rest nothing.
  for (uint32_t data = 0; data < 14; ++data) {
    AmdIh3::Vector v;
    v.src_id = 42;
    v.src_data = data;
    const AmdIrq::Event e = AmdIrq::classify(v);
    if (data < 6) CHECK(e.kind == AmdIrq::Kind::DisplayHotplug && e.pin == data);
    else if (data < 12) CHECK(e.kind == AmdIrq::Kind::DisplayHpdRx && e.pin == data - 6);
    else CHECK(e.kind == AmdIrq::Kind::Unknown);
  }
  for (uint32_t pin = 0; pin < 4; ++pin) rig.card.gfx.set_reg(0x1899 + 8 * pin, rig.card.gfx.reg(0x1899 + 8 * pin) & ~0x100000u);   // (the stale acknowledge of bring-up is not what is looked for)
  // A pulse on pin 2 with the status bit set: acknowledged (bit 20 of that pin's control register only), counted for that pin, no hot plug event.
  rig.card.gfx.set_reg(0x1898 + 16, 0x100);
  rig.card.ih.post(42, 8, 0, 0, 0);
  AmdVegaM::HotplugEvent e;
  CHECK(vega.service_interrupts() == 1 && irq.hpd_rx_count(2) == 1 && irq.count(AmdIrq::Kind::DisplayHpdRx) == 1 && !vega.take_hotplug(&e));
  CHECK((rig.card.gfx.reg(0x1899 + 16) & 0x100000) && !(rig.card.gfx.reg(0x1899 + 8) & 0x100000) && !(rig.card.gfx.reg(0x1899) & 0x100000));
  CHECK(irq.hpd_rx_count(0) == 0 && irq.hpd_rx_count(1) == 0 && irq.hotplug_count(2) == 0 && vega.connected_pins() == 0);
  // The status bit is not set: nothing is counted or acknowledged.
  rig.card.gfx.set_reg(0x1898 + 8, 0);
  rig.card.ih.post(42, 7, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.hpd_rx_count(1) == 0 && !(rig.card.gfx.reg(0x1899 + 8) & 0x100000));
  // A plug on the same pin is still a plug (it needs the other status bit), and the short pulse's bit does not make it one.
  rig.card.ih.post(42, 2, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.hotplug_count(2) == 0 && !vega.take_hotplug(&e));
  // A pulse on a pin that does not exist is nothing (data 12 and up).
  rig.card.ih.post(42, 12, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && irq.count(AmdIrq::Kind::DisplayHpdRx) == 1);
  // The pin that will not stop: switched off after kHpdRxStormLimit pulses, once, the others untouched; the count stops there.
  rig.card.gfx.set_reg(0x1898 + 24, 0x100);
  for (uint32_t i = 0; i < AmdIrq::kHpdRxStormLimit; ++i) {
    rig.card.ih.post(42, 9, 0, 0, 0);
    vega.service_interrupts();
  }
  CHECK(!irq.hpd_rx_throttled(3) && (rig.card.gfx.reg(0x1899 + 24) & 0x1000000));   // (the limit itself is allowed)
  rig.card.ih.post(42, 9, 0, 0, 0);
  vega.service_interrupts();
  CHECK(irq.hpd_rx_throttled(3) && !(rig.card.gfx.reg(0x1899 + 24) & 0x1000000));
  for (uint32_t i = 0; i < 9; ++i) {
    rig.card.ih.post(42, 9, 0, 0, 0);
    vega.service_interrupts();
  }
  CHECK(irq.hpd_rx_throttled(3) && !irq.hpd_rx_throttled(2) && !(rig.card.gfx.reg(0x1899 + 24) & 0x1000000) && (rig.card.gfx.reg(0x1899 + 16) & 0x1000000));
  CHECK(irq.hpd_rx_count(3) == AmdIrq::kHpdRxStormLimit + 10);
  // Shutting down switches the rest off.
  vega.shutdown();
  for (uint32_t pin = 0; pin < 4; ++pin) CHECK(!(rig.card.gfx.reg(0x1899 + 8 * pin) & 0x1000000));
}

// A video BIOS image with the object table of a board with eDP on pin 0, HDMI on pin 1 and a DisplayPort on pin 2 (literal offsets: ROM header 0x100, master data table 0x200 with the
// GPIO_Pin_LUT at 0x300 and the Object_Header at 0x400, the paths at 0x440, the connector objects at 0x500, their records at 0x580).
static std::vector<uint8_t> make_connector_vbios() {
  std::vector<uint8_t> b(0xB00, 0);      // (the mode setting tables are at 0x800..0xA40)
  auto u8 = [&](uint32_t at, uint32_t v) { b[at] = static_cast<uint8_t>(v); };
  auto u16 = [&](uint32_t at, uint32_t v) { u8(at, v); u8(at + 1, v >> 8); };
  u8(0, 0x55); u8(1, 0xAA); u16(0x48, 0x100);
  memcpy(&b[0x104], "ATOM", 4);
  memcpy(&b[0x30], " 761295520", 10);   // (the ATI signature the interpreter checks)
  u16(0x120, 0x200);
  u16(0x11E, 0x640);                // the command tables (master list at 0x640): ProcessAuxChannelTransaction, table 78, at 0x700
  u16(0x200 + 4 + 23 * 2, 0x5C0);   // the indirect I/O programs: none
  u16(0x200 + 4 + 10 * 2, 0x600);   // GPIO_I2C_Info
  u16(0x200 + 4 + 12 * 2, 0x300);
  u16(0x200 + 4 + 22 * 2, 0x400);
  u16(0x300, 4 + 3 * 4);
  const uint32_t shifts[3] = {0, 8, 16};
  for (uint32_t i = 0; i < 3; ++i) { u16(0x304 + i * 4, 0x488d); u8(0x306 + i * 4, shifts[i]); u8(0x307 + i * 4, i + 1); }
  u16(0x400, 0x20); u8(0x403, 2); u16(0x404, 0x0FFF); u16(0x406, 0x100); u16(0x40E, 0x40);
  const uint32_t tags[3] = {0x2, 0x8, 0x80}, ids[3] = {0x1114, 0x110C, 0x1113};   // LCD1 eDP, DFP1 HDMI-A, DFP2 DisplayPort
  u8(0x440, 3);
  const uint32_t encoders[3] = {0x211E, 0x2120, 0x2220};   // the display paths' encoders: UNIPHY link A (eDP), UNIPHY1 link A (HDMI-A), UNIPHY1 link B (DisplayPort)
  for (uint32_t i = 0; i < 3; ++i) { u16(0x444 + i * 12, tags[i]); u16(0x446 + i * 12, 12); u16(0x448 + i * 12, ids[i]); u16(0x44C + i * 12, encoders[i]); }
  u8(0x500, 3);
  const uint32_t ddc[3] = {0x92, 0x91, 0x93};
  for (uint32_t i = 0; i < 3; ++i) {
    u16(0x504 + i * 8, ids[i]); u16(0x508 + i * 8, 0x180 + i * 10);
    u8(0x580 + i * 10, 1); u8(0x581 + i * 10, 4); u8(0x582 + i * 10, ddc[i]); u8(0x583 + i * 10, 0);                  // the I2C record: the DDC line
    u8(0x584 + i * 10, 2); u8(0x585 + i * 10, 4); u8(0x586 + i * 10, i + 1); u8(0x587 + i * 10, 1);                  // the hot plug record
  }
  // GPIO_I2C_Info: three entries (27 bytes) for the lines 0x92, 0x91 and 0x93, all on the same pad registers (0x7000.., the simulated bus) and bits
  u16(0x600, 4 + 3 * 27);
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t at = 0x604 + i * 27;
    const uint32_t regs[8] = {0x7000, 0x7001, 0x7002, 0x7003, 0x7004, 0x7005, 0x7006, 0x7007};   // clock: mask, enable, readback, output; data: the same
    const uint8_t bits[8] = {3, 9, 13, 17, 5, 11, 15, 19};
    for (int k = 0; k < 8; ++k) { u16(at + k * 2, regs[k]); u8(at + 17 + k, bits[k]); }
    u8(at + 16, ddc[i]);
  }
  // ProcessAuxChannelTransaction: the parameters and the request (scratch words 1 to 5) to the card's registers, the doorbell, then the reply (scratch words 5 to 8, the status and the
  // length into the second parameter word) back from them; 8 bytes of parameters (PS 0 and 1), as the real table
  atomimage::Code aux;
  aux.move(atomimage::REG, FakeAuxCard::kParams1, atomimage::PS, 1);
  for (uint32_t w = 1; w <= 5; ++w) aux.move(atomimage::REG, FakeAuxCard::kRequest + w, atomimage::FB, w);
  aux.move(atomimage::REG, FakeAuxCard::kDoorbell, atomimage::PS, 0);
  for (uint32_t k = 0; k < 4; ++k) aux.move(atomimage::FB, 5 + k, atomimage::REG, FakeAuxCard::kData + k);
  aux.move(atomimage::PS, 1, atomimage::REG, FakeAuxCard::kStatus, atomimage::BYTE0, 1);
  aux.move(atomimage::PS, 1, atomimage::REG, FakeAuxCard::kLength, atomimage::BYTE0, 2);
  aux.eot();
  // UNIPHYTransmitterControl (76), revision 1.5: the parameters (12 bytes) to the card; the first word is the panel power's action (in its top byte as in the V4 layout) and the training's
  // SETUP_VSEMPH. DIG1EncoderControl (74) and DIG2EncoderControl (75), revision 1.4: 8 bytes of parameters each to their registers.
  atomimage::Code panel;
  panel.move(atomimage::REG, FakeAuxCard::kPanel, atomimage::PS, 0);
  panel.move(atomimage::REG, FakeAuxCard::kPanel + 1, atomimage::PS, 1);
  panel.move(atomimage::REG, FakeAuxCard::kPanel + 2, atomimage::PS, 2);
  panel.eot();
  u16(0x640 + 4 + 76 * 2, 0x780);
  u16(0x780, 6 + static_cast<uint32_t>(panel.b.size()));
  u8(0x782, 1);
  u8(0x783, 5);
  u8(0x784, 12);
  u8(0x785, 12);
  for (size_t i = 0; i < panel.b.size(); ++i) u8(0x786 + static_cast<uint32_t>(i), panel.b[i]);
  {
    atomimage::Code enc;     // DIGxEncoderControl (4), revision 1.5: 12 bytes of parameters (three words) to the card
    for (uint32_t i = 0; i < 3; ++i) enc.move(atomimage::REG, 0x7050 + i, atomimage::PS, i);
    enc.eot();
    const uint32_t at = 0x7A0;
    u16(0x640 + 4 + 4 * 2, at);
    u16(at, 6 + static_cast<uint32_t>(enc.b.size()));
    u8(at + 2, 1);
    u8(at + 3, 5);
    u8(at + 4, 12);
    u8(at + 5, 12);
    for (size_t i = 0; i < enc.b.size(); ++i) u8(at + 6 + static_cast<uint32_t>(i), enc.b[i]);
  }
  // the mode setting tables: each copies its parameter words to FakeAuxCard::kModeSet registers (crev: SetPixelClock 7, SelectCRTC_Source 3, the others 1)
  const struct { uint32_t index, words, crev; } mode_tables[9] = {{12, 4, 7}, {34, 2, 1}, {35, 1, 1}, {49, 6, 1}, {42, 1, 3}, {44, 1, 1}, {13, 1, 1}, {40, 3, 1}, {33, 1, 1}};
  for (uint32_t k = 0; k < 9; ++k) {
    atomimage::Code m;
    for (uint32_t i = 0; i < mode_tables[k].words; ++i) m.move(atomimage::REG, FakeAuxCard::kModeSet + k * 0x10 + i, atomimage::PS, i);
    m.eot();
    const uint32_t at = 0x800 + k * 0x40;
    u16(0x640 + 4 + mode_tables[k].index * 2, at);
    u16(at, 6 + static_cast<uint32_t>(m.b.size()));
    u8(at + 2, 1);
    u8(at + 3, mode_tables[k].crev);
    u8(at + 4, mode_tables[k].words * 4);
    u8(at + 5, mode_tables[k].words * 4);
    for (size_t i = 0; i < m.b.size(); ++i) u8(at + 6 + static_cast<uint32_t>(i), m.b[i]);
  }
  u16(0x640 + 4 + 78 * 2, 0x700);
  u16(0x700, 6 + static_cast<uint32_t>(aux.b.size()));
  u8(0x704, 8);
  u8(0x705, 8);
  for (size_t i = 0; i < aux.b.size(); ++i) u8(0x706 + static_cast<uint32_t>(i), aux.b[i]);
  return b;
}

// The EDID of a monitor over the connector's DDC lines.
static void test_read_edid() {
  const std::vector<uint8_t> vbios = make_connector_vbios();   // eDP (DDC line 0x92), HDMI-A (0x91), DisplayPort (0x93)
  Rig rig;
  FakeI2cMonitor monitor;
  FakeAuxCard auxcard;
  monitor.edid = make_edid(1);
  rig.card.monitor = &monitor;
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.vbios = ConstByteView(vbios.data(), vbios.size());
  ByteBuffer out(rig.alloc);
  AtomConnectors::Connector hdmi, edp, dp;
  CHECK(!vega.read_edid(hdmi, &out) && monitor.writes == 0);   // (not up)
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  CHECK(vega.connectors().size() == 3);
  edp = vega.connectors()[0];
  hdmi = vega.connectors()[1];
  dp = vega.connectors()[2];
  CHECK(edp.has_ddc && edp.ddc_id == 0x92 && hdmi.has_ddc && hdmi.ddc_id == 0x91 && dp.has_ddc && dp.ddc_id == 0x93);
  // HDMI: read over the pads of the bus the video BIOS names
  CHECK(vega.read_edid(hdmi, &out) && out.size() == 256 && memcmp(out.view().data(), monitor.edid.data(), 256) == 0);
  // DisplayPort and eDP: I2C over the AUX channel, through the video BIOS's table; the DDC pads are not touched, and the table is told the connector's line and hot plug pin
  const uint32_t writes = monitor.writes;
  CHECK(!vega.read_edid(dp, &out) && !vega.read_edid(edp, &out) && auxcard.doorbells == 0);   // (no sink on the AUX registers yet: the table's reads give zeros)
  rig.card.aux = &auxcard;
  auxcard.sink.edid = make_edid(1);
  auxcard.sink.edid[20] ^= 0x55;     // (a different monitor, so that the answers can be told apart)
  uint8_t sum = 0;
  for (size_t i = 0; i < 127; ++i) sum = static_cast<uint8_t>(sum + auxcard.sink.edid[i]);
  auxcard.sink.edid[127] = static_cast<uint8_t>(0 - sum);
  CHECK(vega.read_edid(dp, &out) && out.size() == 256 && memcmp(out.view().data(), auxcard.sink.edid.data(), 256) == 0);
  CHECK(auxcard.panel_writes == 0);       // (a DisplayPort connector has no panel to power)
  CHECK(!auxcard.sink.requests.empty() && auxcard.sink.requests.front().channel == 0x93 && auxcard.sink.requests.front().hpd == 2);
  auxcard.sink.requests.clear();
  CHECK(vega.read_edid(edp, &out) && out.size() == 256 && memcmp(out.view().data(), auxcard.sink.edid.data(), 256) == 0);
  CHECK(!auxcard.sink.requests.empty() && auxcard.sink.requests.front().channel == 0x92 && auxcard.sink.requests.front().hpd == 0);
  CHECK(auxcard.panel_writes == 1 && auxcard.panel_params == 0x0C000000u && auxcard.panel_on);     // powered up (ucAction = POWER_ON) before the first AUX access
  // The panel is off and does not answer on the AUX channel until the table powers it up; the hot plug pin of the connector (pin 0) shows it
  auxcard.panel_on = false;
  auxcard.unpowered_channel = 0x92;
  auxcard.sink.requests.clear();
  rig.card.gfx.set_reg(0x1898, 0x2);
  uint64_t began = rig.clock.now;
  CHECK(vega.read_edid(edp, &out) && out.size() == 256 && auxcard.panel_on && auxcard.panel_writes == 2);
  CHECK(rig.clock.now - began < 100000);       // (no wait: the connector's pin shows the panel)
  CHECK(!auxcard.sink.requests.empty());
  // the pin never shows the panel: the wait runs out (300 sense reads), the AUX channel is tried anyway
  auxcard.panel_on = false;
  rig.card.gfx.set_reg(0x1898, 0);
  began = rig.clock.now;
  CHECK(vega.read_edid(edp, &out) && auxcard.panel_on && auxcard.panel_writes == 3);
  CHECK(rig.clock.now - began >= 300000);      // (the 300 ms)
  auxcard.unpowered_channel = 0;
  CHECK(auxcard.sink.malformed == 0 && auxcard.sink.outside_transaction == 0 && monitor.writes == writes);
  auxcard.sink.monitor_present = false;
  CHECK(!vega.read_edid(dp, &out));
  auxcard.sink.monitor_present = true;
  CHECK(vega.read_edid(dp, &out));
  // no DDC line in the connector, or a line the GPIO table does not have
  AtomConnectors::Connector no_ddc = hdmi;
  no_ddc.has_ddc = false;
  CHECK(!vega.read_edid(no_ddc, &out) && monitor.writes == writes);
  AtomConnectors::Connector unknown = hdmi;
  unknown.ddc_id = 0x77;
  CHECK(!vega.read_edid(unknown, &out) && monitor.writes == writes);
  AtomConnectors::Connector unknown_dp = dp;
  unknown_dp.ddc_id = 0x77;
  const size_t aux_requests = auxcard.sink.requests.size();
  CHECK(!vega.read_edid(unknown_dp, &out) && auxcard.sink.requests.size() == aux_requests);
  // nothing connected: nothing answers
  monitor.present = false;
  CHECK(!vega.read_edid(hdmi, &out));
  monitor.present = true;
  // stopped: no
  vega.shutdown();
  CHECK(!vega.read_edid(hdmi, &out));
}

// AFMT_AUDIO_SRC_CONTROL: each audio format block (one per DIG encoder, 0 to 5 on this card; registers 0x4a45, 0x4b45, 0x4c45, 0x4d45, 0x4e45, 0x4f45, the field is bits 2:0) takes its audio from
// the first connected endpoint; written for the connectors of the video BIOS's table that carry audio and have a display. (Register numbers are literal.)
static void test_audio_select() {
  const std::vector<uint8_t> vbios = make_connector_vbios();   // eDP: DIG 0 (no audio block), HDMI-A: DIG 2, DisplayPort: DIG 3; on pins 0, 1, 2
  const uint32_t afmt[6] = {0x4a45, 0x4b45, 0x4c45, 0x4d45, 0x4e45, 0x4f45};
  Rig rig;
  // endpoints 2, 3 and 5 have a port
  const uint32_t connectivity[8] = {1, 1, 0, 3, 1, 0, 1, 1};
  for (uint32_t p = 0; p < 8; ++p) rig.card.gfx.set_reg(0x17a9 + 4 * p, (connectivity[p] << 30) | 0x00123456u);
  for (uint32_t pin = 0; pin < 3; ++pin) rig.card.gfx.set_reg(0x1898 + 8 * pin, 0x2);   // displays on pins 0, 1 and 2
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.hpd_pins = 0x0F;
  in.vbios = ConstByteView(vbios.data(), vbios.size());
  const auto reset = [&] { for (uint32_t b = 0; b < 6; ++b) rig.card.gfx.set_reg(afmt[b], 0xAB000005u + b * 0x10); };
  reset();
  CHECK(!vega.select_audio_pin(AtomConnectors::Connector()) && vega.select_audio_pins() == 0);        // (not up)
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  CHECK(vega.audio_pins_connected() == 0x2C);
  const AtomConnectors::Connector edp = vega.connectors()[0], hdmi = vega.connectors()[1], dp = vega.connectors()[2];
  // one connector: only the field of its block changes, to the first connected endpoint (2); the others are as they were
  CHECK(vega.select_audio_pin(hdmi));
  CHECK(rig.card.gfx.reg(0x4c45) == 0xAB000022u && rig.card.gfx.reg(afmt[0]) == 0xAB000005u && rig.card.gfx.reg(afmt[1]) == 0xAB000015u && rig.card.gfx.reg(afmt[3]) == 0xAB000035u);
  CHECK(!vega.select_audio_pin(edp) && rig.card.gfx.reg(afmt[0]) == 0xAB000005u);       // (the internal panel has no audio block)
  reset();
  CHECK(vega.select_audio_pin(dp) && rig.card.gfx.reg(afmt[3]) == (0xAB000035u & ~7u) + 2 && rig.card.gfx.reg(afmt[2]) == 0xAB000025u);
  // all of them: the HDMI and the DisplayPort, not the eDP
  reset();
  CHECK(vega.select_audio_pins() == 2);
  for (uint32_t b = 0; b < 6; ++b) CHECK(rig.card.gfx.reg(afmt[b]) == ((b == 2 || b == 3) ? ((0xAB000005u + b * 0x10) & ~7u) + 2 : 0xAB000005u + b * 0x10));
  // the endpoints are read again each time: endpoint 2 loses its port, 3 is the first
  rig.card.gfx.set_reg(0x17a9 + 4 * 2, 0x40000000);
  CHECK(vega.select_audio_pin(hdmi) && (rig.card.gfx.reg(afmt[2]) & 7) == 3 && vega.audio_pins_connected() == 0x28);
  // the display on the HDMI goes away: only the DisplayPort is selected
  rig.card.gfx.set_reg(0x1857 + 1, 0x20000);
  rig.card.gfx.set_reg(0x1898 + 8, 0);
  rig.card.ih.post(42, 1, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1);
  reset();
  CHECK(vega.select_audio_pins() == 1 && rig.card.gfx.reg(afmt[2]) == 0xAB000025u && (rig.card.gfx.reg(afmt[3]) & 7) == 3);
  // a DIG without a block (UNIPHY3 is DIG 6), a connector with no encoder, a type with no audio: nothing written
  reset();
  AtomConnectors::Connector dig6 = hdmi;
  dig6.encoder_object = 0x2125;
  AtomConnectors::Connector bare = hdmi;
  bare.encoder_object = 0;
  AtomConnectors::Connector lcd = hdmi;
  lcd.devices = 0x2;
  CHECK(!vega.select_audio_pin(dig6) && !vega.select_audio_pin(bare) && !vega.select_audio_pin(lcd) && !vega.select_audio_pin(edp));
  uint32_t pin = 0;
  CHECK(!vega.audio_pin_for(dig6, &pin));
  for (uint32_t b = 0; b < 6; ++b) CHECK(rig.card.gfx.reg(afmt[b]) == 0xAB000005u + b * 0x10);       // (none of the blocks was written)
  for (uint32_t b = 0; b < 6; ++b) rig.card.gfx.set_reg(0x17a9 + 4 * b, 0x40000000);
  CHECK(!vega.select_audio_pin(dp) && vega.select_audio_pins() == 0);          // (no endpoint at all)
  for (uint32_t b = 0; b < 6; ++b) CHECK(rig.card.gfx.reg(afmt[b]) == 0xAB000005u + b * 0x10);
  // the blocks themselves
  amd_dce11::AudioFormat blocks(*rig.inputs().registers);
  reset();
  for (uint32_t b = 0; b < 6; ++b) {
    for (uint32_t p = 0; p < 8; ++p) {
      CHECK(blocks.select_pin(b, p) && rig.card.gfx.reg(afmt[b]) == ((0xAB000005u + b * 0x10) & ~7u) + p && blocks.selected_pin(b) == p);
    }
  }
  const size_t before = rig.card.gfx.writes.size();
  CHECK(!blocks.select_pin(6, 0) && !blocks.select_pin(8, 0) && !blocks.select_pin(0xFFFFFFFFu, 0) && !blocks.select_pin(0, 8) && !blocks.select_pin(0, 0xFFFFFFFFu) && rig.card.gfx.writes.size() == before);
  CHECK(blocks.selected_pin(6) == amd_dce11::AudioFormat::kNoPin && blocks.selected_pin(0xFFFFFFFFu) == amd_dce11::AudioFormat::kNoPin);
  rig.card.gfx.set_reg(afmt[1], 0xFFFFFFFFu);
  CHECK(blocks.selected_pin(1) == 7 && blocks.select_pin(1, 0) && rig.card.gfx.reg(afmt[1]) == 0xFFFFFFF8u);
  vega.shutdown();
  CHECK(!vega.select_audio_pin(hdmi) && vega.select_audio_pins() == 0);
}

// Enabling an audio endpoint for a connector: dce_v11_0_afmt_setmode's audio part. The endpoint's indexed registers (HOT_PLUG_CONTROL 0x54, CHANNEL_SPEAKER 0x25, the descriptors 0x28.., the lip
// sync 0x37) are literal, the values are worked out by hand from the EDID below.
static void test_audio_enable() {
  const std::vector<uint8_t> vbios = make_connector_vbios();   // eDP: DIG 0 (no audio block), HDMI-A: DIG 2, DisplayPort: DIG 3; on pins 0, 1, 2
  // A monitor: PCM stereo (32/44.1/48 kHz, 16/20/24 bit), AC-3 6 channels, DTS 8 channels, PCM 8 channels (48/96/192 kHz); speakers 0x05; the HDMI block with latencies 30/40 progressive, 50/60 interlaced
  const uint8_t cea[] = {0x2C, 0x09, 0x07, 0x07, 0x15, 0x07, 0x50, 0x3F, 0x1E, 0x18, 0x0F, 0x54, 0x07,
                         0x83, 0x05, 0x00, 0x00,
                         0x6C, 0x03, 0x0C, 0x00, 0x10, 0x00, 0x80, 0x00, 0xC0, 30, 40, 50, 60};
  std::vector<uint8_t> edid = make_edid(1);
  for (size_t i = 3; i < 127; ++i) edid[128 + i] = 0;
  edid[128 + 2] = 4 + sizeof cea;
  for (size_t i = 0; i < sizeof cea; ++i) edid[128 + 4 + i] = cea[i];
  uint8_t sum = 0;
  for (size_t i = 0; i < 127; ++i) sum = static_cast<uint8_t>(sum + edid[128 + i]);
  edid[255] = static_cast<uint8_t>(0 - sum);
  Rig rig;
  FakeAzalia azalia;
  FakeI2cMonitor monitor;
  monitor.edid = edid;
  rig.card.monitor = &monitor;
  rig.card.azalia = &azalia;
  azalia.connectivity[2] = 0;        // endpoints 2 and 3 have a port
  azalia.connectivity[3] = 3;
  for (uint32_t pin = 0; pin < 3; ++pin) rig.card.gfx.set_reg(0x1898 + 8 * pin, 0x2);
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.hpd_pins = 0x0F;
  in.vbios = ConstByteView(vbios.data(), vbios.size());
  const AtomConnectors::Connector none;
  CHECK(!vega.enable_audio(none, ConstByteView(edid.data(), edid.size())) && !vega.enable_audio(none) && !vega.disable_audio(none));       // (not up)
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  const AtomConnectors::Connector edp = vega.connectors()[0], hdmi = vega.connectors()[1], dp = vega.connectors()[2];
  azalia.writes.clear();
  rig.card.gfx.set_reg(0x4c45, 0xAB000005u);
  azalia.set(2, 0x25, 0xFFFFFFFFu);
  CHECK(vega.enable_audio(hdmi, ConstByteView(edid.data(), edid.size())));
  // the order of the writes: disabled, speakers, (the block selects the endpoint), the descriptors, the lip sync, enabled
  const uint32_t order[] = {0x54, 0x25, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x31, 0x32, 0x33, 0x35, 0x37, 0x54};
  CHECK(azalia.writes.size() == 16);
  for (size_t i = 0; i < azalia.writes.size() && i < 16; ++i) CHECK(azalia.writes[i].pin == 2 && azalia.writes[i].index == order[i]);
  CHECK(azalia.writes[0].value == 0 && azalia.writes[15].value == 0x80000000u);
  CHECK(azalia.value(2, 0x25) == 0xFFFDFF85u);          // the DisplayPort bit cleared, the HDMI bit set, the speakers 5
  CHECK(azalia.value(2, 0x28) == 0x57075407u);          // PCM: the 8 channel descriptor (7, 0x54 Hz bits, byte 2 0x07), the stereo rates of both (0x07 | 0x54)
  CHECK(azalia.value(2, 0x29) == 0x00500705u);          // AC-3
  CHECK(azalia.value(2, 0x2e) == 0x00181E07u);          // DTS
  for (uint32_t reg : {0x2a, 0x2b, 0x2c, 0x2d, 0x2f, 0x31, 0x32, 0x33, 0x35}) CHECK(azalia.value(2, reg) == 0);
  CHECK(azalia.value(2, 0x37) == 0x281Eu);              // lip sync, progressive: video 30, audio 40
  CHECK(azalia.value(2, 0x54) == 0x80000000u && (rig.card.gfx.reg(0x4c45) & 7) == 2 && (rig.card.gfx.reg(0x4c45) & ~7u) == 0xAB000000u);      // enabled; the block takes endpoint 2
  // interlaced: the other latencies
  AmdVegaM::AudioMode interlaced;
  interlaced.interlaced = true;
  CHECK(vega.enable_audio(hdmi, ConstByteView(edid.data(), edid.size()), interlaced) && azalia.value(2, 0x37) == 0x3C32u);
  // the EDID read over the connector's DDC lines
  azalia.writes.clear();
  CHECK(vega.enable_audio(hdmi) && azalia.writes.size() == 16 && azalia.value(2, 0x29) == 0x00500705u);
  CHECK(vega.enable_audio(hdmi, interlaced) && azalia.value(2, 0x37) == 0x3C32u);
  monitor.present = false;
  azalia.writes.clear();
  CHECK(!vega.enable_audio(hdmi) && azalia.writes.empty());          // (no EDID, nothing touched)
  monitor.present = true;
  // a monitor without a CTA extension: stereo speakers, no descriptors written (the old ones stay), no latency
  const std::vector<uint8_t> plain = make_edid(0);
  azalia.writes.clear();
  azalia.set(2, 0x29, 0x1234);
  CHECK(vega.enable_audio(hdmi, ConstByteView(plain.data(), plain.size())));
  CHECK(azalia.writes.size() == 4 && azalia.writes[0].index == 0x54 && azalia.writes[1].index == 0x25 && azalia.writes[2].index == 0x37 && azalia.writes[3].index == 0x54);
  CHECK((azalia.value(2, 0x25) & 0x7F) == 5 && azalia.value(2, 0x29) == 0x1234 && azalia.value(2, 0x37) == 0 && azalia.value(2, 0x54) == 0x80000000u);
  CHECK(vega.enable_audio(hdmi, ConstByteView()) && azalia.writes.size() == 8);        // (and an empty one, as a driver without an EDID)
  // the DisplayPort connector takes the same endpoint with its own block; the eDP panel has none: nothing written
  azalia.writes.clear();
  rig.card.gfx.set_reg(0x4d45, 0xAB000015u);
  CHECK(vega.enable_audio(dp, ConstByteView(edid.data(), edid.size())) && azalia.writes.size() == 16 && (rig.card.gfx.reg(0x4d45) & 7) == 2);
  azalia.writes.clear();
  CHECK(!vega.enable_audio(edp, ConstByteView(edid.data(), edid.size())) && azalia.writes.empty());
  // the first connected endpoint is read again: endpoint 2 loses its port, 3 takes over
  azalia.connectivity[2] = 1;
  azalia.writes.clear();
  CHECK(vega.enable_audio(hdmi, ConstByteView(edid.data(), edid.size())) && azalia.writes.size() == 16 && azalia.writes[0].pin == 3 && azalia.value(3, 0x54) == 0x80000000u && (rig.card.gfx.reg(0x4c45) & 7) == 3);
  // disabling: the endpoint the block has selected, whatever it is; a connector with no block
  azalia.writes.clear();
  CHECK(vega.disable_audio(hdmi) && azalia.writes.size() == 1 && azalia.writes[0].pin == 3 && azalia.writes[0].index == 0x54 && azalia.writes[0].value == 0 && azalia.value(3, 0x54) == 0);
  rig.card.gfx.set_reg(0x4c45, 0xAB000007u);
  azalia.writes.clear();
  CHECK(vega.disable_audio(hdmi) && azalia.writes.size() == 1 && azalia.writes[0].pin == 7);
  azalia.writes.clear();
  CHECK(!vega.disable_audio(edp) && azalia.writes.empty());
  // no endpoint connected: nothing written
  for (uint32_t p = 0; p < 8; ++p) azalia.connectivity[p] = 1;
  azalia.writes.clear();
  CHECK(!vega.enable_audio(hdmi, ConstByteView(edid.data(), edid.size())) && azalia.writes.empty());
  // the endpoint registers themselves: a pin that does not exist is refused and nothing is written
  amd_dce11::AudioPins pins(*rig.inputs().registers);
  EdidAudio::Info info;
  EdidAudio::parse(ConstByteView(edid.data(), edid.size()), &info);
  azalia.writes.clear();
  CHECK(!pins.endpoint_write(8, 0x54, 1) && !pins.set_enabled(8, true) && !pins.write_speaker_allocation(8, info) && !pins.write_sads(8, info) && !pins.write_latency(8, info, false) && azalia.writes.empty());
  CHECK(pins.set_enabled(5, true) && pins.enabled(5) && pins.set_enabled(5, false) && !pins.enabled(5) && !pins.enabled(8));
  azalia.set(5, 0x54, 0x7FFFFFFFu);
  CHECK(!pins.enabled(5));        // (only the enabled bit)
  EdidAudio::Info empty;
  azalia.writes.clear();
  CHECK(!pins.write_sads(5, empty) && azalia.writes.empty());
  // the descriptors by the driver's rules: the descriptor with the most channels of a format, the first of the formats other than PCM, the PCM rates ORed
  EdidAudio::Info rules;
  rules.sad_count = 5;
  rules.sads[0] = {2, 1, 0x01, 0x11};      // AC-3 2 channels
  rules.sads[1] = {2, 5, 0x02, 0x22};      // AC-3 6 channels: not looked at, the first one is the one
  rules.sads[2] = {1, 3, 0x10, 0x01};      // PCM 4 channels
  rules.sads[3] = {1, 3, 0x20, 0x02};      // PCM 4 channels again: not more channels, so the first stays, but the rates are ORed
  rules.sads[4] = {1, 1, 0x40, 0x04};      // PCM 2 channels
  CHECK(pins.write_sads(6, rules));
  CHECK(azalia.value(6, 0x29) == (1u | 0x01u << 8 | 0x11u << 16) && azalia.value(6, 0x28) == (3u | 0x10u << 8 | 0x01u << 16 | 0x70u << 24));
  // each audio coding type goes to its own register (descriptors 0 to 7, 9, 10, 11 and 13: PCM 1, AC-3 2, MPEG-1 3, MP3 4, MPEG-2 5, AAC LC 6, DTS 7, ATRAC 8, E-AC-3 10, DTS-HD 11, MLP 12, WMA Pro 14);
  // the types with no register (0, 9 DSD, 13 DST, 15) go nowhere. A mono descriptor counts.
  const struct { uint8_t format; uint32_t reg; } types[] = {{1, 0x28}, {2, 0x29}, {3, 0x2a}, {4, 0x2b}, {5, 0x2c}, {6, 0x2d}, {7, 0x2e}, {8, 0x2f}, {10, 0x31}, {11, 0x32}, {12, 0x33}, {14, 0x35},
                                                        {0, 0}, {9, 0}, {13, 0}, {15, 0}};
  for (const auto& t : types) {
    EdidAudio::Info one;
    one.sad_count = 1;
    one.sads[0] = {t.format, 0, 0x03, 0x44};
    for (uint32_t reg = 0x28; reg <= 0x35; ++reg) azalia.set(4, reg, 0xDEADBEEFu);
    CHECK(pins.write_sads(4, one));
    for (uint32_t reg = 0x28; reg <= 0x35; ++reg) {
      const bool written = reg != 0x30 && reg != 0x34;
      const uint32_t want = !written ? 0xDEADBEEFu : reg == t.reg ? (0u | 0x03u << 8 | 0x44u << 16 | (t.format == 1 ? 0x03u << 24 : 0u)) : 0u;
      CHECK(azalia.value(4, reg) == want);
    }
  }
  // a monitor that gives latency fields that it says are not there
  EdidAudio::Info absent;
  absent.video_latency[0] = absent.audio_latency[0] = 9;
  absent.video_latency[1] = absent.audio_latency[1] = 9;
  azalia.set(4, 0x37, 0x1234);
  CHECK(pins.write_latency(4, absent, false) && azalia.value(4, 0x37) == 0 && pins.write_latency(4, absent, true) && azalia.value(4, 0x37) == 0);
  // a speaker allocation block of the monitor: its first byte, 7 bits of it; and the lip sync of a monitor that gives none
  rules.speaker_count = 3;
  rules.speaker_allocation = 0xFF;
  azalia.set(6, 0x25, 0);
  CHECK(pins.write_speaker_allocation(6, rules) && azalia.value(6, 0x25) == (0x7Fu | 0x10000u));
  CHECK(pins.write_latency(6, rules, false) && azalia.value(6, 0x37) == 0 && pins.write_latency(6, rules, true) && azalia.value(6, 0x37) == 0);
  rules.latency_present[1] = true;
  rules.video_latency[1] = 0xAB;
  rules.audio_latency[1] = 0xCD;
  CHECK(pins.write_latency(6, rules, true) && azalia.value(6, 0x37) == 0xCDABu && pins.write_latency(6, rules, false) && azalia.value(6, 0x37) == 0);
  vega.shutdown();
  azalia.connectivity[2] = 0;
  azalia.writes.clear();
  CHECK(!vega.enable_audio(hdmi, ConstByteView(edid.data(), edid.size())) && !vega.disable_audio(hdmi) && azalia.writes.empty());
}

// The display driver and the sound driver together: the graphics card's HDMI / DisplayPort codec (tests/FakeHda.hpp) reports presence detect on a pin when the display side has enabled its
// endpoint (HOT_PLUG_CONTROL.AUDIO_ENABLED) and a valid ELD once the speaker allocation is programmed (the registers of tests/FakeAzalia.hpp), so the sound driver's pin is the endpoint that
// enable_audio() chose, and playing goes to it.
static void test_audio_end_to_end() {
  const std::vector<uint8_t> vbios = make_connector_vbios();
  Rig rig;
  FakeAzalia azalia;
  rig.card.azalia = &azalia;
  azalia.connectivity[1] = 0;        // only endpoint 1 has a port
  for (uint32_t pin = 0; pin < 3; ++pin) rig.card.gfx.set_reg(0x1898 + 8 * pin, 0x2);
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.hpd_pins = 0x0F;
  in.vbios = ConstByteView(vbios.data(), vbios.size());
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  const AtomConnectors::Connector hdmi = vega.connectors()[1], dp = vega.connectors()[2];
  const std::vector<uint8_t> plain = make_edid(0);
  const ConstByteView edid(plain.data(), plain.size());
  // the codec's pin n is the display engine's endpoint n: presence when the endpoint is enabled, an ELD once the speaker allocation is there
  FakeHda hda;
  hda.sense = [&](uint32_t n) -> uint32_t {
    if (!(azalia.value(n, 0x54) & 0x80000000u)) return 0;
    return (1u << 31) | (azalia.value(n, 0x25) ? 1u << 30 : 0u);
  };
  IntelHdaAudioDevice sound(hda, rig.alloc);
  CHECK(sound.initialize());
  CHECK(sound.output_pin() == FakeHda::kPins[0] && !sound.output_has_display());      // nothing enabled yet
  // the display driver enables the HDMI connector's endpoint (the first connected one: 1); the sound driver follows it
  CHECK(vega.enable_audio(hdmi, edid));
  CHECK(sound.retarget() && sound.output_pin() == FakeHda::kPins[1] && sound.output_has_display() && sound.output_is_digital());
  CHECK(hda.pins[1].control == 0x40 && hda.pins[1].dip_xmit == 0xC0 && hda.pins[0].control == 0 && hda.converters[0].stream == 0x10);
  std::vector<uint8_t> pcm(2048, 0x33);
  CHECK(sound.write(ConstByteView(pcm.data(), pcm.size())) == 2048 && sound.queued_bytes() == 2048);
  // the HDMI disabled, the sound driver is back to the first pin; the same endpoint serves the DisplayPort connector
  CHECK(vega.disable_audio(hdmi) && sound.retarget() && sound.output_pin() == FakeHda::kPins[0] && !sound.output_has_display());
  CHECK(vega.enable_audio(dp, edid) && sound.retarget() && sound.output_pin() == FakeHda::kPins[1] && sound.output_has_display());
  // endpoint 2 (the codec's DisplayPort pin) takes over when it is the connected one
  azalia.connectivity[1] = 1;
  azalia.connectivity[2] = 0;
  CHECK(vega.disable_audio(dp));
  CHECK(vega.enable_audio(hdmi, edid) && sound.retarget() && sound.output_pin() == FakeHda::kPins[2] && sound.output_has_display() && hda.pins[1].control == 0 && hda.pins[2].control == 0x40);
  // the speaker allocation is what makes the ELD valid: presence without it is a display of lower preference
  azalia.set(2, 0x25, 0);
  CHECK(sound.retarget() && sound.output_pin() == FakeHda::kPins[2] && !sound.output_has_display());
  sound.shutdown();
  vega.shutdown();
}

// The audio clock, the packets in the blanking and the AVI InfoFrame of dce_v11_0_afmt_setmode, in the registers of the connector's audio format block (HDMI-A: DIG 2, at +0x200; DisplayPort: DIG 3, at
// +0x300). Register numbers and field positions are literal (from the headers), the values worked out by hand from the registers' previous contents; the previous contents are not zero, so
// the fields written alone are seen to be.
static void test_audio_packets() {
  const std::vector<uint8_t> vbios = make_connector_vbios();
  Rig rig;
  FakeAzalia azalia;
  rig.card.azalia = &azalia;
  azalia.connectivity[2] = 0;
  for (uint32_t pin = 0; pin < 3; ++pin) rig.card.gfx.set_reg(0x1898 + 8 * pin, 0x2);
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.hpd_pins = 0x0F;
  in.vbios = ConstByteView(vbios.data(), vbios.size());
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  const AtomConnectors::Connector hdmi = vega.connectors()[1], dp = vega.connectors()[2];
  const std::vector<uint8_t> plain = make_edid(0);
  const ConstByteView edid(plain.data(), plain.size());
  // the registers of block `n` (at +0x100 * n from DIG 0's) with the contents they had before
  const auto prefill = [&](uint32_t off) {
    const struct { uint32_t reg, value; } pre[] = {{0x4a09, 0x0F0F0F0Fu}, {0x4a0b, 0xFFFFFFFFu}, {0x4a0c, 0}, {0x4a0d, 0xA5A5A5A5u}, {0x4a0e, 0}, {0x4a0f, 0xC0C0C0C0u}, {0x4a13, 0xFFFFFFFFu},
                                                   {0x4a14, 0}, {0x4a1e, 0xFFFFFFFFu}, {0x4a1f, 0xFFFFFFFFu}, {0x4a20, 0xFFFFFFFFu}, {0x4a21, 0xFFFFFFFFu}, {0x4a2e, 0xFFFFFFFFu},
                                                   {0x4a2f, 0xFFFFFFFFu}, {0x4a30, 0xFFFFFFFFu}, {0x4a31, 0xFFFFFFFFu}, {0x4a32, 0xFFFFFFFFu}, {0x4a33, 0xFFFFFFFFu}, {0x4a38, 0},
                                                   {0x4a39, 0}, {0x4a3a, 0xFFFFFFFFu}, {0x4a3b, 0xFFFFFFFFu}, {0x4a3c, 0xFFFFFFFFu}, {0x4a3d, 0xFFFFFFFFu}, {0x4a3e, 0xFFFFFFFFu},
                                                   {0x4a3f, 0}, {0x4a42, 0}, {0x4a44, 0}};
    for (const auto& r : pre) rig.card.gfx.set_reg(r.reg + off, r.value);
  };
  const auto reg = [&](uint32_t r) { return rig.card.gfx.reg(r); };
  prefill(0x200);
  rig.card.gfx.set_reg(0x16b, 0xFFFFFFFFu);
  AmdVegaM::AudioMode mode;           // 1920x1080 at 60 Hz, 148.5 MHz, 8 bits
  mode.crtc = 3;
  CHECK(vega.enable_audio(hdmi, edid, mode));
  // the audio clock: DTO 0 follows controller 3, 24 MHz over the pixel clock
  CHECK(reg(0x16b) == 0xFFFFFFFBu && reg(0x16c) == 24000 && reg(0x16d) == 148500);
  // the packets
  CHECK(reg(0x4c0d) == 0xA5A5A5B5u);          // HDMI_VBI_PACKET_CONTROL: null packets (bit 0), general control (bit 4), every frame (bit 5)
  CHECK(reg(0x4c3a) == 0x1000);               // AFMT_AUDIO_CRC_CONTROL
  CHECK(reg(0x4c09) == 0x0E0F0F0Fu);          // HDMI_CONTROL: no deep colour (bit 24 and bits 29:28 cleared)
  CHECK(reg(0x4c0e) == 0x33);                 // HDMI_INFOFRAME_CONTROL0: audio info send and cont (0x30), AVI info send and cont (0x03)
  CHECK(reg(0x4c44) == 0x80);                 // AFMT_INFOFRAME_CONTROL0: audio info update
  CHECK(reg(0x4c0f) == 0xC0C0C2C2u);          // HDMI_INFOFRAME_CONTROL1: audio info line (bits 13:8) and AVI info line (5:0) 2
  CHECK(reg(0x4c13) == 0);                    // HDMI_GC: no AVMUTE
  CHECK(reg(0x4c0b) == 0xFFE3FFDFu);          // HDMI_AUDIO_PACKET_CONTROL: delay enable (5:4) 1, packets per line (20:16) 3
  CHECK(reg(0x4c42) == 0x04000001u);          // AFMT_AUDIO_PACKET_CONTROL: 60958 channel status update (bit 26), audio samples sent (bit 0)
  CHECK(reg(0x4c0c) == 0x1100);               // HDMI_ACR_PACKET_CONTROL: the software CTS (bit 8) and automatic sending (bit 12)
  // the audio clock regeneration: the table's row for 148.5 MHz: N 4096 / CTS 148500 at 32 kHz, 6272 / 165000 at 44.1, 6144 / 148500 at 48 (CTS in bits 31:12, N in 19:0)
  CHECK(reg(0x4c2e) == 0x24414FFFu && reg(0x4c2f) == 0xFFF01000u && reg(0x4c30) == 0x28488FFFu && reg(0x4c31) == 0xFFF01880u && reg(0x4c32) == 0x24414FFFu && reg(0x4c33) == 0xFFF01800u);
  // the channel status: channel numbers 1 and 2 (bits 23:20 of 60958_0 and _1), 3 to 8 in the nibbles of 60958_2
  CHECK(reg(0x4c38) == 0x00100000u && reg(0x4c39) == 0x00200000u && reg(0x4c3f) == 0x00876543u);
  CHECK(reg(0x4c14) == 0xFF00);               // all eight channels enabled
  // the AVI InfoFrame of 1080p60: VIC 16, 16:9 (checksum 0x27, PB1 0x10, PB2 0x28, PB4 16), version 2
  CHECK(reg(0x4c1e) == 0x00281027u && reg(0x4c1f) == 0x00000010u && reg(0x4c20) == 0 && reg(0x4c21) == 0x02000000u);
  CHECK(reg(0x4c3b) == 0x00FFFFFFu && reg(0x4c3c) == 0x007FFFFFu && reg(0x4c3d) == 1 && reg(0x4c3e) == 1);       // the ramp test registers
  CHECK((reg(0x4c45) & 7) == 2);              // the endpoint selected
  CHECK(azalia.value(2, 0x54) == 0x80000000u && azalia.writes.back().index == 0x54 && azalia.writes.back().value == 0x80000000u);   // and enabled last
  // the other block is not touched
  CHECK(reg(0x4d09) == 0 && reg(0x4d0d) == 0 && reg(0x4d1e) == 0 && reg(0x4b0d) == 0);
  // deep colour: 10 bits, and 12; the software CTS only without deep colour
  mode.bpc = 10;
  prefill(0x200);
  CHECK(vega.enable_audio(hdmi, edid, mode) && reg(0x4c09) == 0x1F0F0F0Fu && reg(0x4c0c) == 0x1000);
  mode.bpc = 12;
  prefill(0x200);
  CHECK(vega.enable_audio(hdmi, edid, mode) && reg(0x4c09) == 0x2F0F0F0Fu && reg(0x4c0c) == 0x1000);
  for (uint32_t bpc : {0u, 6u, 8u, 16u, 7u, 11u}) {
    mode.bpc = bpc;
    prefill(0x200);
    CHECK(vega.enable_audio(hdmi, edid, mode) && reg(0x4c09) == 0x0E0F0F0Fu && reg(0x4c0c) == (bpc > 8 ? 0x1000u : 0x1100u));
  }
  // another mode: 720x480 interlaced, double clocked, 13.5 MHz, 4:3 (VIC 6), on the DisplayPort connector's block (DIG 3, +0x300) and controller 1; the odd pixel clock is calculated
  AmdVegaM::AudioMode ntsc;
  ntsc.clock_khz = 13500;
  ntsc.hdisplay = 720;
  ntsc.vdisplay = 480;
  ntsc.htotal = 858;
  ntsc.vtotal = 525;
  ntsc.interlaced = true;
  ntsc.double_clock = true;
  ntsc.crtc = 1;
  prefill(0x300);
  CHECK(vega.enable_audio(dp, edid, ntsc));
  CHECK(reg(0x16c) == 24000 && reg(0x16d) == 13500 && (reg(0x16b) & 7) == 1);
  // PB1 0x10, PB2 4:3 (1 << 4) | 8 = 0x18, PB4 VIC 6, PB5 pixel repeat 1: checksum 256 - (0x82 + 2 + 13 + 0x10 + 0x18 + 6 + 1) = 256 - 0xC0 = 0x40
  CHECK(reg(0x4d1e) == (0x40u | 0x10u << 8 | 0x18u << 16) && reg(0x4d1f) == (6u | 1u << 8) && reg(0x4d21) == 0x02000000u);
  // 13.5 MHz is not in the table, so it is calculated: 128 * 32000 / 13500000 = 1024 / 3375 in lowest terms, scaled by 4 to N >= 4096: 4096 / 13500; 44.1 kHz: 784 / 1875, scaled by 8 to
  // N >= 5644: 6272 / 15000; 48 kHz: 512 / 1125, scaled by 12 to N >= 6144: 6144 / 13500
  CHECK(reg(0x4d2e) == (13500u << 12 | 0xFFFu) && reg(0x4d2f) == 0xFFF01000u);
  CHECK(reg(0x4d30) == (15000u << 12 | 0xFFFu) && reg(0x4d31) == 0xFFF01880u && reg(0x4d32) == (13500u << 12 | 0xFFFu) && reg(0x4d33) == 0xFFF01800u);
  // the AVI words of a frame with every byte different (the InfoFrames made by Hdmi have zeros in the bar bytes): header 0x82 at [0], version at [1]; the checksum and the data from [3] go into
  // INFO0..2 four bytes each, the last two data bytes and the version into INFO3
  uint8_t frame[17];
  for (uint32_t i = 0; i < 17; ++i) frame[i] = static_cast<uint8_t>(0x10 + i);
  amd_dce11::AudioFormat blocks(*rig.inputs().registers);
  CHECK(blocks.start(2, ConstByteView(frame, 17)));
  CHECK(reg(0x4c1e) == 0x16151413u && reg(0x4c1f) == 0x1A191817u && reg(0x4c20) == 0x1E1D1C1Bu && reg(0x4c21) == 0x1100201Fu);
  // a short frame, a block or controller that does not exist: nothing is written
  const size_t before = rig.card.gfx.writes.size();
  CHECK(!blocks.start(2, ConstByteView(frame, 16)) && !blocks.start(6, ConstByteView(frame, 17)) && !blocks.setup_packets(6, 0, 148500, 8) && !blocks.setup_packets(2, 6, 148500, 8) &&
        !blocks.setup_packets(2, 0xFFFFFFFFu, 148500, 8) && !blocks.enable_channels(6) && !blocks.enable_channels(0xFFFFFFFFu));
  CHECK(rig.card.gfx.writes.size() == before);
  // a controller that does not exist: nothing is written
  const size_t writes = rig.card.gfx.writes.size();
  const size_t endpoint_writes = azalia.writes.size();
  AmdVegaM::AudioMode bad;
  bad.crtc = 6;
  CHECK(!vega.enable_audio(hdmi, edid, bad) && !vega.enable_audio(dp, edid, bad));
  CHECK(azalia.writes.size() == endpoint_writes && rig.card.gfx.writes.size() == writes);
  vega.shutdown();
}

// DisplayPort link training through the video BIOS: the sink's DPCD is read, the link configured for the mode, the DPCD written and the encoder and transmitter tables called in the order
// of the training (start, pattern 1, the drive, pattern 3 for a sink that has it, complete), for the DisplayPort connector (DIG 3, link B) and the eDP panel (DIG 0, link A, powered up).
static void test_train_link() {
  const std::vector<uint8_t> vbios = make_connector_vbios();   // eDP (0x92, pin 0, DIG 0), HDMI-A (0x91, DIG 2), DisplayPort (0x93, pin 2, DIG 3 link B; object id 19)
  Rig rig;
  FakeAuxCard auxcard;
  rig.card.aux = &auxcard;
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.vbios = ConstByteView(vbios.data(), vbios.size());
  AmdVegaM::LinkTraining t;
  CHECK(!vega.train_link(AtomConnectors::Connector(), 148500, 8, &t));          // (not up)
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  const AtomConnectors::Connector edp = vega.connectors()[0], hdmi = vega.connectors()[1], dp = vega.connectors()[2];
  std::vector<uint8_t>& dpcd = auxcard.sink.dpcd;
  dpcd.assign(0x800, 0);
  // an HBR2 four lane sink with TPS3, enhanced framing and downspread, that has the clock and the equalization at once
  const uint8_t caps[8] = {0x12, 0x14, 0xC4, 0x01, 0x00, 0x00, 0x01, 0x00};
  for (size_t i = 0; i < 8; ++i) dpcd[i] = caps[i];
  dpcd[0x202] = dpcd[0x203] = 0x77;
  dpcd[0x204] = 0x01;
  const auto actions = [&](uint32_t table) {
    std::vector<uint32_t> a;
    for (const FakeAuxCard::DigCall& c : auxcard.dig_calls)
      if (c.table == table && (table == 2 ? (c.words[0] >> 24) == 11 : true)) a.push_back(table == 0 ? (c.words[0] >> 8) & 0xFF : c.words[0] >> 24);
    return a;
  };
  // 1080p60 at 8 bits: the lowest rate that carries it, on the fewest lanes: four lanes at 1.62 Gbit/s (bandwidth code 0x06)
  auxcard.sink.dpcd_writes.clear();
  CHECK(vega.train_link(dp, 148500, 8, &t));
  CHECK(t.outcome.trained() && t.config.lanes == 4 && t.config.rate_khz == 162000 && t.outcome.bandwidth_code == 0x06 && t.caps.max_lanes == 4 && t.caps.tps3);
  // the DPCD writes: power, downspread, lane count with enhanced framing, rate, pattern off, pattern 1, the drive of four lanes, pattern 3, pattern off
  const auto& w = auxcard.sink.dpcd_writes;
  const struct { uint32_t address; std::vector<uint8_t> bytes; } want[] = {{0x600, {0x01}}, {0x107, {0x10}}, {0x101, {0x84}}, {0x100, {0x06}}, {0x102, {0x00}}, {0x102, {0x01}},
                                                                            {0x103, {0x00, 0x00, 0x00, 0x00}}, {0x102, {0x03}}, {0x102, {0x00}}};
  CHECK(w.size() == 9);
  for (size_t i = 0; i < w.size() && i < 9; ++i) CHECK(w[i].address == want[i].address && w[i].bytes == want[i].bytes);
  // DIGx encoder control (revision 1.5, table 4): the DIG and the action: start 0x08, pattern 1 0x09, pattern 3 0x13, complete 0x0B
  CHECK(actions(0) == std::vector<uint32_t>({0x08, 0x09, 0x13, 0x0B}) && actions(1).empty());
  for (const FakeAuxCard::DigCall& c : auxcard.dig_calls)
    if (c.table == 0) CHECK((c.words[0] & 0xFF) == 3 && c.words[1] == 0 && c.words[2] == 0);
  // the transmitter (V1.5): one drive of 0: symbol clock 16200, PHY id 3, action 11; four lanes, connector object 19, DP mode, config (HPD3 0x30 | the CRTC's PLL, here none (3) << 2 | coherent 2);
  // DIG 3 front end, the lane set 0
  CHECK(actions(2) == std::vector<uint32_t>({11}));
  for (const FakeAuxCard::DigCall& c : auxcard.dig_calls)
    if (c.table == 2 && (c.words[0] >> 24) == 11) CHECK(c.words[0] == (16200u | 3u << 16 | 11u << 24) && c.words[1] == (4u | 19u << 8 | (0x30u | 3u << 2 | 2u) << 24) && c.words[2] == (1u << 3));
  CHECK(auxcard.panel_writes == 0);     // (a DisplayPort connector has no panel)
  // with the PLL of the CRTC (PPLL0): the PHY's clock source in the configuration
  auxcard.dig_calls.clear();
  CHECK(vega.train_link(dp, 148500, 8, &t, AmdModeSet::kPpll0));
  for (const FakeAuxCard::DigCall& c : auxcard.dig_calls)
    if (c.table == 2 && (c.words[0] >> 24) == 11) CHECK((c.words[1] >> 24) == (0x30u | 2u << 2 | 2u));
  auxcard.dig_calls.clear();
  auxcard.sink.dpcd_writes.clear();
  // 4K60: four lanes at 5.4 Gbit/s
  auxcard.dig_calls.clear();
  auxcard.sink.dpcd_writes.clear();
  CHECK(vega.train_link(dp, 594000, 8, &t) && t.config.lanes == 4 && t.config.rate_khz == 540000 && t.outcome.bandwidth_code == 0x14);
  CHECK(w[2].address == 0x101 && w[2].bytes == std::vector<uint8_t>({0x84}) && w[3].bytes == std::vector<uint8_t>({0x14}) && w[6].address == 0x103 && w[6].bytes.size() == 4);
  // the eDP panel: powered first (it answers only then), DIG 0, link A
  dpcd[0x700] = 0x02;
  auxcard.unpowered_channel = 0x92;
  auxcard.dig_calls.clear();
  rig.card.gfx.set_reg(0x1898, 0x2);
  CHECK(vega.train_link(edp, 148500, 8, &t) && auxcard.panel_on && t.outcome.trained());
  CHECK(actions(0) == std::vector<uint32_t>({0x08, 0x09, 0x13, 0x0B}) && actions(1).empty());
  for (const FakeAuxCard::DigCall& c : auxcard.dig_calls)
    if (c.table == 0) CHECK((c.words[0] & 0xFF) == 0);
  auxcard.unpowered_channel = 0;
  // an eDP 1.4 panel with its own rate table (1.62 and 2.7 GHz, in units of 200 kHz at 0x010): set by the index, the bandwidth code written 0, the encoder told the table's rate
  dpcd[0x700] = 0x03;
  const uint8_t rates[6] = {0x2A, 0x03, 0x46, 0x05, 0x00, 0x00};
  for (size_t i = 0; i < 6; ++i) dpcd[0x10 + i] = rates[i];
  auxcard.dig_calls.clear();
  auxcard.sink.dpcd_writes.clear();
  CHECK(vega.train_link(edp, 148500, 8, &t) && t.config.rate_index == 0 && t.config.rate_khz == 162000 && t.config.lanes == 4 && t.outcome.rate_index == 0);
  CHECK(dpcd[0x100] == 0x00 && dpcd[0x115] == 0x00 && dpcd[0x101] == 0x84);
  dpcd[0x700] = 0x02;
  dpcd[0x10] = dpcd[0x11] = dpcd[0x12] = dpcd[0x13] = 0;
  dpcd[0x115] = 0x55;
  // not trained: the sink never shows the clock (lane status zero): the training fails and the outcome says how, the cleanup done
  dpcd[0x202] = dpcd[0x203] = 0x00;
  auxcard.dig_calls.clear();
  auxcard.sink.dpcd_writes.clear();
  t = AmdVegaM::LinkTraining();
  CHECK(!vega.train_link(dp, 148500, 8, &t) && t.outcome.result == DpLinkTrainer::Result::ClockRecoveryTries && t.config.lanes == 4 && !t.outcome.trained());
  CHECK(actions(0).back() == 0x0B && w.back().address == 0x102 && w.back().bytes == std::vector<uint8_t>({0x00}));
  dpcd[0x202] = dpcd[0x203] = 0x77;
  // refused: an HDMI connector, a connector without a DIG encoder in the table, one on a DIG the card does not have (UNIPHY3 is DIG 6), a sink that is no DPCD, a card not up
  auxcard.dig_calls.clear();
  AtomConnectors::Connector no_encoder = dp;
  no_encoder.encoder_object = 0;
  AtomConnectors::Connector dig6 = dp;
  dig6.encoder_object = 0x2125;
  CHECK(!vega.train_link(hdmi, 148500, 8, &t) && !vega.train_link(no_encoder, 148500, 8, &t) && !vega.train_link(dig6, 148500, 8, &t) && auxcard.dig_calls.empty());
  dpcd[1] = 0;
  CHECK(!vega.train_link(dp, 148500, 8, &t) && auxcard.dig_calls.empty());
  dpcd[1] = 0x14;
  // other bits per colour change the bits per pixel the link has to carry (3 * bpc; 0 is taken as 24): 148.5 MHz needs four lanes at 1.62 up to 10 bits, four at 2.7 beyond
  const struct { uint32_t bpc, rate; } depths[] = {{6, 162000}, {8, 162000}, {10, 162000}, {12, 270000}, {16, 270000}, {7, 162000}, {0, 162000}};
  for (const auto& d : depths) {
    CHECK(vega.train_link(dp, 148500, d.bpc, &t) && t.config.lanes == 4 && t.config.rate_khz == d.rate);
  }
  // nothing carries 4K at 16 bits on a sink that can only do 2.7: refused before anything is sent
  dpcd[1] = 0x0A;
  auxcard.dig_calls.clear();
  CHECK(!vega.train_link(dp, 594000, 16, &t) && t.config.lanes == 0 && auxcard.dig_calls.empty());
  dpcd[1] = 0x14;
  CHECK(vega.train_link(dp, 148500, 8, nullptr));
  vega.shutdown();
  CHECK(!vega.train_link(dp, 148500, 8, &t));
}

// Mode setting of the three outputs through the video BIOS: the order of the tables and their words for the DisplayPort connector (DIG 3: capabilities, link config, the training between
// the transmitter and the output), the eDP panel (powered up first), and HDMI (no training, pixel clock on PPLL1/PPLL2), the failures by stage, and switching off.
static void test_set_mode() {
  const std::vector<uint8_t> vbios = make_connector_vbios();   // eDP (0x92, pin 0, DIG 0), HDMI-A (0x91, pin 1, DIG 2), DisplayPort (0x93, pin 2, DIG 3 link B)
  Rig rig;
  FakeAuxCard auxcard;
  rig.card.aux = &auxcard;
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.vbios = ConstByteView(vbios.data(), vbios.size());
  in.display_clocks.sclk_high_khz = 1200000; in.display_clocks.mclk_high_khz = 800000; in.display_clocks.sclk_low_khz = 300000; in.display_clocks.mclk_low_khz = 150000;
  CrtcMode m;
  m.pixel_clock_khz = 148500;
  m.h_display = 1920; m.h_sync_start = 2008; m.h_sync_end = 2052; m.h_total = 2200;
  m.v_display = 1080; m.v_sync_start = 1084; m.v_sync_end = 1089; m.v_total = 1125;
  AmdVegaM::ModeSetResult r;
  CHECK(!vega.set_mode(AtomConnectors::Connector(), 0, m, 8, &r) && r.failed == AmdVegaM::ModeSetStage::NotReady);
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  const AtomConnectors::Connector edp = vega.connectors()[0], hdmi = vega.connectors()[1], dp = vega.connectors()[2];
  std::vector<uint8_t>& dpcd = auxcard.sink.dpcd;
  dpcd.assign(0x800, 0);
  for (uint32_t i = 0; i < 6; ++i) rig.card.gfx.set_reg(0x321 + i, 0x10);      // the DMIF buffer allocation completes at once
  const uint8_t caps[8] = {0x12, 0x14, 0xC4, 0x01, 0x00, 0x00, 0x01, 0x00};
  for (size_t i = 0; i < 8; ++i) dpcd[i] = caps[i];
  dpcd[0x202] = dpcd[0x203] = 0x77;
  dpcd[0x204] = 0x01;
  // DisplayPort on CRTC 1 (the driver's order): power gating off, the CRTC locked, blanked and disabled; the CRTC's source (DIG 3 is 0x0B); the pixel clock (PPLL0 for DP), the timing, the
  // overscan and the scaler; the encoder off (DP video off, the transmitter disabled); the CRTC enabled, unblanked and unlocked; the encoder on: setup, panel mode, the transmitter
  // enabled, the link trained (start, pattern 1, the drive, pattern 3, complete) and the video on
  CHECK(vega.set_mode(dp, 1, m, 8, &r) && r.failed == AmdVegaM::ModeSetStage::Done && r.pll == AmdModeSet::kDpDto && r.link.config.lanes == 4 && r.link.config.rate_khz == 162000 &&
        r.link.outcome.trained() && r.panel_mode == 0);
  const std::vector<uint32_t> want_order = {13, 44, 35, 42, 12, 49, 40, 33, 4, 76, 35, 34, 44, 4, 4, 76, 4, 4, 76, 4, 4, 4};       // (a CRTC that was never enabled is not blanked first)
  CHECK(auxcard.table_order == want_order);
  if (auxcard.table_order != want_order) { for (uint32_t t : auxcard.table_order) printf(" %u", t); printf(" stage %d\n", int(r.failed)); }
  const auto& mc = auxcard.mode_calls;
  CHECK(mc.size() == 11 && mc[0].table == 13 && mc[0].words == std::vector<uint32_t>({1u}) && mc[1].table == 44 && mc[1].words == std::vector<uint32_t>({1u | 1u << 8}));
  CHECK(mc[2].table == 35 && mc[2].words == std::vector<uint32_t>({1u}));
  CHECK(mc[3].table == 42 && mc[3].words[0] == (1u | 0x0Bu << 8 | 0u << 16 | 2u << 24));
  CHECK(mc[4].table == 12 && mc[4].words[0] == 1485000 && (mc[4].words[1] & 0xFF) == 11 && ((mc[4].words[1] >> 16) & 0xFF) == 0 && mc[4].words[2] == 1);
  CHECK(mc[5].table == 49 && mc[5].words[0] == (1920u | 280u << 16) && mc[5].words[5] == 1);
  CHECK(mc[6].table == 40 && mc[6].words[2] == 1 && mc[7].table == 33 && mc[7].words[0] == 1);
  CHECK(mc[8].table == 35 && mc[8].words[0] == (1u | 1u << 8) && mc[9].table == 34 && mc[9].words[0] == 1u && mc[10].table == 44 && mc[10].words[0] == 1u);
  // the encoder: video off and the transmitter off first, then setup (DIG 3, four lanes, the pixel clock, 8 bits, 1.62 GHz = 6 x 270 MHz), the panel mode (external DP), the transmitter
  const auto& dc = auxcard.dig_calls;
  CHECK(dc.size() == 11 && ((dc[0].words[0] >> 8) & 0xFF) == 0x0C && dc[1].table == 2 && (dc[1].words[0] >> 24) == 0);
  CHECK(dc[2].words[0] == (3u | 0x0Fu << 8 | 0u << 16 | 4u << 24) && dc[2].words[1] == 14850 && dc[2].words[2] == (2u | 6u << 8));
  CHECK(dc[3].words[0] == (3u | 0x10u << 8 | 0u << 16) && dc[4].table == 2 && (dc[4].words[0] >> 24) == 1 && dc[4].words[0] == (16200u | 3u << 16 | 1u << 24));
  CHECK(dc[4].words[1] == (4u | AtomConnectors::Connector(dp).object_id << 8 | (0x30u | 3u << 2 | 2u) << 24));       // (the DP DTO, 11, truncated to the field's two bits, is the PHY's clock source)
  CHECK(((dc[5].words[0] >> 8) & 0xFF) == 0x08 && ((dc[dc.size() - 2].words[0] >> 8) & 0xFF) == 0x0B && ((dc.back().words[0] >> 8) & 0xFF) == 0x0D);
  // the sink was powered up by the training: D0 written
  CHECK(dpcd[0x600] == 0x01 || dpcd[0x600] == 0x02);
  // HDMI on CRTC 3 (PPLL2), four lanes, 10 bits (deep colour ratio 5:4), no training, no AUX channel use
  auxcard.table_order.clear();
  auxcard.mode_calls.clear();
  auxcard.dig_calls.clear();
  const size_t doorbells = auxcard.doorbells;
  CHECK(vega.set_mode(hdmi, 3, m, 10, &r) && r.pll == AmdModeSet::kComboPhyPll0 + 2 && auxcard.doorbells == doorbells);
  CHECK(auxcard.table_order == std::vector<uint32_t>({13, 44, 35, 42, 12, 49, 40, 33, 76, 35, 34, 44, 4, 4, 76}));
  CHECK(mc.size() == 11 && mc[4].words[2] == (3u | 1u << 8) && ((mc[4].words[1] >> 16) & 0xFF) == 3 && (mc[4].words[1] & 0xFF) == 22);
  CHECK(dc[0].table == 2 && (dc[0].words[0] >> 24) == 0 && dc[1].words[0] == (2u | 0x0Fu << 8 | 3u << 16 | 4u << 24) && dc[1].words[1] == 14850 && dc[1].words[2] == 3u);
  // with a surface: the graphics plane of the CRTC (3: block offset 0x2600) is pointed at it between the timing and the overscan; a bad surface is the Scanout stage
  amd_dce11::ScanoutSurface surface;
  surface.address = 0x100000;
  surface.width = 1920;
  surface.height = 1080;
  surface.pitch_pixels = 1920;
  surface.viewport_width = 1920;
  surface.viewport_height = 1080;
  CHECK(vega.set_mode(hdmi, 3, m, 8, &r, AmdModeSet::kPllInvalid, &surface));
  CHECK(rig.card.dce.get(0x1a00 + 0x2600) == 1 && rig.card.dce.get(0x1a06 + 0x2600) == 1920 && rig.card.dce.get(0x1a0d + 0x2600) == 1920 && rig.card.dce.get(0x1a01 + 0x2600) == 2);
  surface.address = 0x100001;
  CHECK(!vega.set_mode(hdmi, 3, m, 8, &r, AmdModeSet::kPllInvalid, &surface) && r.failed == AmdVegaM::ModeSetStage::Scanout);
  // the bandwidth update at the end of the base setup: the line buffer of CRTC 3 (1920 wide: two DMIF buffers) and its watermarks
  CHECK((rig.card.gfx.reg(0x321 + 3) & 7) == 2 && rig.card.dce.get(0x1b33 + 0x2600) != 0);
  // HDMI is a flat panel device with a FMT block: 8 bits truncated (enable, depth 1), 6 bits depth 0, 12 bits nothing; an interlaced mode turns the interleave on; the VGA mode is off after the unblank
  CHECK(rig.card.dce.get(0x1bf2 + 0x2600) == (1u | 1u << 4) && (rig.card.dce.get(0x1ac0 + 0x2600) & 8) == 0 && (rig.card.dce.get(0xf9) & 1) == 0);
  {
    CrtcMode interlaced = m;
    interlaced.interlaced = true;
    CHECK(vega.set_mode(hdmi, 3, interlaced, 8, &r) && (rig.card.dce.get(0x1ac0 + 0x2600) & 8) != 0 && rig.card.dce.get(0x1bf2 + 0x2600) == (1u | 1u << 4));
    CHECK(vega.set_mode(hdmi, 3, m, 6, &r) && (rig.card.dce.get(0x1ac0 + 0x2600) & 8) == 0 && rig.card.dce.get(0x1bf2 + 0x2600) == 1u);
    CHECK(vega.set_mode(hdmi, 3, m, 12, &r) && rig.card.dce.get(0x1bf2 + 0x2600) == 0u);
    // the CRTC was enabled, so the next preparation blanks it (with the VGA mode on around the blank)
    auxcard.mode_calls.clear();
    CHECK(vega.set_mode(hdmi, 3, m, 8, &r) && auxcard.mode_calls[2].table == 34 && auxcard.mode_calls[2].words[0] == (3u | 1u << 8) && auxcard.mode_calls[3].table == 35);
    auxcard.table_order.clear();
    auxcard.mode_calls.clear();
    auxcard.dig_calls.clear();
  }
  // eDP on CRTC 0: the panel powered up (it answers only then) before its capabilities are read, off for the encoder's mode set and on again for the commit
  auxcard.unpowered_channel = 0x92;
  auxcard.table_order.clear();
  auxcard.mode_calls.clear();
  auxcard.dig_calls.clear();
  rig.card.gfx.set_reg(0x1898, 0x2);
  CHECK(vega.set_mode(edp, 0, m, 8, &r) && auxcard.panel_on && dc[0].table == 2 && (dc[0].words[0] >> 24) == 12);
  size_t powered_off = 0, powered_on = 0;
  for (const FakeAuxCard::DigCall& c : dc) {
    if (c.table == 2 && (c.words[0] >> 24) == 13) ++powered_off;
    if (c.table == 2 && (c.words[0] >> 24) == 12) ++powered_on;
  }
  CHECK(powered_off == 1 && powered_on == 2 && r.panel_mode == 0);
  // an eDP panel with the alternate scrambler reset (DP_EDP_CONFIGURATION_CAP bit 0) is in the internal DP2 mode: the panel mode table gets 1 and the sink DP_EDP_CONFIGURATION_SET 1
  dpcd[0x00D] = 0x01;
  dpcd[0x10A] = 0;
  auxcard.dig_calls.clear();
  CHECK(vega.set_mode(edp, 0, m, 8, &r) && r.panel_mode == 1 && dpcd[0x10A] == 1);
  bool dp2 = false;
  for (const FakeAuxCard::DigCall& c : dc) dp2 = dp2 || c.words[0] == (0u | 0x10u << 8 | 1u << 16);
  CHECK(dp2);
  dpcd[0x00D] = 0;
  auxcard.unpowered_channel = 0;
  // failures name their stage: a mode that is no mode, a connector that is no digital output, a sink that has no DPCD or no lanes for the mode, a link that does not train
  CHECK(!vega.set_mode(dp, 6, m, 8, &r) && r.failed == AmdVegaM::ModeSetStage::Unsupported);
  CrtcMode empty;
  CHECK(!vega.set_mode(dp, 0, empty, 8, &r) && r.failed == AmdVegaM::ModeSetStage::Unsupported);
  AtomConnectors::Connector vga = dp;
  vga.type = AtomConnectors::Type::Vga;
  CHECK(!vega.set_mode(vga, 0, m, 8, &r) && r.failed == AmdVegaM::ModeSetStage::Unsupported);
  AtomConnectors::Connector no_ddc = dp;
  no_ddc.has_ddc = false;
  CHECK(!vega.set_mode(no_ddc, 0, m, 8, &r) && r.failed == AmdVegaM::ModeSetStage::SinkCapabilities);
  dpcd[1] = 0;
  CHECK(!vega.set_mode(dp, 0, m, 8, &r) && r.failed == AmdVegaM::ModeSetStage::SinkCapabilities);
  dpcd[1] = 0x0A;
  CrtcMode huge = m;
  huge.pixel_clock_khz = 2000000;
  CHECK(!vega.set_mode(dp, 0, huge, 8, &r) && r.failed == AmdVegaM::ModeSetStage::SinkCapabilities);
  dpcd[1] = 0x14;
  CrtcMode crooked = m;
  crooked.h_sync_end = crooked.h_sync_start - 1;
  CHECK(!vega.set_mode(hdmi, 0, crooked, 8, &r) && r.failed == AmdVegaM::ModeSetStage::Timing);
  dpcd[0x202] = dpcd[0x203] = 0;
  CHECK(!vega.set_mode(dp, 0, m, 8, &r) && r.failed == AmdVegaM::ModeSetStage::LinkTraining && !vega.link_trained(2));
  dpcd[0x202] = dpcd[0x203] = 0x77;
  // switching off: DP video off, the transmitter off, the CRTC blanked and disabled, its power gating on; the PLL (PPLL0) is still the eDP panel's CRTC 0 (set up by the last set_mode of
  // CRTC 0 above), so it stays on
  CHECK(vega.set_mode(dp, 1, m, 8, &r) && vega.set_mode(edp, 0, m, 8, &r) && vega.link_trained(2));
  auxcard.table_order.clear();
  auxcard.mode_calls.clear();
  auxcard.dig_calls.clear();
  CHECK(vega.disable_mode(dp, 1));
  CHECK(auxcard.table_order == std::vector<uint32_t>({4, 76, 34, 35, 13}) && !vega.link_trained(2));
  CHECK(((dc[0].words[0] >> 8) & 0xFF) == 0x0C && (dc[1].words[0] >> 24) == 0 && dpcd[0x600] == 0x02);        // (the sink to D3)
  CHECK(mc[0].words == std::vector<uint32_t>({1u | 1u << 8, 0}) && mc[1].words == std::vector<uint32_t>({1u}) && mc[2].table == 13 && mc[2].words == std::vector<uint32_t>({1u | 1u << 8}));
  // the DisplayPort outputs share the DP DTO, which is not switched off; the eDP panel's power is
  auxcard.table_order.clear();
  auxcard.mode_calls.clear();
  auxcard.dig_calls.clear();
  CHECK(vega.disable_mode(edp, 0));
  CHECK(auxcard.table_order == std::vector<uint32_t>({4, 76, 76, 34, 35, 13}));       // (the DP DTO is not a PLL to switch off)
  CHECK(!auxcard.panel_on);
  // a connector without an AUX session (HDMI) just goes off; a CRTC that does not exist or a connector that is no output is refused
  auxcard.mode_calls.clear();
  CHECK(vega.disable_mode(hdmi, 3) && rig.card.dce.get(0x1a00 + 0x2600) == 0);
  CHECK(mc.back().table == 12 && mc.back().words == std::vector<uint32_t>({0u, 22u, 0xFFu, 0u}));       // (a combo PHY PLL is switched off with no CRTC)
  CHECK(!vega.disable_mode(dp, 6) && !vega.disable_mode(vga, 0));
  vega.shutdown();
  CHECK(!vega.disable_mode(dp, 0) && !vega.set_mode(dp, 0, m, 8, &r) && r.failed == AmdVegaM::ModeSetStage::NotReady);
}

// The HDMI audio set up in the encoder's mode set (dce_v11_0_encoder_mode_set: afmt_enable, afmt_setmode), with the monitor's EDID: the endpoint enabled, the stage named on failure.
static void test_set_mode_audio() {
  const std::vector<uint8_t> vbios = make_connector_vbios();
  const uint8_t cea[] = {0x2C, 0x09, 0x07, 0x07, 0x15, 0x07, 0x50, 0x3F, 0x1E, 0x18, 0x0F, 0x54, 0x07, 0x83, 0x05, 0x00, 0x00, 0x6C, 0x03, 0x0C, 0x00, 0x10, 0x00, 0x80, 0x00, 0xC0, 30, 40, 50, 60};
  std::vector<uint8_t> edid = make_edid(1);
  for (size_t i = 3; i < 127; ++i) edid[128 + i] = 0;
  edid[128 + 2] = 4 + sizeof cea;
  for (size_t i = 0; i < sizeof cea; ++i) edid[128 + 4 + i] = cea[i];
  uint8_t sum = 0;
  for (size_t i = 0; i < 127; ++i) sum = static_cast<uint8_t>(sum + edid[128 + i]);
  edid[255] = static_cast<uint8_t>(0 - sum);
  Rig rig;
  FakeAzalia azalia;
  FakeAuxCard auxcard;
  rig.card.aux = &auxcard;
  rig.card.azalia = &azalia;
  azalia.connectivity[2] = 0;
  azalia.connectivity[3] = 3;
  for (uint32_t pin = 0; pin < 3; ++pin) rig.card.gfx.set_reg(0x1898 + 8 * pin, 0x2);
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.hpd_pins = 0x0F;
  in.vbios = ConstByteView(vbios.data(), vbios.size());
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  const AtomConnectors::Connector hdmi = vega.connectors()[1], dp = vega.connectors()[2];
  CrtcMode m;
  m.pixel_clock_khz = 148500;
  m.h_display = 1920; m.h_sync_start = 2008; m.h_sync_end = 2052; m.h_total = 2200;
  m.v_display = 1080; m.v_sync_start = 1084; m.v_sync_end = 1089; m.v_total = 1125;
  AmdVegaM::ModeSetResult r;
  azalia.writes.clear();
  rig.card.gfx.set_reg(0x4c45, 0xAB000005u);
  azalia.set(2, 0x25, 0xFFFFFFFFu);
  // without an EDID: no audio setup
  CHECK(vega.set_mode(hdmi, 3, m, 8, &r) && azalia.writes.empty());
  // with it: the endpoint is disabled, programmed and enabled; HDMI's block (DIG 2) selects endpoint 2
  CHECK(vega.set_mode(hdmi, 3, m, 8, &r, AmdModeSet::kPllInvalid, nullptr, ConstByteView(edid.data(), edid.size())));
  CHECK(azalia.writes.size() == 16 && azalia.writes[0].pin == 2 && azalia.writes[0].index == 0x54 && azalia.writes[15].index == 0x54 && azalia.writes[15].value == 0x80000000u);
  // DisplayPort has no audio setup in the modeset, even with an EDID
  azalia.writes.clear();
  std::vector<uint8_t>& dpcd = auxcard.sink.dpcd;
  dpcd.assign(0x800, 0);
  const uint8_t caps[8] = {0x12, 0x14, 0xC4, 0x01, 0x00, 0x00, 0x01, 0x00};
  for (size_t i = 0; i < 8; ++i) dpcd[i] = caps[i];
  dpcd[0x202] = dpcd[0x203] = 0x77;
  dpcd[0x204] = 0x01;
  CHECK(vega.set_mode(dp, 1, m, 8, &r, AmdModeSet::kPllInvalid, nullptr, ConstByteView(edid.data(), edid.size())) && azalia.writes.empty());
  // a card with no connected endpoint: the Audio stage
  azalia.connectivity[2] = 1;
  azalia.connectivity[3] = 1;
  CHECK(!vega.set_mode(hdmi, 3, m, 8, &r, AmdModeSet::kPllInvalid, nullptr, ConstByteView(edid.data(), edid.size())) && r.failed == AmdVegaM::ModeSetStage::Audio);
  vega.shutdown();
}

// An output lit up on its own: the EDID read (over DDC for HDMI, over AUX for DisplayPort), the first detailed timing the connector accepts and the room holds, the picture, the mode set.
static std::vector<uint8_t> edid_with_timings(const std::vector<std::vector<uint8_t>>& dtds, bool hdmi_block = false) {
  std::vector<uint8_t> e(hdmi_block ? 256 : 128, 0);
  const uint8_t header[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
  memcpy(e.data(), header, 8);
  e[18] = 1; e[19] = 3; e[20] = 0x80;
  for (size_t i = 0; i < dtds.size() && i < 4; ++i) memcpy(&e[54 + i * 18], dtds[i].data(), 18);
  if (hdmi_block) {
    e[126] = 1;
    e[128] = 0x02; e[129] = 3; e[130] = 4 + 8; e[132] = 3 << 5 | 7; e[133] = 0x03; e[134] = 0x0C; e[135] = 0x00; e[136] = 0x10; e[137] = 0x00; e[138] = 0x00; e[139] = 68;
    uint8_t sum = 0;
    for (int i = 128; i < 255; ++i) sum = static_cast<uint8_t>(sum + e[i]);
    e[255] = static_cast<uint8_t>(0 - sum);
  }
  uint8_t sum = 0;
  for (int i = 0; i < 127; ++i) sum = static_cast<uint8_t>(sum + e[i]);
  e[127] = static_cast<uint8_t>(0 - sum);
  return e;
}

static void test_light_up() {
  const std::vector<uint8_t> vbios = make_connector_vbios();   // eDP (0x92, pin 0, DIG 0), HDMI-A (0x91, pin 1, DIG 2), DisplayPort (0x93, pin 2, DIG 3)
  // 1080p60 (148.5 MHz, 1920 + 88 + 44 + 148 / 1080 + 4 + 5 + 36, + sync) and 4K60 (594 MHz) timings
  const std::vector<uint8_t> t1080 = {0x02, 0x3A, 0x80, 0x18, 0x71, 0x38, 0x2D, 0x40, 0x58, 0x2C, 0x45, 0x00, 0, 0, 0, 0, 0, 0x1E};
  const std::vector<uint8_t> t4k = {0x28, 0xE8, 0x00, 0x30, 0xF0, 0x70, 0x2C, 0x80, 0x30, 0x20, 0x36, 0x00, 0, 0, 0, 0, 0, 0x18};
  const std::vector<uint8_t> t720 = {0x01, 0x1D, 0x00, 0x72, 0x51, 0xD0, 0x1E, 0x20, 0x6E, 0x28, 0x55, 0x00, 0, 0, 0, 0, 0, 0x1E};
  Rig rig;
  FakeI2cMonitor monitor;
  FakeAuxCard auxcard;
  rig.card.monitor = &monitor;
  rig.card.aux = &auxcard;
  for (uint32_t pin = 0; pin < 3; ++pin) rig.card.gfx.set_reg(0x1898 + 8 * pin, 0x2);
  for (uint32_t i = 0; i < 6; ++i) rig.card.gfx.set_reg(0x321 + i, 0x10);
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.hpd_pins = 0x3F;
  in.vbios = ConstByteView(vbios.data(), vbios.size());
  in.output_framebuffer_bytes = 9ull << 20;
  in.display_clocks.sclk_high_khz = 1200000; in.display_clocks.mclk_high_khz = 800000; in.display_clocks.sclk_low_khz = 300000; in.display_clocks.mclk_low_khz = 150000;
  AmdVegaM::LightUp lu;
  CHECK(!vega.light_up(AtomConnectors::Connector(), 0xFF, &lu) && lu.failed == AmdVegaM::LightUpStage::NotReady);
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  const AtomConnectors::Connector hdmi = vega.connectors()[1], dp = vega.connectors()[2];
  // HDMI (a plain DVI-style EDID here): the 4K timing first is refused (HDMI-A above 165 MHz needs an HDMI block, and 4K does not fit 16 MiB either), the 1080p one is taken
  monitor.edid = edid_with_timings({t4k, t1080});
  CHECK(vega.light_up(hdmi, 0xFF, &lu));
  CHECK(lu.failed == AmdVegaM::LightUpStage::Done && lu.crtc == 0 && lu.width == 1920 && lu.height == 1080 && lu.mode.pixel_clock_khz == 148500 && lu.mode.h_total == 2200 && lu.mode.h_sync_start == 2008 &&
        lu.mode.h_sync_end == 2052 && lu.mode.v_total == 1125 && lu.mode.v_sync_start == 1084 && lu.mode.v_sync_end == 1089 && !lu.mode.h_sync_negative && !lu.mode.v_sync_negative && lu.bpc == 8);
  // the picture: a white border and a grey ramp between; the plane enabled on CRTC 0 with its address inside the block's output area, 1920 pixels a row
  CHECK(lu.set.failed == AmdVegaM::ModeSetStage::Done && rig.card.dce.get(0x1a00) == 1 && rig.card.dce.get(0x1a06) == 1920 && rig.card.dce.get(0x1a0e) == 1080);
  const MmioRegion pic = vega.picture();
  CHECK(pic.valid() && pic.length() == 1920ull * 1080 * 4 && pic.read32(0) == 0xFFFFFFFFu && pic.read32((540ull * 1920 + 960) * 4) == (0xFF000000u | 127u << 16 | 127u << 8 | 127u) && pic.read32((540ull * 1920 + 10) * 4) == (0xFF000000u | 1u << 16 | 1u << 8 | 1u));
  CHECK(pic.read32((1079ull * 1920 + 1919) * 4) == 0xFFFFFFFFu);
  // a second output goes on the next CRTC; the first stays
  monitor.edid = edid_with_timings({t720});
  CHECK(vega.light_up(hdmi, 0xFF, &lu) && lu.crtc == 1 && lu.width == 1280 && lu.height == 720 && lu.mode.v_total == 750);
  // DisplayPort over AUX: the EDID, the link config from the sink, and a 4K timing that the link cannot carry is skipped for the next
  auxcard.sink.edid = edid_with_timings({t4k, t1080});
  std::vector<uint8_t>& dpcd = auxcard.sink.dpcd;
  dpcd.assign(0x800, 0);
  const uint8_t caps[8] = {0x12, 0x0A, 0xC4, 0x01, 0x00, 0x00, 0x01, 0x00};      // HBR, four lanes
  for (size_t i = 0; i < 8; ++i) dpcd[i] = caps[i];
  dpcd[0x202] = dpcd[0x203] = 0x77;
  dpcd[0x204] = 0x01;
  CHECK(vega.light_up(dp, 2, &lu) && lu.crtc == 2 && lu.width == 1920 && lu.set.link.outcome.trained() && lu.set.link.config.lanes == 4);
  // refused: nothing plugged in the pin, no timing the connector accepts, no EDID, an interlaced mode needs no special care, no room
  AtomConnectors::Connector unplugged = hdmi;
  unplugged.hpd_pin = 5;
  CHECK(!vega.light_up(unplugged, 0xFF, &lu) && lu.failed == AmdVegaM::LightUpStage::NotConnected);
  monitor.edid = edid_with_timings({t4k});
  CHECK(!vega.light_up(hdmi, 0xFF, &lu) && lu.failed == AmdVegaM::LightUpStage::NoMode);
  monitor.edid = std::vector<uint8_t>(128, 0);
  CHECK(!vega.light_up(hdmi, 0xFF, &lu) && lu.failed == AmdVegaM::LightUpStage::Edid);
  vega.shutdown();
  Rig plain;
  AmdVegaM none(plain.alloc);
  AmdVegaM::Inputs no_room = plain.inputs();
  no_room.vbios = ConstByteView(vbios.data(), vbios.size());
  plain.card.gfx.set_reg(0x1898, 0x2);
  CHECK(none.bring_up(no_room) && !none.light_up(none.connectors()[1], 0xFF, &lu) && lu.failed == AmdVegaM::LightUpStage::NoRoom);
  none.shutdown();
}

// The sink's DPCD over the AUX channel of the DisplayPort and eDP connectors (through the video BIOS's table, with the panel power-up for the eDP one).
static void test_read_dpcd() {
  const std::vector<uint8_t> vbios = make_connector_vbios();   // eDP (DDC line 0x92, pin 0), HDMI-A (0x91), DisplayPort (0x93, pin 2)
  Rig rig;
  FakeI2cMonitor monitor;
  FakeAuxCard auxcard;
  rig.card.monitor = &monitor;
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.vbios = ConstByteView(vbios.data(), vbios.size());
  Dpcd::Capabilities caps;
  Dpcd::LinkStatus status;
  CHECK(!vega.read_dpcd(AtomConnectors::Connector(), &caps));       // (not up)
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  const AtomConnectors::Connector edp = vega.connectors()[0], hdmi = vega.connectors()[1], dp = vega.connectors()[2];
  rig.card.aux = &auxcard;
  std::vector<uint8_t>& dpcd = auxcard.sink.dpcd;
  dpcd.assign(0x800, 0);
  const uint8_t dp_caps[8] = {0x12, 0x14, 0xC4, 0x01, 0x00, 0x00, 0x01, 0x00};
  for (size_t i = 0; i < 8; ++i) dpcd[i] = dp_caps[i];
  const uint8_t dp_status[8] = {0x01, 0x00, 0x77, 0x77, 0x81, 0x01, 0x00, 0x00};
  for (size_t i = 0; i < 8; ++i) dpcd[0x200 + i] = dp_status[i];
  // DisplayPort: capabilities, then the link status as well; the table is told the connector's line and pin; no panel power
  CHECK(vega.read_dpcd(dp, &caps) && caps.valid && caps.revision == 0x12 && caps.max_lanes == 4 && caps.max_link_rate_kbps() == 5400000 && caps.edp_revision == Dpcd::kNoEdp);
  CHECK(!auxcard.sink.requests.empty() && auxcard.sink.requests.front().channel == 0x93 && auxcard.sink.requests.front().hpd == 2 && auxcard.panel_writes == 0);
  CHECK(auxcard.sink.requests.front().bytes.size() == 4 && auxcard.sink.requests.front().bytes[2] == 0x90);       // a native read (9) of address 0
  CHECK(vega.read_dpcd(dp, &caps, &status) && status.link_status_updated() && status.channel_eq_ok(4) && status.sink_count() == 1);
  // eDP: powered up first (the panel answers only then), the panel's own rate table
  dpcd[0x700] = 0x03;
  dpcd[0x10] = 0xA4; dpcd[0x11] = 0x1F;       // 8100: 1.62 Gbit/s
  dpcd[0x12] = 0x30; dpcd[0x13] = 0x2A;       // 10800: 2.16 Gbit/s
  auxcard.unpowered_channel = 0x92;
  rig.card.gfx.set_reg(0x1898, 0x2);
  CHECK(!auxcard.panel_on && vega.read_dpcd(edp, &caps, &status) && auxcard.panel_on && auxcard.panel_writes == 1 && auxcard.panel_params == 0x0C000000u);
  CHECK(caps.edp_revision == 3 && caps.edp_rate_count == 2 && caps.edp_rates_khz[0] == 1620000 && caps.edp_rates_khz[1] == 2160000);
  CHECK(auxcard.sink.requests.back().channel == 0x92 && auxcard.sink.requests.back().hpd == 0);
  auxcard.unpowered_channel = 0;
  // a DisplayPort connector does not read the eDP registers
  const size_t before = auxcard.sink.requests.size();
  CHECK(vega.read_dpcd(dp, &caps) && caps.edp_revision == Dpcd::kNoEdp && auxcard.sink.requests.size() == before + 1);
  // not a DisplayPort connector, no DDC line, an unknown line: nothing is asked
  const size_t asked = auxcard.sink.requests.size();
  AtomConnectors::Connector no_ddc = dp;
  no_ddc.has_ddc = false;
  AtomConnectors::Connector unknown = dp;
  unknown.ddc_id = 0x77;
  CHECK(!vega.read_dpcd(hdmi, &caps) && !vega.read_dpcd(no_ddc, &caps) && !vega.read_dpcd(unknown, &caps) && auxcard.sink.requests.size() == asked);
  // nothing answers / not a DPCD / a link status that does not come: caps are left alone
  caps = Dpcd::Capabilities();
  auxcard.sink.monitor_present = true;
  auxcard.sink.timeouts = 1000;
  CHECK(!vega.read_dpcd(dp, &caps) && !caps.valid);
  auxcard.sink.timeouts = 0;
  dpcd[1] = 0;
  CHECK(!vega.read_dpcd(dp, &caps) && !caps.valid);
  dpcd[1] = 0x14;
  CHECK(vega.read_dpcd(dp, &caps) && caps.valid);
  dpcd.resize(0x203);       // (a short DPCD: the status read is outside it)
  caps = Dpcd::Capabilities();
  CHECK(!vega.read_dpcd(dp, &caps, &status) && !caps.valid);
  CHECK(vega.read_dpcd(dp, &caps) && caps.valid);
  CHECK(auxcard.sink.malformed == 0);
  vega.shutdown();
  CHECK(!vega.read_dpcd(dp, &caps));
}

// A DisplayPort link that train_link trained and the sink then loses is trained again by service_short_pulses, for the same mode; a link that was never trained, one that holds, a sink
// that went away, a retraining that fails, and one that is lost again and again are left alone.
static void test_retrain_on_short_pulse() {
  const std::vector<uint8_t> vbios = make_connector_vbios();   // eDP (pin 0), HDMI-A (pin 1), DisplayPort (0x93, pin 2, DIG 3)
  Rig rig;
  FakeAuxCard auxcard;
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.hpd_pins = 0x3F;
  in.vbios = ConstByteView(vbios.data(), vbios.size());
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  rig.card.aux = &auxcard;
  const AtomConnectors::Connector dp = vega.connectors()[2];
  std::vector<uint8_t>& dpcd = auxcard.sink.dpcd;
  dpcd.assign(0x800, 0);
  const uint8_t caps[8] = {0x12, 0x14, 0xC4, 0x01, 0x00, 0x00, 0x01, 0x00};
  for (size_t i = 0; i < 8; ++i) dpcd[i] = caps[i];
  dpcd[0x202] = dpcd[0x203] = 0x77;
  dpcd[0x204] = 0x01;
  for (uint32_t pin = 0; pin < 6; ++pin) rig.card.gfx.set_reg(0x1899 + 8 * pin, rig.card.gfx.reg(0x1899 + 8 * pin) & ~0x100000u);
  auto pulse = [&](uint32_t pin) {
    rig.card.gfx.set_reg(0x1898 + 8 * pin, 0x100);
    rig.card.ih.post(42, 6 + pin, 0, 0, 0);
    return vega.service_interrupts();
  };
  // the sink recovers its clock again once the training starts (pattern 1)
  bool heals = true;
  auxcard.sink.after_write = [&](uint32_t address, const std::vector<uint8_t>& bytes) {
    if (heals && address == 0x102 && bytes.size() == 1 && bytes[0] == 0x01) dpcd[0x202] = dpcd[0x203] = 0x77;
  };
  AmdVegaM::SinkEvent ev[8];
  // never trained: a lost link is reported, not trained
  CHECK(!vega.link_trained(2) && !vega.link_trained(6) && !vega.link_trained(0xFFFFFFFFu));
  dpcd[0x101] = 2;
  dpcd[0x202] = 0x70;
  auxcard.dig_calls.clear();
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].link_lost && !ev[0].retrained && !ev[0].retrain_limit && auxcard.dig_calls.empty());
  // trained for 1080p60 at 8 bits (two lanes at 2.7 Gbit/s)
  dpcd[0x202] = 0x77;
  AmdVegaM::LinkTraining t;
  CHECK(vega.train_link(dp, 148500, 8, &t) && vega.link_trained(2) && !vega.link_trained(1));
  // a healthy status check does nothing
  auxcard.dig_calls.clear();
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && !ev[0].link_lost && !ev[0].retrained && auxcard.dig_calls.empty());
  // lost: trained again, for the same mode, and the event says so
  dpcd[0x202] = 0x70;
  auxcard.sink.dpcd_writes.clear();
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1);
  CHECK(ev[0].link_lost && ev[0].retrained && !ev[0].retrain_limit && ev[0].retrain.outcome.trained() && ev[0].retrain.config.lanes == 4 && ev[0].retrain.config.rate_khz == 162000);
  CHECK(!auxcard.dig_calls.empty() && auxcard.dig_calls[0].table == 0 && ((auxcard.dig_calls[0].words[0] >> 8) & 0xFF) == 0x08);       // (the training start)
  CHECK(dpcd[0x202] == 0x77 && vega.link_trained(2));
  // the mode it was trained for is the one used again: 4K60 at 10 bits
  CHECK(vega.train_link(dp, 500000, 10, &t) && t.config.lanes == 4 && t.config.rate_khz == 540000);
  dpcd[0x101] = 4;
  dpcd[0x202] = 0x70;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].retrained && ev[0].retrain.config.lanes == 4 && ev[0].retrain.config.rate_khz == t.config.rate_khz);
  // lost again and again: kMaxRetrains in a row, then left alone
  for (uint32_t i = 1; i < AmdVegaM::kMaxRetrains; ++i) {
    dpcd[0x101] = 4;
    dpcd[0x202] = 0x70;
    CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].retrained);
  }
  dpcd[0x101] = 4;
  dpcd[0x202] = 0x70;
  auxcard.dig_calls.clear();
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].link_lost && !ev[0].retrained && ev[0].retrain_limit && auxcard.dig_calls.empty());
  // ... a healthy check starts the count again
  dpcd[0x202] = 0x77;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && !ev[0].link_lost && !ev[0].retrain_limit);
  dpcd[0x202] = 0x70;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].retrained && ev[0].retrain.outcome.trained());
  // a retraining that fails drops the memory (the next loss is only reported)
  heals = false;
  dpcd[0x101] = 4;
  dpcd[0x202] = 0x70;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].retrained && !ev[0].retrain.outcome.trained() && !vega.link_trained(2));
  auxcard.dig_calls.clear();
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].link_lost && !ev[0].retrained && !ev[0].retrain_limit && auxcard.dig_calls.empty());
  // a sink that stops answering drops it too
  heals = true;
  dpcd[0x202] = 0x77;
  CHECK(vega.train_link(dp, 148500, 8, &t) && vega.link_trained(2));
  auxcard.sink.timeouts = 1000;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].read_failed && !ev[0].retrained && !vega.link_trained(2));
  auxcard.sink.timeouts = 0;
  // a failed train_link forgets an earlier one
  CHECK(vega.train_link(dp, 148500, 8, &t) && vega.link_trained(2));
  dpcd[0x202] = dpcd[0x203] = 0;
  heals = false;
  CHECK(!vega.train_link(dp, 148500, 8, &t) && !vega.link_trained(2));
  vega.shutdown();
}

// The short pulse of a DisplayPort sink: queued by the interrupt handler, looked at over the AUX channel by service_short_pulses (link lost when the link set up for lanes is not in sync,
// an automated test request, the other service interrupts, a sink that does not answer, pins that are no DisplayPort connectors).
static void test_short_pulse_status() {
  const std::vector<uint8_t> vbios = make_connector_vbios();   // eDP (DDC line 0x92, pin 0), HDMI-A (0x91, pin 1), DisplayPort (0x93, pin 2)
  Rig rig;
  FakeAuxCard auxcard;
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.hpd_pins = 0x3F;
  in.vbios = ConstByteView(vbios.data(), vbios.size());
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  rig.card.aux = &auxcard;
  std::vector<uint8_t>& dpcd = auxcard.sink.dpcd;
  dpcd.assign(0x800, 0);
  dpcd[0x101] = 0x84;       // 4 lanes (the enhanced framing bit above the count is not part of it)
  dpcd[0x202] = dpcd[0x203] = 0x77;
  dpcd[0x204] = 0x81;
  for (uint32_t pin = 0; pin < 6; ++pin) rig.card.gfx.set_reg(0x1899 + 8 * pin, rig.card.gfx.reg(0x1899 + 8 * pin) & ~0x100000u);
  auto pulse = [&](uint32_t pin) {
    rig.card.gfx.set_reg(0x1898 + 8 * pin, 0x100);
    rig.card.ih.post(42, 6 + pin, 0, 0, 0);
    return vega.service_interrupts();
  };
  AmdVegaM::SinkEvent ev[8];
  CHECK(vega.pending_short_pulses() == 0 && vega.service_short_pulses(ev, 8) == 0 && auxcard.doorbells == 0);
  // A healthy link: nothing to report but the status
  CHECK(pulse(2) == 1 && vega.pending_short_pulses() == 4);
  CHECK(vega.service_short_pulses(ev, 8) == 1 && vega.pending_short_pulses() == 0);
  CHECK(ev[0].pin == 2 && ev[0].has_connector && ev[0].connector.type == AtomConnectors::Type::DisplayPort && ev[0].checked && !ev[0].read_failed);
  CHECK(ev[0].lanes == 4 && !ev[0].link_lost && !ev[0].test_request && !ev[0].content_protection && !ev[0].sink_specific && ev[0].status.link_status_updated());
  CHECK(auxcard.sink.requests.back().channel == 0x93 && auxcard.sink.requests.back().hpd == 2);
  // Link lost: lane 2 lost its clock recovery
  dpcd[0x203] = 0x70;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].checked && ev[0].link_lost && ev[0].lanes == 4 && !ev[0].test_request);
  // ... but only for the lanes set up: with 2 lanes the lost lane 2 does not matter, with 3 it does, and a link that was never set up (0 lanes) is not lost
  dpcd[0x101] = 2;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && !ev[0].link_lost && ev[0].lanes == 2);
  dpcd[0x101] = 3;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].link_lost && ev[0].lanes == 3);
  dpcd[0x101] = 0;
  dpcd[0x204] = 0;       // (and a status that would fail any lane: no lanes, nothing to lose)
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && !ev[0].link_lost && ev[0].lanes == 0 && ev[0].checked);
  dpcd[0x204] = 0x81;
  dpcd[0x101] = 4;
  dpcd[0x203] = 0x77;
  // Not aligned: lost
  dpcd[0x204] = 0x80;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].link_lost);
  dpcd[0x204] = 0x81;
  // The service interrupts: an automated test request, content protection, sink specific; the link is fine
  dpcd[0x201] = 0x02;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].test_request && !ev[0].content_protection && !ev[0].sink_specific && !ev[0].link_lost);
  dpcd[0x201] = 0x04;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && !ev[0].test_request && ev[0].content_protection && !ev[0].sink_specific);
  dpcd[0x201] = 0x40;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && !ev[0].test_request && !ev[0].content_protection && ev[0].sink_specific);
  dpcd[0x201] = 0;
  // Acknowledged and answered. The service interrupts we handle are cleared by writing them back (the sink clears the bits written), the others are left; an EDID read test gets the checksum of
  // the last block of the sink's EDID and ACK with the checksum-write bit, any other test a NAK.
  auxcard.sink.edid = make_edid(1);
  const uint8_t last_checksum = auxcard.sink.edid[255];
  dpcd[0x201] = 0x46 | 0x10;      // test request, content protection, sink specific, and an MST message ready
  dpcd[0x218] = 0x04;             // the EDID read test
  auxcard.sink.dpcd_writes.clear();
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].acked == 0x46 && ev[0].test_request && ev[0].test_kind == 0x04 && ev[0].test_response == 0x05 && !ev[0].handling_failed);
  CHECK(auxcard.sink.dpcd_writes.size() == 3 && auxcard.sink.dpcd_writes[0].address == 0x201 && auxcard.sink.dpcd_writes[0].bytes == std::vector<uint8_t>{0x46});       // (acknowledged first)
  CHECK(auxcard.sink.dpcd_writes[1].address == 0x261 && auxcard.sink.dpcd_writes[1].bytes == std::vector<uint8_t>{last_checksum} && auxcard.sink.dpcd_writes[2].address == 0x260 && auxcard.sink.dpcd_writes[2].bytes == std::vector<uint8_t>{0x05});
  CHECK(dpcd[0x201] == 0x10 && dpcd[0x260] == 0x05);       // (the MST bit is still there: not ours)
  // the status the event shows is what was read before the acknowledgement
  CHECK(ev[0].status.irq_vector == 0x56 && ev[0].content_protection && ev[0].sink_specific);
  // the other tests: NAK, no checksum
  dpcd[0x201] = 0x02;
  const uint8_t tests[] = {0x01, 0x02, 0x08, 0x00, 0x06};
  for (uint8_t t : tests) {
    dpcd[0x218] = t;
    dpcd[0x201] = 0x02;
    auxcard.sink.dpcd_writes.clear();
    CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].test_kind == t && ev[0].test_response == 0x02 && !ev[0].handling_failed);
    CHECK(auxcard.sink.dpcd_writes.size() == 2 && auxcard.sink.dpcd_writes[0].address == 0x201 && auxcard.sink.dpcd_writes[1].address == 0x260 && auxcard.sink.dpcd_writes[1].bytes[0] == 0x02);
  }
  // an EDID read test of a sink with no EDID: NAK
  dpcd[0x218] = 0x04;
  dpcd[0x201] = 0x02;
  auxcard.sink.monitor_present = false;
  auxcard.sink.dpcd_writes.clear();
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].test_kind == 0x04 && ev[0].test_response == 0x02 && !ev[0].handling_failed && auxcard.sink.dpcd_writes.size() == 2);
  auxcard.sink.monitor_present = true;
  // no test request, no test: only the acknowledgement, and none at all without handled bits
  dpcd[0x201] = 0x04;
  auxcard.sink.dpcd_writes.clear();
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].acked == 0x04 && !ev[0].test_request && ev[0].test_kind == 0 && ev[0].test_response == 0 && auxcard.sink.dpcd_writes.size() == 1);
  dpcd[0x201] = 0x38;
  auxcard.sink.dpcd_writes.clear();
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].acked == 0 && auxcard.sink.dpcd_writes.empty() && dpcd[0x201] == 0x38);
  dpcd[0x201] = 0;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].acked == 0 && auxcard.sink.dpcd_writes.empty());
  // a write that is not taken (the DPCD ends before the response): reported, the rest of the event is still there
  dpcd[0x201] = 0x02;
  dpcd[0x218] = 0x04;
  dpcd.resize(0x260);
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].test_request && ev[0].acked == 0x02 && ev[0].test_response == 0 && ev[0].handling_failed);
  dpcd.resize(0x800);
  dpcd[0x201] = 0;
  dpcd[0x218] = 0;
  // the acknowledgement is not taken: reported, nothing acknowledged, the test is answered all the same
  dpcd[0x201] = 0x02;
  dpcd[0x218] = 0x04;
  auxcard.sink.nack_write_at = 0x201;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].acked == 0 && ev[0].handling_failed && ev[0].test_response == 0x05 && dpcd[0x201] == 0x02);
  auxcard.sink.nack_write_at = 0xFFFFFFFFu;
  dpcd[0x201] = 0;
  dpcd[0x218] = 0;
  // The eDP panel (pin 0): looked at too, without switching the panel's power
  CHECK(pulse(0) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].pin == 0 && ev[0].checked && ev[0].connector.type == AtomConnectors::Type::EmbeddedDisplayPort && ev[0].lanes == 4);
  CHECK(auxcard.sink.requests.back().channel == 0x92 && auxcard.panel_writes == 0);
  // A sink that does not answer: reported as such, nothing else in the event
  auxcard.sink.timeouts = 1000;
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].checked && ev[0].read_failed && ev[0].lanes == 0 && !ev[0].link_lost && !ev[0].test_request);
  auxcard.sink.timeouts = 0;
  // ... also when only the status does not come (the DPCD ends before it)
  dpcd.resize(0x203);
  CHECK(pulse(2) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].read_failed && ev[0].lanes == 0 && ev[0].status.align == 0);
  dpcd.resize(0x800);
  // HDMI (pin 1) is no DisplayPort sink, pin 4 is not in the video BIOS's table: reported, not looked at
  const size_t asked = auxcard.sink.requests.size();
  CHECK(pulse(1) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].pin == 1 && ev[0].has_connector && !ev[0].checked && !ev[0].read_failed);
  CHECK(pulse(4) == 1 && vega.service_short_pulses(ev, 8) == 1 && ev[0].pin == 4 && !ev[0].has_connector && !ev[0].checked);
  CHECK(auxcard.sink.requests.size() == asked);
  // Several pending: lowest pin first, as many as there is room for, the rest stays pending; a pin twice is once
  CHECK(pulse(3) == 1 && pulse(2) == 1 && pulse(0) == 1 && pulse(2) == 1 && vega.pending_short_pulses() == 0xD);
  CHECK(vega.service_short_pulses(nullptr, 4) == 0 && vega.service_short_pulses(ev, 0) == 0 && vega.pending_short_pulses() == 0xD);
  CHECK(vega.service_short_pulses(ev, 2) == 2 && ev[0].pin == 0 && ev[1].pin == 2 && vega.pending_short_pulses() == 0x8);
  CHECK(vega.service_short_pulses(ev, 2) == 1 && ev[0].pin == 3 && vega.pending_short_pulses() == 0);
  // A pulse whose status bit is not set is no pulse; shutting down drops what is pending
  rig.card.gfx.set_reg(0x1898 + 16, 0);
  rig.card.ih.post(42, 8, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && vega.pending_short_pulses() == 0);
  CHECK(pulse(2) == 1 && vega.pending_short_pulses() == 4);
  vega.shutdown();
  CHECK(vega.pending_short_pulses() == 0 && vega.service_short_pulses(ev, 8) == 0);
  CHECK(auxcard.sink.malformed == 0);
}

// The connectors in the hot plug events: named from the video BIOS's object table, absent without one or for a pin it does not list.
static void test_hotplug_connectors() {
  const std::vector<uint8_t> vbios = make_connector_vbios();
  Rig rig;
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.hpd_pins = 0x0F;
  {
    std::vector<uint8_t> callers_copy = vbios;
    in.vbios = ConstByteView(callers_copy.data(), callers_copy.size());
    CHECK(vega.bring_up(in));
  }
  if (!vega.up()) return;
  CHECK(vega.connectors().size() == 3);
  AmdVegaM::HotplugEvent e;
  char name[16];
  rig.card.gfx.set_reg(0x1857 + 1, 0x20000);
  rig.card.gfx.set_reg(0x1898 + 8, 0x2);
  rig.card.ih.post(42, 1, 0, 0, 0);
  rig.card.gfx.set_reg(0x1857 + 2, 0x20000);
  rig.card.gfx.set_reg(0x1898 + 16, 0x2);
  rig.card.ih.post(42, 2, 0, 0, 0);
  rig.card.gfx.set_reg(0x1857 + 3, 0x20000);
  rig.card.gfx.set_reg(0x1898 + 24, 0x2);
  rig.card.ih.post(42, 3, 0, 0, 0);
  CHECK(vega.service_interrupts() == 3);
  CHECK(vega.take_hotplug(&e) && e.pin == 1 && e.connected && e.has_connector);
  AtomConnectors::name(e.connector, ArrayView<char>(name, sizeof name));
  CHECK(!strcmp(name, "HDMI-A-1"));
  CHECK(vega.take_hotplug(&e) && e.pin == 2 && e.has_connector);
  AtomConnectors::name(e.connector, ArrayView<char>(name, sizeof name));
  CHECK(!strcmp(name, "DP-1"));
  CHECK(vega.take_hotplug(&e) && e.pin == 3 && !e.has_connector);   // pin 3 is on no connector
  // Without a video BIOS there are no connectors, and the events say so; a later bring-up with one has them again.
  vega.shutdown();
  memset(rig.doorbell_base, 0, 0x800 * 4);
  in.vbios = ConstByteView();
  CHECK(vega.bring_up(in) && vega.connectors().size() == 0);
  rig.card.ih.post(42, 1, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && vega.take_hotplug(&e) && e.pin == 1 && !e.has_connector);
  vega.shutdown();
  memset(rig.doorbell_base, 0, 0x800 * 4);
  std::vector<uint8_t> noise(vbios.size(), 0x5A);
  in.vbios = ConstByteView(noise.data(), noise.size());
  CHECK(vega.bring_up(in) && vega.connectors().size() == 0);
}
// The audio endpoints (8 of them, indexed register pairs 4 apart from 0x17a8/0x17a9): connected unless the port connectivity of the default configuration (index 0x56, bits 31:30) is 1;
// the connectors that carry audio and have a display, named from the video BIOS. (Register numbers and bit positions are literal.)
static void test_audio_pins() {
  const std::vector<uint8_t> vbios = make_connector_vbios();   // eDP on pin 0, HDMI-A on pin 1, DisplayPort on pin 2
  Rig rig;
  // port connectivity per endpoint: 0 connector, 1 none, 2 fixed function, 3 both; only 1 is not connected
  const uint32_t connectivity[8] = {0, 1, 2, 3, 1, 0, 1, 1};
  for (uint32_t p = 0; p < 8; ++p) rig.card.gfx.set_reg(0x17a9 + 4 * p, (connectivity[p] << 30) | (0x00123456u & 0x3FFFFFFFu));
  rig.card.gfx.set_reg(0x1898 + 8, 0x2);   // a display on pin 1 (the HDMI connector)
  rig.card.gfx.set_reg(0x1898 + 16, 0x2);  // and on pin 2 (the DisplayPort)
  rig.card.gfx.set_reg(0x1898, 0x2);       // and on pin 0 (the internal panel)
  AmdVegaM vega(rig.alloc);
  AmdVegaM::Inputs in = rig.inputs();
  in.hpd_pins = 0x0F;
  in.vbios = ConstByteView(vbios.data(), vbios.size());
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  // pins 0, 2, 3, 5 are connected: 0b00101101
  CHECK(vega.audio_pins_connected() == 0x2D);
  // each was asked for the default configuration (index 0x56)
  for (uint32_t p = 0; p < 8; ++p) CHECK(rig.card.gfx.reg(0x17a8 + 4 * p) == 0x56);
  // The outputs that carry audio with a display: the HDMI (pin 1) and the DisplayPort (pin 2), not the eDP on pin 0.
  AtomConnectors::Connector outs[4];
  char name[16];
  CHECK(vega.audio_outputs(outs, 4) == 2);
  AtomConnectors::name(outs[0], ArrayView<char>(name, sizeof name));
  CHECK(!strcmp(name, "HDMI-A-1"));
  AtomConnectors::name(outs[1], ArrayView<char>(name, sizeof name));
  CHECK(!strcmp(name, "DP-1"));
  CHECK(vega.audio_outputs(outs, 1) == 1 && vega.audio_outputs(nullptr, 0) == 0);
  // The DIG encoders from the video BIOS's display paths: eDP on UNIPHY link A (DIG 0, no audio block), HDMI on UNIPHY1 link A (DIG 2), DisplayPort on UNIPHY1 link B (DIG 3). The driver gives
  // the audio blocks the first connected endpoint (pin 0 of 0, 2, 3, 5).
  const ArrayView<const AtomConnectors::Connector> all = vega.connectors();
  CHECK(all.size() == 3 && AtomConnectors::dig_encoder(all[0]) == 0 && AtomConnectors::afmt_instance(all[0]) == AtomConnectors::kNoDig);
  CHECK(AtomConnectors::afmt_instance(all[1]) == 2 && AtomConnectors::afmt_instance(all[2]) == 3);
  uint32_t pin = 99;
  CHECK(!vega.audio_pin_for(all[0], &pin) && pin == 99);
  CHECK(vega.audio_pin_for(all[1], &pin) && pin == 0);
  AtomConnectors::Connector no_encoder = all[1];
  pin = 99;
  no_encoder.encoder_object = 0;      // (an HDMI connector the video BIOS gives no encoder)
  CHECK(!vega.audio_pin_for(no_encoder, &pin) && pin == 99);
  pin = 99;
  CHECK(vega.audio_pin_for(all[2], &pin) && pin == 0 && vega.audio_pin_for(all[2], nullptr));
  // The HDMI display goes away (pin 1 interrupt): only the DisplayPort is left. And the endpoints are read again on request.
  rig.card.gfx.set_reg(0x1857 + 1, 0x20000);
  rig.card.gfx.set_reg(0x1898 + 8, 0);
  rig.card.ih.post(42, 1, 0, 0, 0);
  CHECK(vega.service_interrupts() == 1 && vega.audio_outputs(outs, 4) == 1);
  AtomConnectors::name(outs[0], ArrayView<char>(name, sizeof name));
  CHECK(!strcmp(name, "DP-1"));
  rig.card.gfx.set_reg(0x17a9 + 4 * 1, 0);   // endpoint 1 gets a port
  CHECK(vega.audio_pins_connected() == 0x2D && vega.refresh_audio_pins() == 0x2F && vega.audio_pins_connected() == 0x2F);
  // the first connected endpoint follows the endpoints: pin 0 loses its port, pin 1 (just given one) is the first
  rig.card.gfx.set_reg(0x17a9, 0x40000000);
  CHECK(vega.refresh_audio_pins() == 0x2E && vega.audio_pin_for(vega.connectors()[1], &pin) && pin == 1);
  rig.card.gfx.set_reg(0x17a9 + 4 * 1, 0x40000000);
  CHECK(vega.refresh_audio_pins() == 0x2C && vega.audio_pin_for(vega.connectors()[1], &pin) && pin == 2);
  rig.card.gfx.set_reg(0x17a9 + 4 * 1, 0);
  rig.card.gfx.set_reg(0x17a9, 0);
  rig.card.gfx.set_reg(0x17a9 + 4 * 2, 0x40000000);
  rig.card.gfx.set_reg(0x17a9 + 4 * 3, 0x40000000);
  rig.card.gfx.set_reg(0x17a9 + 4 * 5, 0x40000000);
  CHECK(vega.refresh_audio_pins() == 0x03 && vega.audio_pin_for(vega.connectors()[1], &pin) && pin == 0);
  for (uint32_t p = 0; p < 8; ++p) rig.card.gfx.set_reg(0x17a9 + 4 * p, 0x40000000);
  CHECK(vega.refresh_audio_pins() == 0 && !vega.audio_pin_for(vega.connectors()[1], &pin));     // no endpoint at all
  {
    const uint32_t back[8] = {0, 0, 2, 3, 1, 0, 1, 1};     // (as it was: the endpoints below expect the mask 0x2F)
    for (uint32_t p = 0; p < 8; ++p) rig.card.gfx.set_reg(0x17a9 + 4 * p, (back[p] << 30) | 0x00123456u);
    CHECK(vega.refresh_audio_pins() == 0x2F);
  }
  // The endpoints themselves: a pin that does not exist reads zero and touches nothing, and the mask never goes past the eighth.
  {
    Rig plain;
    for (uint32_t p = 0; p < 8; ++p) plain.card.gfx.set_reg(0x17a9 + 4 * p, 0x40000000);
    amd_dce11::AudioPins pins(*plain.inputs().registers);
    const size_t writes = plain.card.gfx.writes.size();
    CHECK(pins.endpoint_read(8, 0x56) == 0 && pins.endpoint_read(100, 0x56) == 0 && pins.port_connectivity(8) == 0 && !pins.connected(8) && !pins.connected(0xFFFFFFFFu));
    CHECK(plain.card.gfx.writes.size() == writes);
    CHECK(pins.connected_mask(100) == 0 && pins.connected_mask(0) == 0);
    CHECK(pins.port_connectivity(3) == 1 && pins.endpoint_read(3, 0x56) == 0x40000000);
    for (uint32_t p = 0; p < 8; ++p) plain.card.gfx.set_reg(0x17a9 + 4 * p, 0);
    CHECK(pins.connected_mask(100) == 0xFF && pins.connected_mask(3) == 0x7);
  }
  // Stopped: nothing.
  vega.shutdown();
  CHECK(vega.audio_pins_connected() == 0 && vega.refresh_audio_pins() == 0 && vega.audio_outputs(outs, 4) == 0);
  // Without a video BIOS: the endpoints, but no names.
  AmdVegaM bare(rig.alloc);
  AmdVegaM::Inputs bare_in = rig.inputs();
  memset(rig.doorbell_base, 0, 0x800 * 4);
  CHECK(bare.bring_up(bare_in) && bare.audio_pins_connected() == 0x2F && bare.audio_outputs(outs, 4) == 0);
  CHECK(AtomConnectors::carries_audio(AtomConnectors::Type::HdmiA) && AtomConnectors::carries_audio(AtomConnectors::Type::HdmiB) && AtomConnectors::carries_audio(AtomConnectors::Type::DisplayPort));
  CHECK(!AtomConnectors::carries_audio(AtomConnectors::Type::EmbeddedDisplayPort) && !AtomConnectors::carries_audio(AtomConnectors::Type::Vga) && !AtomConnectors::carries_audio(AtomConnectors::Type::Lvds) &&
        !AtomConnectors::carries_audio(AtomConnectors::Type::DviD));
}


// A draw through the whole path: shaders compiled by the GCN compiler, staged in system memory, copied into the card's block by SDMA, the PM4 stream submitted on the graphics ring, the
// ring's indirect buffer run by a model of the command processor (GcnCp, reading the simulated video memory), and the colour buffer copied back; it must equal the same draw done directly
// on the emulator.
struct VramMemory final : gcn::IGpuMemory {
  uint8_t* vram;
  bool in_range(uint64_t address, uint64_t bytes) const { return address >= kGpuBase && address - kGpuBase <= kVideoMemory && bytes <= kVideoMemory - (address - kGpuBase) && (address & 3) == 0; }
  bool read(uint64_t address, ArrayView<uint32_t> out) override {
    if (!in_range(address, out.size() * 4)) return false;
    memcpy(out.data(), vram + (address - kGpuBase), out.size() * 4);
    return true;
  }
  bool write(uint64_t address, ArrayView<const uint32_t> data) override {
    if (!in_range(address, data.size() * 4)) return false;
    memcpy(vram + (address - kGpuBase), data.data(), data.size() * 4);
    return true;
  }
};

static bool compile_gcn(allocators::IAllocator& alloc, const char* text, List<uint32_t>& words, gcn::ShaderInfo* info) {
  gcn::TgsiShader sh(alloc);
  const char* error = nullptr;
  uint32_t line = 0;
  if (!gcn::parse_tgsi(alloc, StringView(text), &sh, &error, &line)) return false;
  gcn::Code code(alloc);
  if (!gcn::compile_tgsi(sh, code, info, &error)) return false;
  for (const uint32_t w : code.words()) words.push_back(w);
  return true;
}

static void test_draw_on_card() {
  Rig rig;
  AmdVegaM vega(rig.alloc);
  Pool pool(8u << 20);
  AmdVegaM::Inputs in = rig.inputs();
  in.system_addresses = &pool;
  CHECK(vega.bring_up(in));
  if (!vega.up()) return;
  const AmdVegaM::Layout& l = vega.layout();
  rig.card.sdma.gart_start = vega.gart_plan().gart_start;
  rig.card.sdma.gart_size = vega.gart_plan().gart_size;
  rig.card.sdma.gart_table = rig.vram + kRegion + l.gart_table;
  rig.card.sdma.system_host = pool.memory.data();
  rig.card.sdma.system_base = Pool::kBase;
  rig.card.sdma.system_bytes = pool.memory.size();

  List<uint32_t> vs(rig.alloc), ps(rig.alloc);
  VegaDrawRequest req;
  CHECK(compile_gcn(rig.alloc,
                    "VERT\nDCL IN[0]\nDCL IN[1]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\nDCL CONST[0..3]\nDCL TEMP[0]\n"
                    "  0: MUL TEMP[0], CONST[0], IN[0].xxxx\n  1: MAD TEMP[0], CONST[1], IN[0].yyyy, TEMP[0]\n  2: MAD TEMP[0], CONST[2], IN[0].zzzz, TEMP[0]\n  3: ADD OUT[0], TEMP[0], CONST[3]\n  4: MOV OUT[1], IN[1]\n  5: END\n",
                    vs, &req.vertex_info) &&
        compile_gcn(rig.alloc, "FRAG\nDCL IN[0], GENERIC[0], PERSPECTIVE\nDCL OUT[0], COLOR\n  0: MOV OUT[0], IN[0]\n  1: END\n", ps, &req.pixel_info));
  const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const float triangle[21] = {-0.8f, -0.7f, 0.5f, 1, 0, 0, 1, 0.9f, -0.5f, 0.5f, 0, 1, 0, 0.5f, -0.1f, 0.8f, 0.5f, 0, 0, 1, 0.25f};
  req.vertex_code = ArrayView<const uint32_t>(vs.view().data(), vs.count());
  req.pixel_code = ArrayView<const uint32_t>(ps.view().data(), ps.count());
  req.vertex_constants = ArrayView<const float>(identity, 16);
  req.vertex_data = ArrayView<const uint32_t>(reinterpret_cast<const uint32_t*>(triangle), 21);      // (floats are 32 bit words)
  req.vertex_stride = 28;
  req.attributes[0] = {3, 0};
  req.attributes[1] = {4, 12};
  req.vertex_count = 3;
  req.width = 37;
  req.height = 29;
  req.clear[0] = req.clear[1] = req.clear[2] = req.clear[3] = -1.0f;

  VramMemory memory;
  memory.vram = rig.vram;
  uint32_t hook_calls = 0, draws = 0;
  gcn::CommandProcessorModel::Result last = gcn::CommandProcessorModel::Result::Ok;
  rig.card.gfx.graphics_ib_hook = [&](uint64_t address, uint32_t dwords) {
    ++hook_calls;
    if (!memory.in_range(address, dwords * 4ull)) return false;
    gcn::CommandProcessorModel cp(rig.alloc, memory);
    last = cp.execute(ArrayView<const uint32_t>(reinterpret_cast<const uint32_t*>(rig.vram + (address - kGpuBase)), dwords));
    draws += cp.draws();
    return last == gcn::CommandProcessorModel::Result::Ok;
  };

  const uint64_t staging_bytes = AmdVegaDraw::area_bytes(req), pixel_bytes = AmdVegaDraw::target_bytes(req);
  CHECK(staging_bytes > 0 && staging_bytes % 256 == 0 && pixel_bytes == 40ull * 29 * 16);
  uint8_t* staging = pool.memory.data() + 4096;
  uint8_t* pixels = pool.memory.data() + 4096 + ((staging_bytes + 8191) / 4096) * 4096;
  const bool ok = AmdVegaDraw::draw(vega, req, 4096, ByteView(staging, staging_bytes), ByteView(pixels, pixel_bytes));
  if (!ok) printf("  draw failed (model: %s)\n", gcn::CommandProcessorModel::result_name(last));
  CHECK(ok && hook_calls == 1 && draws == 1 && rig.card.gfx.ib_protocol_errors == 0);

  // the same draw on the emulator
  gcn::Framebuffer fb(rig.alloc, req.width, req.height);
  fb.clear(-1, -1, -1, -1);
  gcn::DrawCall c;
  c.vertex_code = req.vertex_code;
  c.pixel_code = req.pixel_code;
  c.vertex_info = req.vertex_info;
  c.pixel_info = req.pixel_info;
  c.vertex_constants = req.vertex_constants;
  c.vertex_data = req.vertex_data;
  c.vertex_stride = 28;
  c.attributes[0] = {3, 0};
  c.attributes[1] = {4, 12};
  const uint32_t idx[3] = {0, 1, 2};
  c.indices = ArrayView<const uint32_t>(idx, 3);
  CHECK(gcn::draw(rig.alloc, c, fb) == gcn::DrawStatus::Ok);
  uint32_t different = 0, covered = 0;
  for (uint32_t y = 0; y < req.height; ++y)
    for (uint32_t x = 0; x < req.width; ++x)
      for (uint32_t k = 0; k < 4; ++k) {
        float got;
        memcpy(&got, pixels + (static_cast<uint64_t>(y) * 40 + x) * 16 + 4 * k, 4);
        if (got != fb.pixel(x, y)[k]) ++different;
        if (k == 0 && got != -1.0f) ++covered;
      }
  CHECK(different == 0 && covered > 100);
  // the padding columns of the pitch were cleared, not drawn on
  float pad;
  memcpy(&pad, pixels + (5ull * 40 + 38) * 16, 4);
  CHECK(pad == -1.0f);
  // indexed draw (32 bit indices staged with the vertices)
  const uint32_t order[3] = {0, 2, 1};
  req.indices = ArrayView<const uint32_t>(order, 3);
  CHECK(AmdVegaDraw::draw(vega, req, 4096, ByteView(staging, AmdVegaDraw::area_bytes(req)), ByteView(pixels, pixel_bytes)) && hook_calls == 2 && draws == 2);
  uint32_t covered2 = 0;
  for (uint32_t y = 0; y < req.height; ++y)
    for (uint32_t x = 0; x < req.width; ++x) {
      float v;
      memcpy(&v, pixels + (static_cast<uint64_t>(y) * 40 + x) * 16, 4);
      if (v != -1.0f) ++covered2;
    }
  CHECK(covered2 == covered);          // (the other winding fills the same pixels)
  // what is refused before anything is submitted
  const uint32_t calls = hook_calls;
  CHECK(!AmdVegaDraw::draw(vega, req, 4096 + 16, ByteView(staging, staging_bytes), ByteView(pixels, pixel_bytes)));                          // misaligned
  CHECK(!AmdVegaDraw::draw(vega, req, l.framebuffer_bytes - 256, ByteView(staging, staging_bytes), ByteView(pixels, pixel_bytes)));          // does not fit the area
  CHECK(!AmdVegaDraw::draw(vega, req, 4096, ByteView(staging, 256), ByteView(pixels, pixel_bytes)));                                         // staging too small
  CHECK(!AmdVegaDraw::draw(vega, req, 4096, ByteView(staging, staging_bytes), ByteView(pixels, 16)));                                        // pixels too small
  VegaDrawRequest bad = req;
  bad.width = 0;
  CHECK(AmdVegaDraw::area_bytes(bad) == 0);
  bad = req; bad.vertex_count = 4; bad.indices = ArrayView<const uint32_t>();
  CHECK(!AmdVegaDraw::draw(vega, bad, 4096, ByteView(staging, staging_bytes), ByteView(pixels, pixel_bytes)));                               // not whole triangles
  CHECK(hook_calls == calls);
  vega.shutdown();
  CHECK(!AmdVegaDraw::draw(vega, req, 4096, ByteView(staging, staging_bytes), ByteView(pixels, pixel_bytes)));
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_plan();
  test_bring_up();
  test_transfer();
  test_draw_on_card();
  test_failures();
  test_display();
  test_present();
  test_interrupts();
  test_interrupt_sink();
  test_msi_input();
  test_sq_messages();
  test_display_interrupts();
  test_vblank_interrupt();
  test_vline_interrupt();
  test_video_engine_interrupts();
  test_recovery();
  test_auto_recovery();
  test_recovery_failure();
  test_recovery_failure_sdma();
  test_card_reset();
  test_post_after_reset();
  test_hotplug_events();
  test_hpd_rx();
  test_hotplug_connectors();
  test_audio_pins();
  test_read_edid();
  test_audio_select();
  test_audio_enable();
  test_audio_packets();
  test_audio_end_to_end();
  test_read_dpcd();
  test_train_link();
  test_set_mode();
  test_set_mode_audio();
  test_light_up();
  test_short_pulse_status();
  test_retrain_on_short_pulse();
  printf(g_failed ? "AmdVegaMTest: %d FAILED\n" : "AmdVegaMTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
