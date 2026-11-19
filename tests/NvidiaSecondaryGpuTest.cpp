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

// Host tests for the display driver of a non-boot NVIDIA GPU (NVIDIA/NvidiaSecondaryGpu.*): the modes of an EDID and the one started in, the
// whole start against the GF119 display engine model of NvDisplayModel.hpp (behind it the PRAMIN window and the MMU's invalidate, so the BAR1
// page tables land in the model's VRAM): BAR1 and BAR2 set up and BAR1 mapping VRAM 1:1, the head and SOR programmed for the mode and pointed
// at the surface, drawing through the CPU's view, another mode, a DisplayPort output trained, the refusals before any write (no monitor,
// a display class whose modes are not set here), and the shutdown. The same from scratch on Turing (c57d) and GA10x (c67d): BAR1 on the
// GP100 tables through Turing's BAR/TLB registers, the core at the c37d offsets, the window and window immediate channels, HDMI and DP (the
// per-class SOR registers), the eDP panel on Turing; Ada refused. Then NvidiaSecondaryGpu itself on the fake card of NvTestCard.hpp.
//
// Built twice by tests/run_host_tests.sh: with -DTCOS_NVIDIA_NATIVE (everything above) and without it (NvidiaSecondaryGpuOffTest: the driver
// must refuse before touching the card, so the pool falls back to the probe).
#include <stdio.h>
#include <string.h>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaSecondaryGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplayHw.hpp"
#include "NvBiosImages.hpp"
#include "NvDisplayModel.hpp"
#include "NvTestCard.hpp"

namespace hardware {
void delay_us(uint64_t) {}   // (the drivers' pauses: nothing to wait for against host memory)
}  // namespace hardware

static int g_failed = 0, g_checks = 0;
#define CHECK(...) do { ++g_checks; if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

// --- EDIDs ------------------------------------------------------------------------------------------------------------------------------
void put_timing(uint8_t* d, uint32_t khz, uint32_t ha, uint32_t hb, uint32_t hso, uint32_t hsw, uint32_t va, uint32_t vb, uint32_t vso, uint32_t vsw,
                uint8_t flags = 0x1e) {
  const uint32_t c = khz / 10;
  d[0] = static_cast<uint8_t>(c);
  d[1] = static_cast<uint8_t>(c >> 8);
  d[2] = static_cast<uint8_t>(ha);
  d[3] = static_cast<uint8_t>(hb);
  d[4] = static_cast<uint8_t>((ha >> 8) << 4 | (hb >> 8));
  d[5] = static_cast<uint8_t>(va);
  d[6] = static_cast<uint8_t>(vb);
  d[7] = static_cast<uint8_t>((va >> 8) << 4 | (vb >> 8));
  d[8] = static_cast<uint8_t>(hso);
  d[9] = static_cast<uint8_t>(hsw);
  d[10] = static_cast<uint8_t>((vso & 0xf) << 4 | (vsw & 0xf));
  d[11] = static_cast<uint8_t>(((hso >> 8) & 3) << 6 | ((hsw >> 8) & 3) << 4 | ((vso >> 4) & 3) << 2 | ((vsw >> 4) & 3));
  d[17] = flags;                 // digital separate sync, both positive
}
void put_1080p(uint8_t* d) { put_timing(d, 148500, 1920, 280, 88, 44, 1080, 45, 4, 5); }
void put_720p(uint8_t* d) { put_timing(d, 74250, 1280, 370, 110, 40, 720, 30, 5, 5); }
void put_2160p60(uint8_t* d) { put_timing(d, 594000, 3840, 560, 176, 88, 2160, 90, 8, 10); }
void put_2160p30(uint8_t* d) { put_timing(d, 297000, 3840, 560, 176, 88, 2160, 90, 8, 10); }

void checksum(uint8_t* block) {
  uint32_t sum = 0;
  for (int i = 0; i < 127; ++i) sum += block[i];
  block[127] = static_cast<uint8_t>(256 - (sum & 0xff));
}

// A 1.4 digital monitor; `first`/`second` fill the first two descriptors, a name the third, and with `cta` an extension carrying 2160p30 and
// 720p (a duplicate size) as detailed timings.
Bytes monitor_edid(void (*first)(uint8_t*), void (*second)(uint8_t*), bool cta = false) {
  Bytes e(cta ? 256 : 128, 0);
  uint8_t* b = e.data();
  const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  memcpy(b, header, 8);
  b[8] = 0x52; b[9] = 0x74;                   // "TST"
  b[10] = 0x01;
  b[18] = 1; b[19] = 4;
  b[20] = 0xa5;
  b[21] = 60; b[22] = 34;
  b[23] = 120;
  b[24] = 0x06;                               // preferred timing mode: the first descriptor
  for (int i = 38; i < 54; ++i) b[i] = 1;     // no standard timings
  first(b + 54);
  if (second) second(b + 72);
  b[93] = 0xfc;                               // monitor name
  memcpy(b + 95, "TEST MONITOR\n", 13);
  b[111] = 0x10;                              // dummy descriptor
  if (cta) {
    b[126] = 1;
    uint8_t* x = b + 128;
    x[0] = 0x02; x[1] = 3; x[2] = 4; x[3] = 0;
    put_2160p30(x + 4);
    put_720p(x + 22);
    checksum(x);
  }
  checksum(b);
  return e;
}

struct MutexLock final : INvLock {
  std::mutex m;
  void lock() override { m.lock(); }
  void unlock() override { m.unlock(); }
};

// --- The card ---------------------------------------------------------------------------------------------------------------------------
// The display model, plus the PRAMIN window (BAR0 0x700000 shows the 1 MiB of VRAM at 0x001700 << 16) and the MMU's invalidate status
// (always ready). Every write is logged in order.
struct Card final : INvRegisters {
  FakeMemory mem;
  Model model;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  // Everything in order for the panel tests: 'w' a register write (its offset), 'd' a wait of 10 ms or more (microseconds), 'g' the panel's
  // power line switched (1 on, 0 off), 'a' the first DPCD access after the panel got power.
  std::vector<std::pair<char, uint32_t>> events;
  void delay_us(uint32_t us) override { if (us >= 10000) events.push_back({'d', us}); }
  explicit Card(DisplayClass cls = DisplayClass::Gf119) : model(cls, mem) { model.regs[0x100c80] = 0x00ff8000; }
  static bool pramin(uint32_t off) { return off >= 0x700000 && off < 0x800000; }
  uint64_t pramin_address(uint32_t off) { return (uint64_t{model.regs[0x001700]} << 16) + (off - 0x700000); }
  uint32_t read32(uint32_t off) override {
    if (pramin(off)) {
      const uint64_t a = pramin_address(off);
      return a + 4 <= mem.vram.size() ? mem.rd(a) : 0xffffffffu;
    }
    return model.read32(off);
  }
  void write32(uint32_t off, uint32_t v) override {
    writes.push_back({off, v});
    events.push_back({'w', off});
    if (pramin(off)) {
      const uint64_t a = pramin_address(off);
      if (a + 4 <= mem.vram.size()) mem.wr(a, v);
      return;
    }
    if (off == 0xb830b0) { model.regs[off] = v & ~0x80000000u; return; }   // Turing/GA10x MMU invalidate: done at once
    model.write32(off, v);
  }
  int index_of(uint32_t off) const {
    for (size_t i = 0; i < writes.size(); ++i)
      if (writes[i].first == off) return static_cast<int>(i);
    return -1;
  }
  uint32_t last_write(uint32_t off) const {
    for (size_t i = writes.size(); i-- > 0;)
      if (writes[i].first == off) return writes[i].second;
    return 0xdeadbeef;
  }
  uint32_t px(uint64_t at) const { uint32_t v; memcpy(&v, mem.vram.data() + at, 4); return v; }
};

// A Kepler board: DP-1 (a DP++ port: DP and TMDS outputs on SOR 0), HDMI-1 (SOR 1), DVI-1 (dual-link TMDS on SOR 2), eDP-1 (SOR 3).
BoardInfo kepler_board() {
  BoardInfo b;
  b.connector_count = 4;
  b.connectors[0].type = 0x46;
  b.connectors[1].type = 0x61;
  b.connectors[2].type = 0x30;
  b.connectors[3].type = 0x47;
  auto out = [&](uint32_t i, DcbOutputType type, uint8_t connector, uint8_t sor, uint8_t link) {
    DcbOutput& o = b.outputs[i];
    o.type = type;
    o.connector = connector;
    o.or_mask = static_cast<uint8_t>(1u << sor);
    o.heads = 0x0f;
    o.sorconf_link = link;
    o.i2c_index = connector;
  };
  out(0, DcbOutputType::Dp, 0, 0, 1);
  out(1, DcbOutputType::Tmds, 0, 0, 1);
  out(2, DcbOutputType::Tmds, 1, 1, 1);
  out(3, DcbOutputType::Tmds, 2, 2, 3);
  out(4, DcbOutputType::Dp, 3, 3, 1);
  b.output_count = 5;
  return b;
}

struct Rig {
  Card card;
  BoardInfo board = kepler_board();
  NvidiaNative::Connector connectors[4];
  TestAllocator allocator;
  Clock clock;
  Sink sink;
  MutexLock locks[5];
  explicit Rig(DisplayClass cls = DisplayClass::Gf119) : card(cls) {
    connectors[0].dcb_type = 0x46;
    connectors[0].displayport = true;
    connectors[1].dcb_type = 0x61;
    connectors[2].dcb_type = 0x30;
    connectors[3].dcb_type = 0x47;
    connectors[3].displayport = true;
  }
  void plug(uint32_t connector, const Bytes& edid, EdidSource source) {
    NvidiaNative::Connector& c = connectors[connector];
    c.connected = true;
    c.edid_source = source;
    memcpy(c.edid, edid.data(), edid.size());
    c.edid_bytes = static_cast<uint32_t>(edid.size());
  }
  NvSecondaryDisplay::Inputs inputs(const Chip& chip) {
    NvSecondaryDisplay::Inputs in;
    in.registers = &card;
    in.chip = chip;
    in.board = &board;
    in.connectors = ArrayView<const NvidiaNative::Connector>(connectors);
    in.vram.bytes = card.mem.vram.size();
    in.vram.reserved_head = 1u << 20;
    in.vram.reserved_tail = 1u << 20;
    in.bar1 = ByteView(card.mem.vram.data(), card.mem.vram.size());   // (BAR1 maps VRAM 1:1: the same bytes)
    in.allocator = &allocator;
    in.clock = &clock;
    in.aux = &sink;
    in.timeouts = kQuick;
    in.memory_timeouts.register_us = 50;
    in.locks = Bar1Locks{&locks[0], &locks[1], &locks[2], &locks[3], &locks[4]};
    return in;
  }
};

const Chip kKepler = identify_chip(0x0e4000a1, 0);   // GK104
const Chip kTuring = identify_chip(0x164000a1, 0x16400000);   // TU104
const Chip kAmpere = identify_chip(0x174000a1, 0x17400000);   // GA104
const Chip kAda = identify_chip(0x194000a1, 0x19400000);      // AD104

bool is(const Optional<uint32_t>& got, uint32_t want) { return got && *got == want; }

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------
static void test_edid_modes() {
  DisplayTiming t;
  uint8_t d[18] = {};
  put_1080p(d);
  CHECK(decode_detailed_timing(ConstByteView(d), &t));
  CHECK(t.clock_khz == 148500 && t.hdisplay == 1920 && t.hsync_start == 2008 && t.hsync_end == 2052 && t.htotal == 2200);
  CHECK(t.vdisplay == 1080 && t.vsync_start == 1084 && t.vsync_end == 1089 && t.vtotal == 1125 && !t.hsync_negative && !t.vsync_negative);
  put_timing(d, 74250, 1920, 280, 88, 44, 540, 22, 2, 5, 0x98);   // 1080i, negative syncs
  CHECK(decode_detailed_timing(ConstByteView(d), &t));
  CHECK(t.interlaced && t.vdisplay == 1080 && t.vsync_start == 1084 && t.vsync_end == 1094 && t.vtotal == 1125 && t.hsync_negative && t.vsync_negative);
  HeadRaster r;
  CHECK(compute_head_raster(t, &r) && r.v_total == 1125);
  uint8_t monitor[18] = {0, 0, 0, 0xfc};
  CHECK(!decode_detailed_timing(ConstByteView(monitor), &t));     // a monitor descriptor
  put_1080p(d);
  d[17] = 0x3e;                                                    // stereo
  CHECK(!decode_detailed_timing(ConstByteView(d), &t));
  put_1080p(d);
  d[9] = 0;                                                        // no sync width
  CHECK(!decode_detailed_timing(ConstByteView(d), &t));
  CHECK(!decode_detailed_timing(ConstByteView(d).first(17), &t));  // truncated

  EdidMode modes[8];
  const Bytes edid = monitor_edid(put_1080p, put_720p, true);
  const uint32_t n = edid_modes(ConstByteView(edid.data(), edid.size()), ArrayView<EdidMode>(modes));
  CHECK(n == 3);                                                   // 1080p, 720p, 2160p30 (the extension's 720p is a duplicate)
  CHECK(modes[0].timing.hdisplay == 1920 && modes[0].preferred && modes[0].refresh_millihz == 60000);
  CHECK(modes[1].timing.hdisplay == 1280 && !modes[1].preferred && modes[2].timing.hdisplay == 3840 && modes[2].timing.clock_khz == 297000);
  CHECK(edid_modes(ConstByteView(edid.data(), 100), ArrayView<EdidMode>(modes)) == 0);   // shorter than a block
  CHECK(edid_modes(ConstByteView(edid.data(), edid.size()), ArrayView<EdidMode>(modes).first(1)) == 1);
  // truncation and bit flips anywhere: nothing read outside the bytes (ASan), whatever comes out
  for (size_t cut = 0; cut <= edid.size(); cut += 7) (void)edid_modes(ConstByteView(edid.data(), cut), ArrayView<EdidMode>(modes));
  for (size_t bit = 0; bit < edid.size() * 8; bit += 3) {
    Bytes flipped = edid;
    flipped[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    const uint32_t k = edid_modes(ConstByteView(flipped.data(), flipped.size()), ArrayView<EdidMode>(modes));
    CHECK(k <= 8);
  }

  // The mode to start in: the preferred one if the output carries it, else the biggest that fits.
  DcbOutput tmds;
  tmds.type = DcbOutputType::Tmds;
  tmds.sorconf_link = 1;
  CHECK(max_output_clock_khz(tmds) == 165000);
  tmds.sorconf_link = 3;
  CHECK(max_output_clock_khz(tmds) == 330000);
  DcbOutput dp;
  dp.type = DcbOutputType::Dp;
  CHECK(max_output_clock_khz(dp) == 720000);                      // HBR2 x4 when the DCB says nothing
  dp.dp_max_link_rate = 0x0a;
  dp.dp_max_lanes = 2;
  CHECK(max_output_clock_khz(dp) == 180000);
  CHECK(is(choose_mode(ArrayView<const EdidMode>(modes).first(n), 165000), 0));
  const Bytes uhd = monitor_edid(put_2160p60, put_720p, true);    // 2160p60 preferred, 720p, and 2160p30 in the extension
  const uint32_t m = edid_modes(ConstByteView(uhd.data(), uhd.size()), ArrayView<EdidMode>(modes));
  CHECK(m == 2 && modes[0].preferred && modes[0].timing.clock_khz == 594000);   // (2160p30 is the same size as 2160p60: dropped)
  CHECK(is(choose_mode(ArrayView<const EdidMode>(modes).first(m), 165000), 1));   // 720p: the only one single-link TMDS carries
  CHECK(is(choose_mode(ArrayView<const EdidMode>(modes).first(m), 720000), 0));
  CHECK(!choose_mode(ArrayView<const EdidMode>(modes).first(m), 50000));
}

#ifdef TCOS_NVIDIA_NATIVE
// HDMI from scratch on GF119-family registers: BAR1, the surface, the head and SOR, drawing.
static void test_start_hdmi() {
  Rig rig;
  rig.plug(1, monitor_edid(put_1080p, put_720p), EdidSource::Ddc);
  auto display = std::make_unique<NvSecondaryDisplay>(rig.inputs(kKepler));
  CHECK(display->start() == DisplayStatus::Ok);
  CHECK(display->running() && strcmp(display->status(), "running") == 0);
  CHECK(display->connector() == 1 && display->output().type == DcbOutputType::Tmds && display->route().sor == 1 && display->route().head == 0);
  CHECK(display->mode_count() == 2 && display->current_mode() == 0 && display->width() == 1920 && display->height() == 1080);
  CHECK(display->pitch_bytes() == 7680 && display->surface().offset % 0x10000 == 0 && display->surface().offset >= (1u << 20));
  Card& c = rig.card;
  Model& model = c.model;
  CHECK(model.errors == 0);

  // BAR2, then BAR1, pointed at instance blocks in VRAM; BAR1's page tables map the linear part 1:1.
  Bar1LinearVram* memory = display->memory();
  CHECK(memory && memory->ready() && memory->linear_bytes() == (31u << 20));
  const int bar2 = c.index_of(0x001714), bar1 = c.index_of(0x001704);
  CHECK(bar2 >= 0 && bar1 > bar2);
  CHECK(c.last_write(0x001714) == (0x80000000u | static_cast<uint32_t>(memory->bar().bar2_instance().gpu_address >> 12)));
  CHECK(c.last_write(0x001704) == (0x80000000u | static_cast<uint32_t>(memory->bar().bar1_instance().gpu_address >> 12)));
  CHECK(c.index_of(0x100cbc) > bar1);                               // the TLB invalidated after the tables were written
  const uint64_t surface = display->surface().offset;
  for (const uint64_t va : {surface, surface + 0x123000, uint64_t{1u << 20}, uint64_t{(31u << 20) - 4096}}) {
    const Optional<NvVmm::Translation> tr = memory->bar().bar1().translate(va);
    CHECK(tr && tr->address == va && tr->target == MemoryTarget::Vram);
  }
  CHECK(!memory->bar().bar1().translate(31u << 20));               // nothing past the linear part
  // The page tables lie in the heap (written through PRAMIN), below the linear end like everything else.
  CHECK(memory->bar().bar1().root().gpu_address + memory->bar().bar1().root().bytes <= (31u << 20));

  // The head and the SOR: SOR 1 owned by head 0 as TMDS link A, the 1080p raster at 148.5 MHz, 24 bits, the surface scanned out.
  CHECK(model.core.arm[0x0200 + 1 * 0x20] == (0x1u | 0x1u << 8));
  CHECK(model.core.arm[0x0404] == 5u << 6);                         // positive syncs, 24 bits per pixel
  CHECK(model.core.arm[0x0414] == (1125u << 16 | 2200) && model.core.arm[0x0418] == (4u << 16 | 43));
  CHECK(model.core.arm[0x041c] == (40u << 16 | 191) && model.core.arm[0x0420] == (1120u << 16 | 2111));
  CHECK(model.core.arm[0x0450] == 148500000u && model.core.arm[0x04b8] == (1080u << 16 | 1920));
  CHECK(model.core.arm[0x0468] == (1080u << 16 | 1920) && model.core.arm[0x046c] == (1u << 24 | 7680) && model.core.arm[0x0470] == 0xe6u << 8);
  CHECK(model.scanout[0] == surface);
  CHECK(rig.clock.calls.size() == 1 && rig.clock.calls[0].first == 0 && rig.clock.calls[0].second == 148500);
  CHECK(model.super_acks == 3 && (model.regs[0x6100ac] & 7) == 0);
  CHECK((model.regs[0x612300 + 0x800] & 0x007c0000) == 0x0au << 18);   // the SOR's TMDS link speed

  // Drawing through the CPU's view lands in VRAM where the head reads.
  ByteView px = display->pixels();
  CHECK(px.size() == uint64_t{7680} * 1080);
  px[0] = 0x11; px[1] = 0x22; px[2] = 0x33;
  px[7680 * 1079 + 1919 * 4] = 0x44;
  CHECK(c.px(surface) == 0x00332211 && c.mem.vram[surface + 7680 * 1079 + 1919 * 4] == 0x44);
  const size_t flushes = c.writes.size();
  display->flush();
  CHECK(c.writes.size() == flushes + 1 && c.writes.back().first == 0x070000);   // the BAR write flush
}

// Another mode (720p), back again, and a bad index.
static void test_set_mode() {
  Rig rig;
  rig.plug(1, monitor_edid(put_1080p, put_720p), EdidSource::Ddc);
  auto display = std::make_unique<NvSecondaryDisplay>(rig.inputs(kKepler));
  CHECK(display->start() == DisplayStatus::Ok);
  Model& model = rig.card.model;
  const uint64_t first = display->surface().offset;
  const uint32_t allocations = display->memory()->heap().allocation_count();
  CHECK(display->set_mode(1) == DisplayStatus::Ok);
  CHECK(display->current_mode() == 1 && display->width() == 1280 && display->height() == 720 && display->pitch_bytes() == 5120);
  CHECK(model.core.arm[0x0414] == (750u << 16 | 1650) && model.core.arm[0x0450] == 74250000u);
  CHECK(model.scanout[0] == display->surface().offset && display->surface().offset != first);
  CHECK(display->memory()->heap().allocation_count() == allocations);   // the old surface went back
  CHECK(rig.clock.calls.size() == 2 && rig.clock.calls[1].second == 74250);
  CHECK(display->set_mode(1) == DisplayStatus::Ok);                      // (the same mode: nothing to do)
  CHECK(display->set_mode(2) == DisplayStatus::BadArgument && display->current_mode() == 1);
  CHECK(display->set_mode(0) == DisplayStatus::Ok && model.core.arm[0x0414] == (1125u << 16 | 2200) && model.scanout[0] == display->surface().offset);
  // A mode that does not latch (the armed state never changes): the old one is set again, its surface kept.
  model.never_latch = true;
  const uint64_t kept = display->surface().offset;
  CHECK(display->set_mode(1) != DisplayStatus::Ok);
  CHECK(display->current_mode() == 0 && display->surface().offset == kept && display->memory()->heap().allocation_count() == allocations);
  CHECK(model.errors == 0);
}

// DisplayPort: the DP output of the DP++ port (the EDID came over AUX), trained at HBR2 x4.
static void test_start_dp() {
  Rig rig;
  rig.plug(0, monitor_edid(put_1080p, put_720p, true), EdidSource::Aux);
  auto display = std::make_unique<NvSecondaryDisplay>(rig.inputs(kKepler));
  CHECK(display->start() == DisplayStatus::Ok);
  CHECK(display->connector() == 0 && display->output().type == DcbOutputType::Dp && display->route().sor == 0 && display->route().dp);
  CHECK(display->mode_count() == 3 && display->width() == 1920);       // 2160p30 is on the list over DP
  Model& model = rig.card.model;
  CHECK(model.core.arm[0x0200] == (0x1u | 0x8u << 8));                 // SOR 0, DP link A
  CHECK(rig.sink.dpcd[0x100] == 0x14 && (rig.sink.dpcd[0x101] & 0x1f) == 4);   // trained at HBR2 x4
  CHECK(model.scanout[0] == display->surface().offset && model.errors == 0);
  // The same monitor through the port's TMDS output (an HDMI adapter: the EDID came over DDC): TMDS on the same SOR, 2160p30 not offered.
  Rig hdmi;
  hdmi.plug(0, monitor_edid(put_1080p, put_720p, true), EdidSource::Ddc);
  auto tmds = std::make_unique<NvSecondaryDisplay>(hdmi.inputs(kKepler));
  CHECK(tmds->start() == DisplayStatus::Ok && tmds->output().type == DcbOutputType::Tmds && tmds->mode_count() == 2);
  CHECK(hdmi.card.model.core.arm[0x0200] == (0x1u | 0x1u << 8));
}

// Nothing written when there is nothing to drive, or a display whose modes are not set here.
static void test_refusals() {
  {
    Rig rig;                                                           // no monitor anywhere
    auto display = std::make_unique<NvSecondaryDisplay>(rig.inputs(kKepler));
    CHECK(display->start() == DisplayStatus::NoOutput && !display->running() && strstr(display->status(), "no monitor"));
    CHECK(rig.card.writes.empty());
    rig.plug(3, monitor_edid(put_1080p, put_720p), EdidSource::Aux);   // only the internal panel: the integrated GPU's
    CHECK(display->start() == DisplayStatus::NoOutput && rig.card.writes.empty());
    rig.connectors[1].connected = true;                                // connected, but no EDID came
    CHECK(display->start() == DisplayStatus::NoOutput && rig.card.writes.empty());
    CHECK(display->set_mode(0) == DisplayStatus::NotClaimed);
    CHECK(rig.allocator.live == 1);                                    // (the VRAM heap, kept until the next start or the end)
  }
  {
    Rig rig;                                                           // a monitor whose every mode is too fast for single-link TMDS
    rig.plug(1, monitor_edid(put_2160p60, nullptr), EdidSource::Ddc);
    auto display = std::make_unique<NvSecondaryDisplay>(rig.inputs(kKepler));
    CHECK(display->start() == DisplayStatus::NoOutput && rig.card.writes.empty());
  }
  {
    Rig rig(DisplayClass::Gsp);                                        // Ada: the GSP's (nouveau has no other way)
    rig.plug(1, monitor_edid(put_1080p, put_720p), EdidSource::Ddc);
    auto display = std::make_unique<NvSecondaryDisplay>(rig.inputs(kAda));
    CHECK(display->start() == DisplayStatus::NeedsGsp && strstr(display->status(), "GSP") && rig.card.writes.empty());
    CHECK(!sets_modes(DisplayClass::Gsp) && !sets_modes(DisplayClass::Nv50) && strstr(refusal(DisplayClass::Nv50), "Tesla"));
    CHECK(sets_modes(DisplayClass::Gf119) && sets_modes(DisplayClass::Gv100) && sets_modes(DisplayClass::Tu102) && sets_modes(DisplayClass::Ga102));
  }
  {
    Rig rig(DisplayClass::Nv50);                                       // Tesla: its mode setting is not written
    rig.plug(1, monitor_edid(put_1080p, put_720p), EdidSource::Ddc);
    auto display = std::make_unique<NvSecondaryDisplay>(rig.inputs(identify_chip(0x084000a1, 0)));
    CHECK(display->start() == DisplayStatus::NotImplemented && strstr(display->status(), "Tesla") && rig.card.writes.empty());
  }
  {
    Rig rig;                                                           // too little VRAM for a surface
    rig.plug(1, monitor_edid(put_1080p, put_720p), EdidSource::Ddc);
    NvSecondaryDisplay::Inputs in = rig.inputs(kKepler);
    in.vram.bytes = 8u << 20;
    auto display = std::make_unique<NvSecondaryDisplay>(in);
    CHECK(display->start() == DisplayStatus::NoMemory && rig.card.writes.empty());
    in = rig.inputs(kKepler);
    in.bar1 = in.bar1.first(4u << 20);                                 // a BAR1 mapping too small for the linear part
    auto small = std::make_unique<NvSecondaryDisplay>(in);
    CHECK(small->start() == DisplayStatus::NoMemory && rig.card.writes.empty());
  }
  {
    Rig rig;                                                           // a DP sink that never trains: every mode is tried, then nothing is left lit
    rig.plug(0, monitor_edid(put_1080p, put_720p), EdidSource::Aux);
    rig.sink.broken[0x14] = rig.sink.broken[0x0a] = rig.sink.broken[0x06] = 0xf;
    auto display = std::make_unique<NvSecondaryDisplay>(rig.inputs(kKepler));
    CHECK(display->start() == DisplayStatus::LinkTraining && !display->running());
    CHECK(display->memory() && !display->memory()->ready() && display->memory()->heap().allocation_count() == display->memory()->table_allocations());
  }
}

// Shutdown: the head blanked and its SOR let go, the channels stopped, every VRAM object released, the virtual BARs off.
static void test_shutdown() {
  Rig rig;
  rig.plug(2, monitor_edid(put_1080p, put_720p), EdidSource::Ddc);     // DVI (dual-link TMDS: 1080p still single link)
  auto display = std::make_unique<NvSecondaryDisplay>(rig.inputs(kKepler));
  CHECK(display->start() == DisplayStatus::Ok && display->route().sor == 2 && display->route().link == 1);
  Model& model = rig.card.model;
  CHECK(model.core.arm[0x0200 + 2 * 0x20] == (0x1u | 0x1u << 8) && model.core.arm[0x0474] != 0);
  const uint32_t acks = model.super_acks;
  display->shutdown();
  CHECK(!display->running() && model.core.arm[0x0200 + 2 * 0x20] == 0 && model.core.arm[0x0474] == 0);   // blanked, the SOR free
  CHECK(model.super_acks == acks + 3);                                  // through the supervisor
  CHECK((model.regs[0x610490] & 3) == 0);                               // the core channel stopped
  CHECK((rig.card.last_write(0x001704) & 0x80000000u) == 0 && (rig.card.last_write(0x001714) & 0x80000000u) == 0);
  CHECK(display->memory()->heap().allocation_count() == display->memory()->table_allocations());
  CHECK(display->memory()->heap().bad_releases() == 0 && model.errors == 0);
  const size_t writes = rig.card.writes.size();
  display->shutdown();                                                  // twice: nothing more happens
  CHECK(rig.card.writes.size() == writes);
  CHECK(display->start() == DisplayStatus::Ok && model.scanout[0] == display->surface().offset);   // and up again
  display.reset();
  CHECK(model.errors == 0);                                             // (the restarted core channel's pointers began again at 0)
  CHECK(rig.allocator.live == 0 && (rig.card.last_write(0x001704) & 0x80000000u) == 0);
}

// --- Turing and GA10x from scratch (window channels) ------------------------------------------------------------------------------------

// HDMI on Turing (c57d) or GA104 (c67d): BAR1 through Turing's BAR/TLB registers on GP100 tables, the core methods at the c37d offsets, the
// window channel of head 0's window 0 and its immediate channel, the image scanned out, the per-class SOR clock; another mode; the shutdown.
static void test_window_class_hdmi(const Chip& chip, DisplayClass cls) {
  Rig rig(cls);
  rig.plug(1, monitor_edid(put_1080p, put_720p), EdidSource::Ddc);
  auto display = std::make_unique<NvSecondaryDisplay>(rig.inputs(chip));
  CHECK(display->start() == DisplayStatus::Ok);
  CHECK(display->engine() && display->engine()->display_class() == cls);
  CHECK(display->connector() == 1 && display->output().type == DcbOutputType::Tmds && display->route().sor == 1 && display->route().head == 0);
  CHECK(display->width() == 1920 && display->height() == 1080 && display->pitch_bytes() == 7680);
  Card& c = rig.card;
  Model& model = c.model;
  CHECK(model.errors == 0);

  // BAR2 then BAR1 at 0xb80f48 / 0xb80f40 (not 0x001714 / 0x001704), the TLB invalidated through 0xb830b0 after; GP100 tables 1:1.
  Bar1LinearVram* memory = display->memory();
  CHECK(memory && memory->ready() && memory->linear_bytes() == (31u << 20));
  const int bar2 = c.index_of(0xb80f48), bar1 = c.index_of(0xb80f40);
  CHECK(bar2 >= 0 && bar1 > bar2 && c.index_of(0x001704) < 0 && c.index_of(0x001714) < 0);
  CHECK(c.last_write(0xb80f48) == (0x80000000u | static_cast<uint32_t>(memory->bar().bar2_instance().gpu_address >> 12)));
  CHECK(c.last_write(0xb80f40) == (0x80000000u | static_cast<uint32_t>(memory->bar().bar1_instance().gpu_address >> 12)));
  CHECK(c.index_of(0xb830b0) > bar1 && c.index_of(0x100cbc) < 0);
  CHECK(memory->bar().bar1().layout().format == VmmFormat::Gp100 && memory->bar().bar1().layout().subcontexts);
  const uint64_t surface = display->surface().offset;
  for (const uint64_t va : {surface, surface + 0x123000, uint64_t{1u << 20}, uint64_t{(31u << 20) - 4096}}) {
    const Optional<NvVmm::Translation> tr = memory->bar().bar1().translate(va);
    CHECK(tr && tr->address == va && tr->target == MemoryTarget::Vram);
  }
  CHECK(!memory->bar().bar1().translate(31u << 20));
  // The instance block: the PDB with the GP100 "version 2" and 64 KiB bits, and the subcontext PDBs (gv100_vmm_join).
  const uint64_t inst = memory->bar().bar1_instance().gpu_address;
  CHECK((c.px(inst + 0x200) & 0xc00) == 0xc00 && c.px(inst + 0x298) != 0);

  // The display engine: 8 KiB RAMHT, the core channel and window 0's channels (window slot 1, immediate slot 33)
  CHECK(model.regs[0x610010] == 0x9 && (model.regs[0x610b00] & 0x30000) == 0x10000);
  CHECK(model.regs[0x610b00 + 1 * 0x10] == 0x11 && model.regs[0x610b00 + 33 * 0x10] == 0x11);
  CHECK(model.regs[0x640008] == 0x21);                                        // tu102_disp_init's lock pins (also GA102)
  // The core: SOR 1 owned by head 0 as TMDS A, the windows' owners, the head at 0x2000.
  CHECK(model.core.arm[0x0300 + 1 * 0x20] == (0x1u | 0x1u << 8));
  CHECK(model.core.arm[0x1000] == 0 && model.core.arm[0x1000 + 0x80] == 0 && model.core.arm[0x1000 + 2 * 0x80] == 1);
  CHECK(model.core.arm[0x1004] == 0x0f && model.core.arm[0x1010] == 0x00117fff);   // corec57d's usage bounds
  CHECK(model.core.arm[0x2004] == (1u | 4u << 4));                                  // positive syncs, 24 bpp
  CHECK(model.core.arm[0x2064] == (1125u << 16 | 2200) && model.core.arm[0x2068] == (4u << 16 | 43));
  CHECK(model.core.arm[0x206c] == (40u << 16 | 191) && model.core.arm[0x2070] == (1120u << 16 | 2111));
  CHECK(model.core.arm[0x200c] == 148500000u && model.core.arm[0x2028] == 148500000u && model.core.arm[0x2030] == 0x1014);
  CHECK(model.core.arm[0x204c] == (1080u << 16 | 1920));
  // The window: the surface pitch-linear X8R8G8B8, the VRAM context DMA, scanned out; placed at 0,0 by its immediate channel.
  CHECK(model.win[0].arm[0x0224] == (1080u << 16 | 1920) && model.win[0].arm[0x0228] == 1u << 4 && model.win[0].arm[0x022c] == 0xe6);
  CHECK(model.win[0].arm[0x0230] == 7680u / 64 && model.win[0].arm[0x0240] == 0xf0000001u && model.win[0].arm[0x0260] == surface >> 8);
  CHECK(model.window_scanout[0] == surface);
  CHECK(model.win[Model::kWimm].updates == 1 && Model::has_method(model.win[Model::kWimm], 0x0208, 0));
  CHECK(rig.clock.calls.size() == 1 && rig.clock.calls[0].first == 0 && rig.clock.calls[0].second == 148500);
  CHECK(model.super_acks == 3 && (model.regs[0x611c30] & 7) == 0);
  if (cls == DisplayClass::Ga102) CHECK(c.index_of(0x00ec04 + 0x10) >= 0 && (model.regs[0x612300 + 0x800] & 0x007c0000) == 0);   // ga102_sor_clock
  else CHECK((model.regs[0x612300 + 0x800] & 0x007c0000) == 0x0au << 18);                                                         // gf119_sor_clock

  // Drawing through the CPU's view lands where the window reads.
  ByteView px = display->pixels();
  px[0] = 0x11; px[1] = 0x22; px[2] = 0x33;
  CHECK(c.px(surface) == 0x00332211);
  display->flush();
  CHECK(c.writes.back().first == 0x070000);

  // Another mode: the window shows the new surface, the old one went back.
  const uint32_t allocations = display->memory()->heap().allocation_count();
  CHECK(display->set_mode(1) == DisplayStatus::Ok && display->width() == 1280);
  CHECK(model.core.arm[0x2064] == (750u << 16 | 1650) && model.window_scanout[0] == display->surface().offset && display->surface().offset != surface);
  CHECK(display->memory()->heap().allocation_count() == allocations && rig.clock.calls.size() == 2);
  CHECK(model.errors == 0);

  // Shutdown: the window's image off, the SOR let go through the supervisor, every channel stopped, both BARs off.
  const uint32_t acks = model.super_acks;
  display->shutdown();
  CHECK(model.win[0].arm[0x0240] == 0 && model.window_scanout[0] == 0 && model.core.arm[0x0300 + 0x20] == 0 && model.super_acks == acks + 3);
  CHECK((model.regs[0x610b00] & 3) == 0 && (model.regs[0x610b10] & 3) == 0 && (model.regs[0x610b00 + 33 * 0x10] & 3) == 0);
  CHECK((c.last_write(0xb80f40) & 0x80000000u) == 0 && (c.last_write(0xb80f48) & 0x80000000u) == 0);
  CHECK(display->memory()->heap().allocation_count() == display->memory()->table_allocations() && display->memory()->heap().bad_releases() == 0);
  CHECK(display->start() == DisplayStatus::Ok && model.window_scanout[0] == display->surface().offset);   // and up again
  display.reset();
  CHECK(model.errors == 0);
  CHECK(rig.allocator.live == 0);
}

// DisplayPort on the DP++ port: trained at HBR2 x4 with the class's SOR registers (Turing: the code and the link enables after 40 ms;
// GA102: the rate as index 2), lanes 0..3 in order, the DP watermark at gv100's register.
static void test_window_class_dp(const Chip& chip, DisplayClass cls) {
  Rig rig(cls);
  rig.plug(0, monitor_edid(put_1080p, put_720p, true), EdidSource::Aux);
  auto display = std::make_unique<NvSecondaryDisplay>(rig.inputs(chip));
  CHECK(display->start() == DisplayStatus::Ok);
  CHECK(display->connector() == 0 && display->output().type == DcbOutputType::Dp && display->route().sor == 0 && display->mode_count() == 3);
  Model& model = rig.card.model;
  CHECK(model.core.arm[0x0300] == (0x1u | 0x8u << 8));                       // SOR 0, DP link A
  CHECK(rig.sink.dpcd[0x100] == 0x14 && (rig.sink.dpcd[0x101] & 0x1f) == 4);
  const uint32_t clk = model.regs[0x612300];
  CHECK(((clk >> 18) & 0x1f) == (cls == DisplayClass::Ga102 ? 2u : 0x14u));
  CHECK((clk & 0x30000) == 0x10000 && (model.regs[0x61c10c] & 3) == 1 && (model.regs[0x61c10c] & 0x001f0000) == 0x000f0000);
  CHECK((model.regs[0x61c130] & 0xf) == 0xf);
  bool waited = false;
  for (const auto& e : rig.card.events) waited |= e.first == 'd' && e.second == 40000;
  CHECK(waited);                                                              // tu102/ga102_sor_dp_links' 40 ms
  CHECK((model.regs[0x616550] & 0x08000000) != 0);                            // gv100_sor_dp_watermark (head 0)
  CHECK(model.window_scanout[0] == display->surface().offset && model.errors == 0);
}

// The built-in eDP panel on a muxed Turing laptop: the power sequence as on Kepler, the panel's rate table, SOR 3 as DP A at the c57d
// offset, the window scanned out, the backlight; the shutdown in order.
static void test_panel_edp_turing();

// --- The built-in panel (a muxed laptop's discrete GPU) ----------------------------------------------------------------------------------
namespace {
// The panel's power line (DCB GPIO function 0x01); `readable` false: an INvGpio that cannot read it back; `present` false: the board has none.
struct PanelGpio final : INvGpio {
  Card* card = nullptr;
  bool on = false, present = true, readable = true, stuck = false;
  uint32_t sets = 0;
  bool set_function(uint8_t function, bool value) override {
    if (function != 0x01 || !present || stuck) return false;
    ++sets;
    on = value;
    if (card) card->events.push_back({'g', value ? 1u : 0u});
    return true;
  }
  Optional<bool> get_function(uint8_t function) override {
    if (function != 0x01 || !present || !readable) return Optional<bool>();
    return on;
  }
};

// An eDP panel's DPCD: answers only while powered (`gpio->on`), eDP 1.4 with a rate table; clock recovery fails on the lanes of `broken`
// at an effective rate (the bandwidth code, or the table entry's rate / 27 MHz when DP_LINK_RATE_SET was used).
struct EdpSink final : IDpcdAux {
  std::vector<uint8_t> dpcd = std::vector<uint8_t>(0x800, 0);
  std::map<uint8_t, uint8_t> broken;
  PanelGpio* gpio = nullptr;
  Card* card = nullptr;
  bool told = false;
  uint32_t unpowered = 0;
  EdpSink() {
    dpcd[0x000] = 0x14;
    dpcd[0x001] = 0x14;
    dpcd[0x002] = 0x84;
    dpcd[0x003] = 0x01;
    dpcd[0x006] = 0x01;
    dpcd[0x700] = 0x03;                      // eDP 1.4
    // the rate table in DPCD units (200 kHz of bit rate): 1.62, 2.7, 3.24, 5.4 Gbit/s
    const uint16_t rates[] = {8100, 13500, 16200, 27000};
    for (int i = 0; i < 4; ++i) {
      dpcd[0x010 + i * 2] = static_cast<uint8_t>(rates[i]);
      dpcd[0x011 + i * 2] = static_cast<uint8_t>(rates[i] >> 8);
    }
  }
  uint8_t effective_rate() const {
    if (dpcd[0x100]) return dpcd[0x100];
    const uint32_t i = dpcd[0x115];
    if (i >= 8) return 0;
    const uint32_t khz = (dpcd[0x010 + i * 2] | uint32_t{dpcd[0x011 + i * 2]} << 8) * 200u / 10;   // the link clock
    return static_cast<uint8_t>(khz / 27000);
  }
  void model() {
    const uint8_t rate = effective_rate(), lanes = dpcd[0x101] & 0x1f, pattern = dpcd[0x102] & 3;
    bool all = true;
    for (uint32_t l = 0; l < 4; ++l) {
      uint8_t nibble = 0;
      if (l < lanes) {
        const bool ok = pattern != 0 && !(broken[rate] & (1u << l));
        nibble = ok ? (pattern >= 2 ? 7 : 1) : 0;
        if (!ok || pattern < 2) all = false;
      }
      const uint32_t at = 0x202 + l / 2, shift = l % 2 * 4;
      dpcd[at] = static_cast<uint8_t>((dpcd[at] & ~(0xf << shift)) | (nibble << shift));
    }
    dpcd[0x204] = all ? 1 : 0;
    dpcd[0x206] = dpcd[0x207] = 0;
  }
  bool powered() {
    if (gpio && !gpio->on && gpio->present) {
      ++unpowered;
      return false;
    }
    if (!told && card) card->events.push_back({'a', 0});
    told = true;
    return true;
  }
  bool dpcd_read(uint32_t address, ByteView out) override {
    if (!powered() || address + out.size() > dpcd.size()) return false;
    if (address == 0x200 || address == 0x202) model();
    for (size_t i = 0; i < out.size(); ++i) out[i] = dpcd[address + i];
    return true;
  }
  bool dpcd_write(uint32_t address, ConstByteView data) override {
    if (!powered() || address + data.size() > dpcd.size()) return false;
    for (size_t i = 0; i < data.size(); ++i) dpcd[address + i] = data[i];
    return true;
  }
};

// The EDID once the panel has power (an eDP panel's AUX answers only then).
struct PanelEdid final : INvPanelEdid {
  Bytes edid;
  PanelGpio* gpio = nullptr;
  uint32_t reads = 0, unpowered = 0;
  uint32_t read_panel_edid(uint32_t connector, ByteView out) override {
    ++reads;
    if (connector != 3 || (gpio && gpio->present && !gpio->on)) {
      ++unpowered;
      return 0;
    }
    const size_t n = edid.size() < out.size() ? edid.size() : out.size();
    out.first(n).copy_from(ConstByteView(edid.data(), n));
    return static_cast<uint32_t>(n);
  }
};

uint64_t g_now_us = 0;
uint64_t fake_now() { return g_now_us; }

// The Kepler board of the other tests, its eDP-1 (connector 3, DP on SOR 3) as the panel.
struct PanelRig {
  Rig rig;
  Chip chip;
  PanelGpio gpio;
  EdpSink sink;
  PanelEdid edid;
  explicit PanelRig(DisplayClass cls = DisplayClass::Gf119, const Chip& c = kKepler) : rig(cls), chip(c) {
    gpio.card = &rig.card;
    sink.gpio = &gpio;
    sink.card = &rig.card;
    edid.gpio = &gpio;
    edid.edid = monitor_edid(put_1080p, put_720p);
    rig.card.model.regs[0x61c080 + 3 * 0x800] = 0x1000;   // the SOR's backlight PWM divider
    rig.card.model.regs[0x61c084 + 3 * 0x800] = 0x0800;   // ... at half
  }
  NvSecondaryDisplay::Inputs inputs() {
    NvSecondaryDisplay::Inputs in = rig.inputs(chip);
    in.panel = true;
    in.gpio = &gpio;
    in.aux = &sink;
    in.panel_edid = &edid;
    return in;
  }
  std::vector<std::pair<char, uint32_t>>& events() { return rig.card.events; }
  int find(char kind, uint32_t value, int from = 0) const {
    const auto& e = rig.card.events;
    for (size_t i = static_cast<size_t>(from); i < e.size(); ++i)
      if (e[i].first == kind && e[i].second == value) return static_cast<int>(i);
    return -1;
  }
  int find_write(uint32_t off, int from = 0) const { return find('w', off, from); }
};

constexpr uint32_t kPwmCtl3 = 0x61c084 + 3 * 0x800;
}  // namespace

// eDP: the power line before any AUX transaction, T1+T3, the EDID read once powered, link training on the panel's rate table, T8, the
// backlight; the reverse on shutdown with T9 and T10; the power cycle time before the next start.
static void test_panel_edp() {
  PanelRig p;
  auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
  CHECK(display->start() == DisplayStatus::Ok);
  CHECK(display->panel() && display->panel_result() == PanelLightStatus::Ok && display->connector() == 3);
  CHECK(display->output().type == DcbOutputType::Dp && display->route().sor == 3 && display->route().dp && display->width() == 1920);
  Model& model = p.rig.card.model;
  CHECK(model.errors == 0 && model.scanout[0] == display->surface().offset);
  CHECK(model.core.arm[0x0200 + 3 * 0x20] == (0x1u | 0x8u << 8));          // SOR 3 as DP link A on head 0
  // Order: power line, T1+T3 (210 ms), then the first DPCD access, the mode set, T8 (50 ms), the PWM.
  const int g1 = p.find('g', 1), up = p.find('d', 210000), aux = p.find('a', 0), t8 = p.find('d', 50000), bl = p.find_write(kPwmCtl3);
  CHECK(g1 >= 0 && up > g1 && aux > up && t8 > aux && bl > t8);
  CHECK(p.find_write(0x6101d0) > aux && p.find_write(0x6101d0) < t8);       // the supervisor ran between training and the backlight
  CHECK(p.sink.unpowered == 0 && p.edid.reads == 1 && p.edid.unpowered == 0);
  CHECK(model.regs[kPwmCtl3] == (0x0800u | 0xc0000000u));                  // the level the panel had, NEW | bit 30
  CHECK(display->panel_power()->powered() && display->panel_power()->switched_line() && display->panel_power()->backlight());
  CHECK(display->panel_power()->waited_ms() == 260);
  // eDP 1.4: the fastest entry of the panel's table (5.4 Gbit/s, index 3) set by DP_LINK_RATE_SET with DP_LINK_BW_SET 0; the SOR at 0x14.
  CHECK(p.sink.dpcd[0x100] == 0 && p.sink.dpcd[0x115] == 3 && (p.sink.dpcd[0x101] & 0x1f) == 4);
  CHECK((model.regs[0x612300 + 3 * 0x800] & 0x007c0000) == 0x14u << 18);

  // Shutdown: PWM to 0, T9, the head blanked and the SOR let go, T10, then the power line off.
  const size_t before = p.events().size();
  display->shutdown();
  const int off = p.find_write(kPwmCtl3, static_cast<int>(before)), t9 = p.find('d', 50000, off), t10 = p.find('d', 500000, off),
            g0 = p.find('g', 0, off);
  const int blank = p.find_write(0x6101d0, off);
  CHECK(off >= 0 && t9 > off && blank > t9 && t10 > blank && g0 > t10);
  CHECK(model.regs[kPwmCtl3] == 0xc0000000u && !p.gpio.on && model.core.arm[0x0200 + 3 * 0x20] == 0);
  CHECK(!display->panel_power()->powered() && !display->panel_power()->backlight());

  // Up again at once: without a clock the whole power cycle (T11+T12, 510 ms) comes before the power line.
  const size_t again = p.events().size();
  CHECK(display->start() == DisplayStatus::Ok);
  const int cycle = p.find('d', 510000, static_cast<int>(again)), on = p.find('g', 1, static_cast<int>(again));
  CHECK(cycle >= 0 && on > cycle);
  CHECK(model.regs[kPwmCtl3] == (0x0800u | 0xc0000000u));                  // the level kept across the release
  display.reset();                                                         // (the destructor turns it off)
  CHECK(!p.gpio.on);
  CHECK(model.errors == 0);
}

// The power cycle with a clock: only what is left of it is waited; a power line that was on already is left alone, and one the GpioI cannot
// read back is taken as ours.
static void test_panel_power_cases() {
  {
    PanelRig p;
    NvSecondaryDisplay::Inputs in = p.inputs();
    in.now_us = fake_now;
    g_now_us = 1000000;
    auto display = std::make_unique<NvSecondaryDisplay>(in);
    CHECK(display->start() == DisplayStatus::Ok);
    display->shutdown();
    g_now_us += 200000;                                                    // 200 ms later: 310 ms of the cycle left
    size_t mark = p.events().size();
    CHECK(display->start() == DisplayStatus::Ok && p.find('d', 310000, static_cast<int>(mark)) >= 0);
    CHECK(p.find('d', 510000, static_cast<int>(mark)) < 0);
    display->shutdown();
    g_now_us += 2000000;                                                   // long enough: nothing to wait
    mark = p.events().size();
    CHECK(display->start() == DisplayStatus::Ok);
    const int first_g = p.find('g', 1, static_cast<int>(mark));
    bool waited_before = false;
    for (int i = static_cast<int>(mark); i < first_g; ++i) waited_before |= p.events()[static_cast<size_t>(i)].first == 'd';
    CHECK(first_g >= 0 && !waited_before);
  }
  {
    PanelRig p;                                                            // the firmware left the panel powered
    p.gpio.on = true;
    auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
    CHECK(display->start() == DisplayStatus::Ok && p.gpio.sets == 0 && !display->panel_power()->switched_line());
    CHECK(display->panel_power()->has_line() && p.find('d', 210000) >= 0);   // the wait is made anyway (nouveau)
    display->shutdown();
    CHECK(p.gpio.on && p.gpio.sets == 0);                                  // not switched off: this did not switch it on
  }
  {
    PanelRig p;                                                            // a line that cannot be read back
    p.gpio.readable = false;
    auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
    CHECK(display->start() == DisplayStatus::Ok && display->panel_power()->switched_line());
    display->shutdown();
    CHECK(!p.gpio.on && p.gpio.sets == 2);
  }
  {
    PanelRig p;                                                            // no power line on the board: only the waits
    p.gpio.present = false;
    auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
    CHECK(display->start() == DisplayStatus::Ok && !display->panel_power()->has_line() && p.find('d', 210000) >= 0);
  }
  {
    PanelRig p;                                                            // the line will not switch: refused before anything is lit
    p.gpio.stuck = true;
    auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
    CHECK(display->start() == DisplayStatus::NoOutput && display->panel_result() == PanelLightStatus::PowerFailed && !display->running());
    CHECK(p.find_write(kPwmCtl3) < 0 && p.rig.card.model.core.arm[0x0200 + 3 * 0x20] == 0);
  }
  {
    PanelRig p;                                                            // no backlight PWM on the SOR: nothing written there
    p.rig.card.model.regs[0x61c080 + 3 * 0x800] = 0;
    auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
    CHECK(display->start() == DisplayStatus::Ok && !display->panel_power()->pwm() && p.find_write(kPwmCtl3) < 0);
  }
  // The timings themselves.
  const PanelPowerTimings e = PanelPowerTimings::edp(), l = PanelPowerTimings::lvds();
  CHECK(e.power_up_ms == 210 && e.backlight_on_ms == 50 && e.backlight_off_ms == 50 && e.power_down_ms == 500 && e.power_cycle_ms == 510);
  CHECK(l.power_up_ms == 60 && l.backlight_on_ms == 200 && l.backlight_off_ms == 200 && l.power_down_ms == 50);
}

// The failures: no EDID, a link that never trains, no panel on the board; each leaves the panel unpowered and nothing lit.
static void test_panel_failures() {
  {
    PanelRig p;
    p.edid.edid.clear();                                                   // the panel sends nothing (and none was read at start)
    auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
    CHECK(display->start() == DisplayStatus::NoOutput && display->panel_result() == PanelLightStatus::NoEdid && !display->running());
    CHECK(!p.gpio.on && p.find('g', 0) > p.find('g', 1) && p.find_write(kPwmCtl3) < 0);
    CHECK(display->memory()->heap().allocation_count() == display->memory()->table_allocations());
  }
  {
    PanelRig p;                                                            // the EDID came at start (LVDS-style, over DDC) and is used
    p.edid.edid.clear();
    p.rig.plug(3, monitor_edid(put_720p, nullptr), EdidSource::Aux);
    auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
    CHECK(display->start() == DisplayStatus::Ok && display->width() == 1280);
  }
  {
    PanelRig p;                                                            // every rate of the table fails
    for (uint8_t r : {uint8_t{0x06}, uint8_t{0x0a}, uint8_t{0x0c}, uint8_t{0x14}}) p.sink.broken[r] = 0xf;
    auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
    CHECK(display->start() == DisplayStatus::LinkTraining && display->panel_result() == PanelLightStatus::LinkTraining);
    CHECK(!p.gpio.on && p.find_write(kPwmCtl3) < 0 && !display->running());
  }
  {
    PanelRig p;                                                            // the fastest entry fails: 3.24 Gbit/s (index 2, SOR 0x0c) carries 1080p
    p.sink.broken[0x14] = 0xf;
    auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
    CHECK(display->start() == DisplayStatus::Ok && p.sink.dpcd[0x115] == 2 && p.sink.dpcd[0x100] == 0);
    CHECK((p.rig.card.model.regs[0x612300 + 3 * 0x800] & 0x007c0000) == 0x0cu << 18);
  }
  {
    PanelRig p;                                                            // the same table written again (8100 = 1.62 Gbit/s ... 27000 = 5.4): the fastest is used
    const uint16_t real[] = {8100, 13500, 16200, 27000};
    for (int i = 0; i < 4; ++i) {
      p.sink.dpcd[0x010 + i * 2] = static_cast<uint8_t>(real[i]);
      p.sink.dpcd[0x011 + i * 2] = static_cast<uint8_t>(real[i] >> 8);
    }
    auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
    CHECK(display->start() == DisplayStatus::Ok && p.sink.dpcd[0x115] == 3);
    CHECK((p.rig.card.model.regs[0x612300 + 3 * 0x800] & 0x007c0000) == 0x14u << 18);
  }
  {
    PanelRig p;                                                            // a board without a panel output
    p.rig.board.connectors[3].type = 0x61;
    p.rig.connectors[3].dcb_type = 0x61;
    auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
    CHECK(display->start() == DisplayStatus::NoOutput && display->panel_result() == PanelLightStatus::NoPanel && p.rig.card.writes.empty());
    CHECK(!find_panel_output(p.rig.board, ArrayView<const NvidiaNative::Connector>(p.rig.connectors), nullptr, nullptr));
  }
}

static void test_panel_edp_turing() {
  PanelRig p(DisplayClass::Tu102, kTuring);
  auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
  CHECK(display->start() == DisplayStatus::Ok);
  CHECK(display->panel() && display->panel_result() == PanelLightStatus::Ok && display->connector() == 3 && display->route().sor == 3);
  Model& model = p.rig.card.model;
  CHECK(model.errors == 0 && model.window_scanout[0] == display->surface().offset);
  CHECK(model.core.arm[0x0300 + 3 * 0x20] == (0x1u | 0x8u << 8));          // SOR 3 as DP link A on head 0
  const int g1 = p.find('g', 1), up = p.find('d', 210000), aux = p.find('a', 0), t8 = p.find('d', 50000), bl = p.find_write(kPwmCtl3);
  CHECK(g1 >= 0 && up > g1 && aux > up && t8 > aux && bl > t8);
  CHECK(p.find_write(0x6107a8) > aux && p.find_write(0x6107a8) < t8);       // the (Volta+) supervisor ran between training and the backlight
  CHECK(p.sink.dpcd[0x100] == 0 && p.sink.dpcd[0x115] == 3 && (p.sink.dpcd[0x101] & 0x1f) == 4);
  CHECK((model.regs[0x612300 + 3 * 0x800] & 0x007c0000) == 0x14u << 18 && (model.regs[0x612300 + 3 * 0x800] & 0x30000) == 0x10000);
  CHECK(model.regs[kPwmCtl3] == (0x0800u | 0xc0000000u));
  const size_t before = p.events().size();
  display->shutdown();
  const int off = p.find_write(kPwmCtl3, static_cast<int>(before)), t9 = p.find('d', 50000, off), t10 = p.find('d', 500000, off),
            g0 = p.find('g', 0, off), blank = p.find_write(0x6107a8, off);
  CHECK(off >= 0 && t9 > off && blank > t9 && t10 > blank && g0 > t10);
  CHECK(!p.gpio.on && model.core.arm[0x0300 + 3 * 0x20] == 0 && model.window_scanout[0] == 0 && model.errors == 0);
  // GA10x: the same panel, the 5.4 Gbit/s entry as GA102's rate index 2
  PanelRig a(DisplayClass::Ga102, kAmpere);
  auto ga = std::make_unique<NvSecondaryDisplay>(a.inputs());
  CHECK(ga->start() == DisplayStatus::Ok && a.sink.dpcd[0x115] == 3);
  CHECK((a.rig.card.model.regs[0x612300 + 3 * 0x800] & 0x007c0000) == 2u << 18 && a.rig.card.model.errors == 0);
}

// LVDS: the DCB's LVDS output on a SOR, the SPWG waits, the LVDS protocol on the SOR.
static void test_panel_lvds() {
  PanelRig p;
  BoardInfo& b = p.rig.board;
  b.connectors[3].type = 0x40;
  p.rig.connectors[3].dcb_type = 0x40;
  p.rig.connectors[3].displayport = false;
  b.outputs[4].type = DcbOutputType::Lvds;
  b.outputs[4].sorconf_link = 1;
  uint32_t connector = 0;
  DcbOutput out;
  CHECK(find_panel_output(b, ArrayView<const NvidiaNative::Connector>(p.rig.connectors), &connector, &out) && connector == 3 && out.type == DcbOutputType::Lvds);
  auto display = std::make_unique<NvSecondaryDisplay>(p.inputs());
  CHECK(display->start() == DisplayStatus::Ok && display->route().panel && !display->route().dp && display->route().protocol == disp_hw::kProtocolLvds);
  CHECK(p.rig.card.model.core.arm[0x0200 + 3 * 0x20] == 0x1u);              // SOR 3 owned by head 0, protocol LVDS (0)
  CHECK(p.find('d', 60000) >= 0 && p.find('d', 200000) > p.find('d', 60000) && p.sink.told == false);   // no AUX for LVDS
  CHECK(display->panel_power()->waited_ms() == 260);
  display->shutdown();
  CHECK(!p.gpio.on && p.find('d', 200000, p.find_write(kPwmCtl3, p.find('d', 200000) + 1)) >= 0);
}
#endif  // TCOS_NVIDIA_NATIVE

// The VPLL adapter over Nvkm/NvPll: heads past the fourth and a zero clock refused; otherwise what nv_pll_set says (this image has no PLL
// limits table, so nothing may be written).
static void test_vpll() {
  Card card;
  const Bytes image = kepler_image(false);
  NvVpllClock vpll(ConstByteView(image.data(), image.size()), kKepler, card);
  CHECK(!vpll.set_vpll(4, 148500) && !vpll.set_vpll(0, 0) && card.writes.empty());
  const bool ok = vpll.set_vpll(1, 148500);
  CHECK(ok == vpll.last().ok() && vpll.last().family == NvPllFamily::Gf100);
  if (!ok) CHECK(vpll.last().writes == 0);
}

// NvidiaSecondaryGpu on the fake card (BAR0 and BAR1 as host memory, the Kepler VBIOS in the PROM window, no monitor answering).
static void test_gpu() {
  TestAllocator a;
  {
    FakeCard card(128u << 20);                                       // (BAR1 large enough for the linear part of 1 GiB of VRAM)
    card.bar0[0] = 0x0e4000a1;
    const Bytes image = kepler_image(false);
    memcpy(reinterpret_cast<uint8_t*>(card.bar0.data()) + 0x300000, image.data(), image.size());
    card.bar0[0x022438 / 4] = 1;                                     // one memory partition of 1 GiB
    card.bar0[0x11020c / 4] = 1024;
    card.bar0[0x022448 / 4] = 4;                                     // a display with four heads and four SORs
    card.bar0[0x612004 / 4] = 0x0f0f;
    GpuContext context = context_for(card, a);
    context.device.class_code = kPciClassDisplay;
    CHECK(NvidiaSecondaryGpu::supports(context.device));
    PciDeviceInfo other = context.device;
    other.vendor_id = 0x1002;
    CHECK(!NvidiaSecondaryGpu::supports(other));
    other = context.device;
    other.class_code = 0x04;                                         // the GPU's HD-audio function
    CHECK(!NvidiaSecondaryGpu::supports(other));
#ifdef TCOS_NVIDIA_NATIVE
    NvidiaSecondaryGpu gpu(context);
    CHECK(!gpu.initialize() && strstr(gpu.status(), "no monitor"));  // started, but nothing to show on (and no panel output: no standby)
    CHECK(!gpu.native() && !gpu.display() && gpu.mode_count() == 0 && gpu.connector_count() == 0 && gpu.width() == 0);
    IPanelOutput& panel = *gpu.panel_output();
    CHECK(!panel.has_panel_output() && !panel.can_light_panel() && !panel.panel_lit() && panel.light_panel() == PanelLightStatus::NoPanel);
    panel.release_panel();                                           // (nothing lit: nothing happens)
    gpu.shutdown();
    // An Ada card: its display is the GSP's.
    card.bar0[0] = 0x194000a1;
    card.bar0[0xa00 / 4] = 0x19400000;
    NvidiaSecondaryGpu ada(context);
    CHECK(!ada.initialize() && strstr(ada.status(), "GSP"));
    ada.shutdown();
    // A GA104 and a TU104: past the class and the VRAM size (GA10x: 0x1183a4) to the monitors, of which there are none.
    card.bar0[0x1183a4 / 4] = 1024;
    card.bar0[0x02243c / 4] = 1;                                     // (Turing: one FBPA of 1 GiB, gp100's sizing)
    card.bar0[0x90020c / 4] = 1024;
    for (const uint32_t boot0 : {0x174000a1u, 0x164000a1u}) {
      card.bar0[0] = boot0;
      card.bar0[0xa00 / 4] = boot0 & 0xfff00000u;
      NvidiaSecondaryGpu gpu2(context);
      const bool ok = gpu2.initialize();
      CHECK(!ok && !strstr(gpu2.status(), "GSP") && !strstr(gpu2.status(), "VRAM"));
      if (ok || strstr(gpu2.status(), "GSP") || strstr(gpu2.status(), "VRAM")) printf("  %08x: %s\n", boot0, gpu2.status());
      gpu2.shutdown();
    }
    card.bar0[0] = 0x0e4000a1;
    card.bar0[0xa00 / 4] = 0;
    // No register mapper.
    GpuContext none = context;
    none.registers = nullptr;
    NvidiaSecondaryGpu unmapped(none);
    CHECK(!unmapped.initialize() && strstr(unmapped.status(), "register mapper"));
#else
    // Built without the flag: refused before anything is mapped, read or written.
    const std::vector<uint32_t> before = card.bar0;
    NvidiaSecondaryGpu gpu(context);
    CHECK(!gpu.initialize() && strstr(gpu.status(), "TCOS_NVIDIA_NATIVE"));
    CHECK(card.maps == 0 && card.bar0 == before && !gpu.native() && gpu.mode_count() == 0 && gpu.connector_count() == 0);
    CHECK(!gpu.panel_output()->can_light_panel() && gpu.panel_output()->light_panel() == PanelLightStatus::NotSupported && card.bar0 == before);
    gpu.shutdown();
    // ... and so are the newly covered Turing and GA10x cards.
    for (const uint32_t boot0 : {0x164000a1u, 0x174000a1u}) {
      card.bar0[0] = boot0;
      card.bar0[0xa00 / 4] = boot0 & 0xfff00000u;
      const std::vector<uint32_t> was = card.bar0;
      const uint32_t maps = card.maps;
      NvidiaSecondaryGpu newer(context);
      CHECK(NvidiaSecondaryGpu::supports(context.device));
      CHECK(!newer.initialize() && strstr(newer.status(), "TCOS_NVIDIA_NATIVE") && card.maps == maps && card.bar0 == was);
      newer.shutdown();
    }
    card.bar0[0] = 0x0e4000a1;
    card.bar0[0xa00 / 4] = 0;
#endif
    PciAddress where;
    NvidiaSecondaryGpu located(context);
    CHECK(located.pci_location(&where) && strcmp(located.name(), "NVIDIA GPU") == 0);
  }
  CHECK(a.live == 0);
}

int main() {
  test_edid_modes();
#ifdef TCOS_NVIDIA_NATIVE
  test_start_hdmi();
  test_set_mode();
  test_start_dp();
  test_refusals();
  test_shutdown();
  test_panel_edp();
  test_panel_power_cases();
  test_panel_failures();
  test_panel_lvds();
  test_window_class_hdmi(kTuring, DisplayClass::Tu102);
  test_window_class_hdmi(kAmpere, DisplayClass::Ga102);
  test_window_class_dp(kTuring, DisplayClass::Tu102);
  test_window_class_dp(kAmpere, DisplayClass::Ga102);
  test_panel_edp_turing();
#endif
  test_vpll();
  test_gpu();
#ifdef TCOS_NVIDIA_NATIVE
  printf("NvidiaSecondaryGpuTest: %d checks, %d failed\n", g_checks, g_failed);
#else
  printf("NvidiaSecondaryGpuOffTest: %d checks, %d failed\n", g_checks, g_failed);
#endif
  return g_failed ? 1 : 0;
}
