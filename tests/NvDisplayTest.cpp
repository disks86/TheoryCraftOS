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

// Host tests for the native NVIDIA display code (src/Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplay*.cpp) against a model of the display
// engine: registers (ownership handshake, channel control, the user areas' PUT/GET, the readback of assembly and armed state, the supervisor)
// and memory (the model fetches the push buffers from the fake video memory and decodes methods and data as EVO does, looks context DMA
// handles up through the RAMHT in the instance memory and writes notifiers where the context DMA points). The register and method numbers
// below are typed in here separately from NvDisplayHw.hpp; both come from the same (remembered) nouveau sources, so the model checks that the
// driver is consistent with itself and with nouveau's sequences, not that the numbers are right on hardware.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplay.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplayMode.hpp"
#include "NvDisplayModel.hpp"

using namespace hardware;
using namespace hardware::nv;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)


// ---------------------------------------------------------------------------------------------------------------------------------------
static void test_classes() {
  CHECK(display_class(chip_of(Family::Tesla, 0x50)) == DisplayClass::Nv50);
  CHECK(display_class(chip_of(Family::Fermi, 0xc0)) == DisplayClass::Nv50);
  CHECK(display_class(chip_of(Family::Fermi, 0xd9)) == DisplayClass::Gf119);
  CHECK(display_class(chip_of(Family::Kepler, 0xe4)) == DisplayClass::Gf119);
  CHECK(display_class(chip_of(Family::Pascal, 0x132)) == DisplayClass::Gf119);
  CHECK(display_class(chip_of(Family::Volta, 0x140)) == DisplayClass::Gv100);
  CHECK(display_class(chip_of(Family::Turing, 0x162)) == DisplayClass::Tu102);
  CHECK(display_class(chip_of(Family::Ampere, 0x172)) == DisplayClass::Gsp);
  CHECK(display_class(chip_of(Family::Nv40, 0x40)) == DisplayClass::None);

  // Ampere: refused before a register is touched
  FakeMemory mem;
  Model model(DisplayClass::Gf119, mem);
  Firmware fw;
  NvNativeDisplay ampere(model, mem, chip_of(Family::Ampere, 0x172), kQuick);
  ByteView view;
  CHECK(!ampere.engage_display(fw.fb(), &view));
  CHECK(strstr(ampere.display_status(), "GSP") != nullptr);
  CHECK(model.writes.empty());
  // a chip whose display the chip table says is not native
  NvNativeDisplay off(model, mem, chip_of(Family::Kepler, 0xe4, false), kQuick);
  CHECK(!off.engage_display(fw.fb(), &view) && model.writes.empty());
  NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe4, false), kQuick);
  CHECK(engine.probe() == DisplayStatus::NotNative);
}

static void test_gf119_takeover() {
  FakeMemory mem;
  Model model(DisplayClass::Gf119, mem);
  model.firmware_surface(1, 0, 1024, 768, 4096);
  model.core.arm[0x414 + 0x300] = model.core.assy[0x414 + 0x300] = 806u << 16 | 1344;   // a raster the takeover must not touch
  Firmware fw;
  NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
  ByteView view;
  CHECK(display.engage_display(fw.fb(), &view));
  CHECK(display.display_engaged() && display.engaged_heads() == 0x2);
  CHECK(view.size() == 4096u * 768);
  CHECK(view.equals(ConstByteView(fw.pixels.data(), fw.pixels.size())));   // the picture came along
  const uint64_t fb = display.framebuffer_offset();
  CHECK(fb >= kFirstAllocation && (fb & 0xffff) == 0);
  CHECK(model.errors == 0);
  CHECK(model.scanout[1] == fb);
  // ownership: the VBIOS bit written back, then the handshake bit cleared and waited for
  CHECK(model.wrote(0x6100ac, 0x100));
  CHECK(model.write_index(0x6100ac) < model.write_index(0x6194e8));
  CHECK((model.regs[0x6194e8] & 3) == 0);
  // capabilities copied before the handshake; instance memory; interrupts off
  CHECK(model.write_index(0x6101b4) >= 0 && model.write_index(0x6101b4) < model.write_index(0x6100ac));
  const uint32_t inst = model.regs[0x610010];
  CHECK((inst & 0xf) == 9 && ((uint64_t{inst & ~0xfu} << 8) & 0xffff) == 0);
  CHECK(model.wrote(0x6100b0, 0) && model.wrote(0x610090, 0));
  // the core channel: push buffer in VRAM, then PUT enabled, PUT 0, started
  CHECK((model.regs[0x610494] & 3) == 1);
  CHECK(model.wrote(0x610498, 0x00010000) && model.wrote(0x61049c, 1));
  CHECK(model.regs[0x610490] == 0x01000013);
  // the method stream: notifier armed, the surface of head 1, UPDATE, notifier off
  const auto& m = model.core.methods;
  CHECK(Model::has_method(model.core, 0x0088, 0xf0000000));
  CHECK(Model::has_method(model.core, 0x0084, 0x80000000));
  CHECK(Model::has_method(model.core, 0x0460 + 0x300, static_cast<uint32_t>(fb >> 8)));
  CHECK(Model::has_method(model.core, 0x0468 + 0x300, 768u << 16 | 1024));
  CHECK(Model::has_method(model.core, 0x046c + 0x300, 1u << 24 | 4096));
  CHECK(Model::has_method(model.core, 0x0470 + 0x300, 0xcfu << 8));
  CHECK(Model::has_method(model.core, 0x0474 + 0x300, 0xf0000001));
  CHECK(m.size() >= 2 && m[m.size() - 2].first == 0x0080 && m.back().first == 0x0084 && m.back().second == 0);
  for (auto& x : m) CHECK(x.first < 0x0400 + 0x300 || x.first >= 0x0460 + 0x300);   // nothing of the raster or OR sent
  CHECK(model.core.arm[0x414 + 0x300] == (806u << 16 | 1344));
  CHECK(model.core.updates == 1);
  // what is drawn goes to the screen; release copies it back and restores the firmware's surface
  view[0] = 0xAB;
  view[4096 * 767 + 5] = 0xCD;
  display.disengage_display();
  CHECK(!display.display_engaged());
  CHECK(fw.pixels[0] == 0xAB && fw.pixels[4096 * 767 + 5] == 0xCD);
  CHECK(model.core.arm[0x460 + 0x300] == 0 && model.scanout[1] == 0);
  CHECK(model.core.arm[0x474 + 0x300] == 0xf0000001);
  CHECK(mem.released.size() == 1 && mem.released[0] == fb);
  CHECK(model.errors == 0);
  // engaging again works on the claimed engine
  CHECK(display.engage_display(fw.fb(), &view) && model.scanout[1] != 0);
}

static void test_gf119_clone() {
  FakeMemory mem;
  Model model(DisplayClass::Gf119, mem);
  model.firmware_surface(0, 0x100000, 1024, 768, 4096);
  model.firmware_surface(2, 0x100000, 1024, 768, 4096);
  model.firmware_surface(3, 0x600000, 800, 600, 3328);   // another picture: left alone
  Firmware fw;
  NvNativeDisplay display(model, mem, chip_of(Family::Maxwell, 0x117), kQuick);
  ByteView view;
  CHECK(display.engage_display(fw.fb(), &view));
  CHECK(display.engaged_heads() == 0x5);
  CHECK(model.scanout[0] == display.framebuffer_offset() && model.scanout[2] == display.framebuffer_offset());
  CHECK(model.core.arm[0x460 + 3 * 0x300] == 0x6000);
  display.disengage_display();
  CHECK(model.scanout[0] == 0x100000 && model.scanout[2] == 0x100000);
  // a firmware mode no head shows
  NvFirmwareFramebuffer other = fw.fb();
  other.height = 600;
  CHECK(!display.engage_display(other, &view));
  CHECK(strstr(display.display_status(), "no head") != nullptr);
}

static void test_nv50_takeover() {
  FakeMemory mem;
  Model model(DisplayClass::Nv50, mem);
  model.firmware_surface(0, 0, 1024, 768, 4096);
  Firmware fw;
  NvNativeDisplay display(model, mem, chip_of(Family::Tesla, 0x84), kQuick);
  ByteView view;
  CHECK(display.engage_display(fw.fb(), &view));
  CHECK(model.errors == 0);
  CHECK(model.scanout[0] == display.framebuffer_offset());
  CHECK(model.wrote(0x610024, 0x100));
  CHECK(model.regs[0x610200] == 0x01000013);
  CHECK(model.wrote(0x610208, 0x00010000) && model.wrote(0x61020c, 0));
  CHECK(Model::has_method(model.core, 0x0860, static_cast<uint32_t>(display.framebuffer_offset() >> 8)));
  CHECK(Model::has_method(model.core, 0x086c, 1u << 20 | 4096));
  CHECK(Model::has_method(model.core, 0x0874, 0xf0000001));
  CHECK(model.regs[0x61002c] == 0 && model.regs[0x610028] == 0);
  display.disengage_display();
  CHECK(model.scanout[0] == 0);
}

static void test_volta_turing(DisplayClass cls, Family family, uint32_t chipset) {
  FakeMemory mem;
  Model model(cls, mem);
  model.unown_windows(2);
  model.firmware_surface(1, 0, 1024, 768, 4096, 2);
  Firmware fw;
  NvNativeDisplay display(model, mem, chip_of(family, chipset), kQuick);
  ByteView view;
  CHECK(display.engage_display(fw.fb(), &view));
  CHECK(model.errors == 0);
  const uint64_t fb = display.framebuffer_offset();
  CHECK(model.window_scanout[2] == fb);
  // ownership and instance memory
  CHECK((model.regs[0x6254e8] & 2) == 0);
  CHECK(model.regs[0x610010] == 0x9 && (uint64_t{model.regs[0x610014]} << 16) >= kFirstAllocation);
  CHECK(model.regs[0x640008] == (cls == DisplayClass::Tu102 ? 0x21u : 0x1234u));
  // the core channel and window 2's channel (control slot 3)
  CHECK(model.wrote(0x610b28, 1) && model.wrote(0x610b2c, 0x40));
  CHECK((model.regs[0x610b00] & 0x30000) == 0x10000);
  CHECK(model.regs[0x610b00 + 3 * 0x10] == 0x11);
  CHECK((model.regs[0x610b24 + 3 * 0x10] & 3) == 1);
  // the window's methods: notifier, surface, UPDATE
  const Model::Chan& w = model.win[2];
  CHECK(Model::has_method(w, 0x021c, 0xf0000000));
  CHECK(Model::has_method(w, 0x0260, static_cast<uint32_t>(fb >> 8)));
  CHECK(Model::has_method(w, 0x0240, 0xf0000001));
  CHECK(Model::has_method(w, 0x0230, 4096 / 64));
  CHECK(Model::has_method(w, 0x0224, 768u << 16 | 1024));
  CHECK(w.methods.back().first == 0x0200 && w.methods.back().second == 1);
  CHECK(model.core.methods.empty());     // the core channel had nothing to say
  display.disengage_display();
  CHECK(model.window_scanout[2] == 0);
  CHECK(mem.released.size() == 1);
}

static void test_timeouts() {
  Firmware fw;
  ByteView view;
  {
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    model.vbios_never_releases = true;
    NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(!display.engage_display(fw.fb(), &view));
    CHECK(strstr(display.display_status(), "VBIOS") != nullptr);
    CHECK(mem.allocations == mem.releases);      // the framebuffer went back
  }
  {
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    model.never_start = true;
    NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(engine.probe() == DisplayStatus::Ok);
    CHECK(engine.claim() == DisplayStatus::ChannelTimeout);
  }
  {
    FakeMemory mem;
    Model model(DisplayClass::Gv100, mem);
    model.unown_windows(0);
    model.firmware_surface(0, 0, 1024, 768, 4096, 0);
    model.never_start = true;
    NvDisplayEngine engine(model, mem, chip_of(Family::Volta, 0x140), kQuick);
    CHECK(engine.probe() == DisplayStatus::Ok);
    CHECK(engine.claim(0) == DisplayStatus::ChannelTimeout);
  }
  {   // the engine never fetches: PUT moves, GET does not
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    model.never_fetch = true;
    NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(!display.engage_display(fw.fb(), &view));
    CHECK(strstr(display.display_status(), "push buffer") != nullptr);
    CHECK(model.scanout[0] == ~0ull && model.core.arm[0x460] == 0);
  }
  {   // fetched and armed, but the notifier never comes: the old surface is sent back
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    model.never_notify = true;
    NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(!display.engage_display(fw.fb(), &view));
    CHECK(strstr(display.display_status(), "complete") != nullptr);
    CHECK(model.core.arm[0x460] == 0 && model.scanout[0] == 0);
    CHECK(mem.released.size() == 1);
  }
  {   // the engine takes the methods and notifies, but the armed state does not change
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    model.never_latch = true;
    NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(!display.engage_display(fw.fb(), &view));
    CHECK(strstr(display.display_status(), "take") != nullptr);
  }
}

static void test_refusals() {
  Firmware fw;
  ByteView view;
  {
    FakeMemory mem;
    mem.fail = true;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(!display.engage_display(fw.fb(), &view));
    CHECK(strstr(display.display_status(), "video memory") != nullptr);
    NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(engine.probe() == DisplayStatus::Ok && engine.claim() == DisplayStatus::NoMemory);
  }
  {   // a block-linear firmware surface is not taken over
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    model.core.arm[0x46c] = 4096;
    NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(!display.engage_display(fw.fb(), &view) && model.writes.empty());
  }
  {   // a GPU that reads all ones
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.regs[0x022448] = 0xffffffff;
    NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(engine.probe() == DisplayStatus::NoHeads);
  }
  {   // bad surfaces and operations before claim
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    ScanoutSurface s;
    CHECK(engine.probe() == DisplayStatus::Ok && engine.armed_surface(0, &s));
    CHECK(engine.set_surface(0, s) == DisplayStatus::NotClaimed);
    CHECK(engine.claim() == DisplayStatus::Ok);
    ScanoutSurface bad = s;
    bad.offset = 0x123;
    CHECK(engine.set_surface(0, bad) == DisplayStatus::UnsupportedSurface);
    bad = s;
    bad.pitch_bytes = 4000;
    CHECK(engine.set_surface(0, bad) == DisplayStatus::UnsupportedSurface);
    CHECK(engine.set_surface(7, s) == DisplayStatus::BadArgument);
    // the push buffer wraps around many times
    s.context_dma = NvDisplayEngine::vram_handle();
    for (uint32_t i = 0; i < 200; ++i) {
      s.offset = (i & 1) ? 0x400000 : 0x800000;
      CHECK(engine.set_surface(0, s) == DisplayStatus::Ok);
    }
    CHECK(model.core.jumps >= 2 && model.scanout[0] == 0x400000 && model.errors == 0);
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Milestone B
// ---------------------------------------------------------------------------------------------------------------------------------------
static DisplayTiming mode_1080p() {
  DisplayTiming t;
  t.clock_khz = 148500;
  t.hdisplay = 1920; t.hsync_start = 2008; t.hsync_end = 2052; t.htotal = 2200;
  t.vdisplay = 1080; t.vsync_start = 1084; t.vsync_end = 1089; t.vtotal = 1125;
  return t;
}

static void test_timings() {
  HeadRaster r;
  CHECK(compute_head_raster(mode_1080p(), &r));
  CHECK(r.h_total == 2200 && r.h_sync_end == 43 && r.h_blank_end == 191 && r.h_blank_start == 2111);
  CHECK(r.v_total == 1125 && r.v_sync_end == 4 && r.v_blank_end == 40 && r.v_blank_start == 1120);
  CHECK(r.v_blank2_start == 1 && r.v_blank2_end == 0 && r.blank_us == 637);
  CHECK(r.raster_size() == (1125u << 16 | 2200) && r.blank_start() == (1120u << 16 | 2111));

  DisplayTiming t;
  t.clock_khz = 74250;
  t.hdisplay = 1280; t.hsync_start = 1390; t.hsync_end = 1430; t.htotal = 1650;
  t.vdisplay = 720; t.vsync_start = 725; t.vsync_end = 730; t.vtotal = 750;
  CHECK(compute_head_raster(t, &r));
  CHECK(r.h_sync_end == 39 && r.h_blank_end == 259 && r.h_blank_start == 1539);
  CHECK(r.v_sync_end == 4 && r.v_blank_end == 24 && r.v_blank_start == 744 && r.blank_us == 622);

  DisplayTiming i = mode_1080p();      // 1080i
  i.clock_khz = 74250;
  i.vsync_end = 1094;
  i.interlaced = true;
  CHECK(compute_head_raster(i, &r));
  CHECK(r.v_sync_end == 4 && r.v_blank_end == 19 && r.v_blank_start == 559 && r.blank_us == 592);
  CHECK(r.v_blank2_end == 581 && r.v_blank2_start == 1121 && r.v_total == 1125 && r.interlaced);

  DisplayTiming bad = mode_1080p();
  bad.hsync_end = bad.hsync_start;
  CHECK(!compute_head_raster(bad, &r));
  bad = mode_1080p();
  bad.clock_khz = 0;
  CHECK(!compute_head_raster(bad, &r));
  bad = mode_1080p();
  bad.vtotal = 1000;
  CHECK(!compute_head_raster(bad, &r));
}

static void test_routes() {
  DcbOutput tmds;
  tmds.type = DcbOutputType::Tmds;
  tmds.heads = 0x3;
  tmds.or_mask = 0x4;
  tmds.sorconf_link = 3;
  OutputRoute r;
  CHECK(route_output(tmds, 0xf, 0xf, 1, 297000, &r) && r.sor == 2 && r.head == 1 && r.protocol == 0x5 && r.link == 3);
  CHECK(route_output(tmds, 0xf, 0xf, 3, 108000, &r) && r.head == 0 && r.protocol == 0x1 && r.link == 1);
  DcbOutput dp;
  dp.type = DcbOutputType::Dp;
  dp.heads = 0xf;
  dp.or_mask = 0x2;
  dp.sorconf_link = 2;
  CHECK(route_output(dp, 0xf, 0xf, 2, 148500, &r) && r.sor == 1 && r.head == 2 && r.protocol == 0x9 && r.link == 2 && r.dp);
  CHECK(!route_output(dp, 0x1, 0xf, 0, 148500, &r));     // its SOR is not there
  dp.heads = 0x10;
  CHECK(!route_output(dp, 0xf, 0xf, 0, 148500, &r));     // nor a head it may use
  DcbOutput analog;
  analog.type = DcbOutputType::Analog;
  analog.heads = 1;
  analog.or_mask = 1;
  CHECK(!route_output(analog, 0xf, 0xf, 0, 148500, &r));
}


static void test_modeset_tmds() {
  FakeMemory mem;
  Model model(DisplayClass::Gf119, mem);
  NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
  CHECK(engine.probe() == DisplayStatus::Ok);
  Clock clock;
  NvModeSetter setter(engine, &clock, nullptr, nullptr);
  NvModeSetter::Request req;
  req.output.type = DcbOutputType::Tmds;
  req.output.heads = 0x3;
  req.output.or_mask = 0x2;
  req.output.sorconf_link = 1;
  req.head = 1;
  req.timing = mode_1080p();
  req.surface.offset = 0x1000000;
  req.surface.width = 1920;
  req.surface.height = 1080;
  req.surface.pitch_bytes = 7680;
  req.surface.format = 0xcf;
  req.surface.pitch_linear = true;
  CHECK(setter.set_mode(req) == DisplayStatus::Ok);
  CHECK(model.errors == 0);
  CHECK(setter.supervisor_stages() == 7 && model.super_acks == 3);
  CHECK(clock.calls.size() == 1 && clock.calls[0].first == 1 && clock.calls[0].second == 148500);
  const uint32_t h = 0x300;
  CHECK(model.core.arm[0x0200 + 0x20] == (0x2u | 0x1u << 8));             // SOR 1 owned by head 1, single TMDS A
  CHECK(model.core.arm[0x0404 + h] == 5u << 6);
  CHECK(model.core.arm[0x0408 + h] == (0x31ec6000u | 1u << 25));
  CHECK(model.core.arm[0x0414 + h] == (1125u << 16 | 2200) && model.core.arm[0x0418 + h] == (4u << 16 | 43));
  CHECK(model.core.arm[0x041c + h] == (40u << 16 | 191) && model.core.arm[0x0420 + h] == (1120u << 16 | 2111) && model.core.arm[0x0424 + h] == 1);
  CHECK(model.core.arm[0x0450 + h] == 148500000u && model.core.arm[0x0454 + h] == 0x00200000 && model.core.arm[0x0458 + h] == 148500000u);
  CHECK(model.core.arm[0x04b8 + h] == (1080u << 16 | 1920) && model.core.arm[0x04c8 + h] == (1080u << 16 | 1920));
  CHECK(model.scanout[1] == 0x1000000);
  CHECK((model.regs[0x612300 + 0x800] & 0x007c0707) == 0x0au << 18);
  CHECK(model.regs[0x6101d4 + 0x800] == 0 && (model.regs[0x6100ac] & 7) == 0);
  // an unsupported depth, a surface of the wrong size
  req.depth_bpp = 16;
  CHECK(setter.set_mode(req) == DisplayStatus::BadArgument);
  req.depth_bpp = 24;
  req.surface.width = 1280;
  CHECK(setter.set_mode(req) == DisplayStatus::UnsupportedSurface);
  // Volta and NV50 mode setting is not done
  Model volta(DisplayClass::Gv100, mem);
  NvDisplayEngine ve(volta, mem, chip_of(Family::Volta, 0x140), kQuick);
  CHECK(ve.probe() == DisplayStatus::Ok);
  NvModeSetter vs(ve, &clock, nullptr, nullptr);
  CHECK(vs.set_mode(req) == DisplayStatus::NotImplemented);
}

static void test_dp_training_fallback() {
  FakeMemory mem;
  Model model(DisplayClass::Gf119, mem);
  NvDisplayEngine engine(model, mem, chip_of(Family::Pascal, 0x134), kQuick);
  CHECK(engine.probe() == DisplayStatus::Ok);
  Sink sink;
  sink.broken[0x14] = 0xf;      // HBR2 never trains
  sink.broken[0x0a] = 0xc;      // HBR: lanes 2 and 3 are bad
  const Dpcd::Capabilities caps = [&] {
    Dpcd::Capabilities c;
    CHECK(Dpcd::read_capabilities(sink, false, &c));
    return c;
  }();
  CHECK(caps.valid && caps.max_lanes == 4 && caps.max_link_rate_code == 0x14);
  DcbOutput dp;
  dp.type = DcbOutputType::Dp;
  NvSorDp sor(model, 0, 1, placeholder_dp_drive_table(), kQuick);
  // 1080p60 at 24 bpp: 445 500 kB/s
  const DpTrainResult r = train_dp_link(sink, sor, caps, dp, 148500ull * 3);
  CHECK(r.trained && r.bandwidth_code == 0x0a && r.lanes == 2);
  CHECK(r.attempts == 5);
  const uint8_t want_bw[] = {0x14, 0x14, 0x14, 0x0a, 0x0a}, want_lanes[] = {4, 2, 1, 4, 2};
  for (uint32_t i = 0; i < 5; ++i) CHECK(r.tried_bandwidth[i] == want_bw[i] && r.tried_lanes[i] == want_lanes[i]);
  CHECK((model.regs[0x61c10c] & 0x001f4000) == (0x3u << 16 | 0x4000));
  CHECK(((model.regs[0x612300] >> 18) & 0x1f) == 0x0a);
  CHECK((model.regs[0x61c130] & 0xf) == (1u << 2 | 1u << 1));     // lanes 0 and 1 are bytes 2 and 1
  CHECK(model.regs[0x61c110] == 0x10101010);                       // pattern off at the end
  CHECK(sink.dpcd[0x100] == 0x0a && (sink.dpcd[0x101] & 0x1f) == 2);
  // the DCB entry caps the rate: HBR at most, so HBR2 is never tried
  dp.dp_max_link_rate = 0x0a;
  const DpTrainResult capped = train_dp_link(sink, sor, caps, dp, 148500ull * 3);
  CHECK(capped.trained && capped.tried_bandwidth[0] == 0x0a && capped.attempts == 2);
  // nothing carries 4K60 at 24 bpp once HBR2 is broken
  dp.dp_max_link_rate = 0;
  const DpTrainResult big = train_dp_link(sink, sor, caps, dp, 594000ull * 3);
  CHECK(!big.trained && big.attempts == 1);
  sink.broken.clear();
  sink.broken[0x14] = 0xf;
  sink.broken[0x0a] = 0xf;
  sink.broken[0x06] = 0xf;
  const DpTrainResult none = train_dp_link(sink, sor, caps, dp, 148500ull * 3);
  CHECK(!none.trained && none.last == DpLinkTrainer::Result::ClockRecoveryTries);
}

static void test_modeset_edp() {
  FakeMemory mem;
  Model model(DisplayClass::Gf119, mem);
  NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe7), kQuick);
  CHECK(engine.probe() == DisplayStatus::Ok);
  Clock clock;
  Gpio gpio;
  Sink sink;
  sink.broken[0x14] = 0xc;      // HBR2 x4 fails, HBR2 x2 trains
  NvModeSetter setter(engine, &clock, &gpio, &sink);
  NvModeSetter::Request req;
  req.output.type = DcbOutputType::Dp;
  req.output.heads = 0x1;
  req.output.or_mask = 0x1;
  req.output.sorconf_link = 1;
  req.edp = true;
  req.timing = mode_1080p();
  req.surface.offset = 0x1000000;
  req.surface.width = 1920;
  req.surface.height = 1080;
  req.surface.pitch_bytes = 7680;
  req.surface.format = 0xe6;
  req.surface.pitch_linear = true;
  CHECK(setter.set_mode(req) == DisplayStatus::Ok);
  CHECK(gpio.calls.size() == 1 && gpio.calls[0].first == 0x01 && gpio.calls[0].second);
  CHECK(setter.dp_result().trained && setter.dp_result().bandwidth_code == 0x14 && setter.dp_result().lanes == 2);
  const DpTransferUnit& tu = setter.transfer_unit();
  CHECK(tu.valid && tu.tu >= 32 && tu.tu <= 64 && tu.watermark > 6 && tu.hblank_symbols > 0);
  CHECK((model.regs[0x616610] & 0x0800003f) == (0x08000000 | (tu.watermark & 0x3f)));
  CHECK(((model.regs[0x61c10c] & 0x1fc) >> 2) == tu.tu);
  CHECK(model.core.arm[0x0200] == (0x1u | 0x8u << 8));             // SOR 0, head 0, DP link A
  CHECK(model.errors == 0);
  // without the GPIO the panel cannot be powered
  NvModeSetter no_gpio(engine, &clock, nullptr, &sink);
  CHECK(no_gpio.set_mode(req) == DisplayStatus::NoOutput);
  // a sink that trains nowhere
  sink.broken[0x14] = sink.broken[0x0a] = sink.broken[0x06] = 0xf;
  CHECK(setter.set_mode(req) == DisplayStatus::LinkTraining);
  // the transfer unit of a mode the link cannot carry
  HeadRaster r;
  CHECK(compute_head_raster(mode_1080p(), &r));
  CHECK(!compute_dp_transfer_unit(r, 0x06, 1, true, 24).valid);
  CHECK(compute_dp_transfer_unit(r, 0x14, 4, true, 24).valid);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_classes();
  test_gf119_takeover();
  test_gf119_clone();
  test_nv50_takeover();
  test_volta_turing(DisplayClass::Gv100, Family::Volta, 0x140);
  test_volta_turing(DisplayClass::Tu102, Family::Turing, 0x164);
  test_timeouts();
  test_refusals();
  test_timings();
  test_routes();
  test_modeset_tmds();
  test_dp_training_fallback();
  test_modeset_edp();
  if (g_failed) {
    printf("NvDisplayTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("NvDisplayTest: all passed\n");
  return 0;
}
