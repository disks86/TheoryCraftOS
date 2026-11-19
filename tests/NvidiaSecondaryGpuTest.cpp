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
// a display class whose modes are not set here), and the shutdown. Then NvidiaSecondaryGpu itself on the fake card of NvTestCard.hpp.
//
// Built twice by tests/run_host_tests.sh: with -DTCOS_NVIDIA_NATIVE (everything above) and without it (NvidiaSecondaryGpuOffTest: the driver
// must refuse before touching the card, so the pool falls back to the probe).
#include <stdio.h>
#include <string.h>
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
    if (pramin(off)) {
      const uint64_t a = pramin_address(off);
      if (a + 4 <= mem.vram.size()) mem.wr(a, v);
      return;
    }
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
    Rig rig(DisplayClass::Gv100);                                      // Volta: its mode setting is not written
    rig.plug(1, monitor_edid(put_1080p, put_720p), EdidSource::Ddc);
    auto display = std::make_unique<NvSecondaryDisplay>(rig.inputs(identify_chip(0x140000a1, 0)));
    CHECK(display->start() == DisplayStatus::NotImplemented && strstr(display->status(), "GF119") && rig.card.writes.empty());
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
  CHECK(rig.allocator.live == 0 && (rig.card.last_write(0x001704) & 0x80000000u) == 0);
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
    CHECK(!gpu.initialize() && strstr(gpu.status(), "no monitor"));  // started, but nothing to show on
    CHECK(!gpu.native() && !gpu.display() && gpu.mode_count() == 0 && gpu.connector_count() == 0 && gpu.width() == 0);
    gpu.shutdown();
    // An Ampere card: its display is the GSP's.
    card.bar0[0] = 0x172000a1;
    NvidiaSecondaryGpu ampere(context);
    CHECK(!ampere.initialize() && strstr(ampere.status(), "GF119"));
    ampere.shutdown();
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
    gpu.shutdown();
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
