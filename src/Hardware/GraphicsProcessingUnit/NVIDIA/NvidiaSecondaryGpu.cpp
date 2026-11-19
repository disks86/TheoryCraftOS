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

// Follows nouveau (MIT license) where it has a counterpart: drm_edid.c (drm_mode_detailed) for the timings, nouveau_connector.c
// (nouveau_conn_mode_valid: the TMDS clock limits), nvkm/subdev/bar/gf100.c (the BARs, through Nvkm/NvBar), dispnv50/disp.c (the atomic
// enable and disable of a head, through Nvkm/NvDisplayMode).
#include "Hardware/GraphicsProcessingUnit/NVIDIA/NvidiaSecondaryGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplayHw.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvI2cEdid.hpp"
#include "Hardware/Monitor/Edid.hpp"

namespace hardware {
namespace nv {

namespace {
constexpr uint64_t kSurfaceAlign = 64 * 1024;
constexpr uint32_t kMaxPitch = 0x3ff00;          // what the head's storage method holds
constexpr uint32_t kTmdsSingleLinkKhz = 165000, kTmdsDualLinkKhz = 330000;
constexpr uint8_t kDefaultDpRate = 0x14, kDefaultDpLanes = 4;

uint64_t align_up(uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

bool is_panel_connector(uint8_t dcb_type) { return dcb_type == 0x40 || dcb_type == 0x41 || dcb_type == 0x47; }   // LVDS, LVDS SPWG, eDP

uint32_t refresh_of(const DisplayTiming& t) {
  const uint64_t pixels = uint64_t{t.htotal} * t.vtotal;
  if (!pixels) return 0;
  uint64_t mhz = uint64_t{t.clock_khz} * 1000000 / pixels;
  if (t.interlaced) mhz *= 2;                    // fields
  return static_cast<uint32_t>(mhz);
}

// Adds `t` unless a mode of that size is there already.
void add_mode(const DisplayTiming& t, ArrayView<EdidMode> out, uint32_t* count) {
  for (uint32_t i = 0; i < *count; ++i)
    if (out[i].timing.hdisplay == t.hdisplay && out[i].timing.vdisplay == t.vdisplay) return;
  if (*count >= out.size()) return;
  EdidMode& m = out[(*count)++];
  m.timing = t;
  m.refresh_millihz = refresh_of(t);
  m.preferred = false;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------------------------------------------------------------------
bool decode_detailed_timing(ConstByteView d, DisplayTiming* out) {
  if (!out || d.size() < 18) return false;
  const uint32_t clock_10khz = d[0] | uint32_t{d[1]} << 8;
  if (clock_10khz == 0) return false;            // a monitor descriptor
  const uint32_t ha = d[2] | uint32_t(d[4] >> 4) << 8, hb = d[3] | uint32_t(d[4] & 0xf) << 8;
  const uint32_t va = d[5] | uint32_t(d[7] >> 4) << 8, vb = d[6] | uint32_t(d[7] & 0xf) << 8;
  const uint32_t hso = d[8] | uint32_t((d[11] >> 6) & 3) << 8, hsw = d[9] | uint32_t((d[11] >> 4) & 3) << 8;
  const uint32_t vso = uint32_t(d[10] >> 4) | uint32_t((d[11] >> 2) & 3) << 4, vsw = uint32_t(d[10] & 0xf) | uint32_t(d[11] & 3) << 4;
  const uint8_t flags = d[17];
  if (flags & 0x60) return false;                // stereo
  if (!ha || !va || !hsw || !vsw) return false;
  DisplayTiming t;
  t.clock_khz = clock_10khz * 10;
  t.hdisplay = ha;
  t.hsync_start = ha + hso;
  t.hsync_end = t.hsync_start + hsw;
  t.htotal = ha + hb;
  t.vdisplay = va;
  t.vsync_start = va + vso;
  t.vsync_end = t.vsync_start + vsw;
  t.vtotal = va + vb;
  // drm_mode_detailed: a sync end past the total moves the total
  if (t.hsync_end > t.htotal) t.htotal = t.hsync_end + 1;
  if (t.vsync_end > t.vtotal) t.vtotal = t.vsync_end + 1;
  t.interlaced = (flags & 0x80) != 0;
  if (t.interlaced) {
    t.vdisplay *= 2;
    t.vsync_start *= 2;
    t.vsync_end *= 2;
    t.vtotal = t.vtotal * 2 | 1;
  }
  t.hsync_negative = !(flags & 0x02);
  t.vsync_negative = !(flags & 0x04);
  *out = t;
  return true;
}

uint32_t edid_modes(ConstByteView edid, ArrayView<EdidMode> out) {
  uint32_t count = 0;
  if (edid.size() < 128 || out.empty()) return 0;
  DisplayTiming t;
  for (uint32_t i = 0; i < 4; ++i)
    if (decode_detailed_timing(edid.subview(54 + i * 18, 18), &t)) add_mode(t, out, &count);
  const uint32_t first_base = count;
  for (uint64_t block = 128; block + 128 <= edid.size(); block += 128) {
    const ConstByteView b = edid.subview(block, 128);
    if (b[0] != 0x02) continue;                  // a CTA extension
    const uint32_t d = b[2];
    if (d < 4 || d > 127) continue;
    for (uint32_t at = d; at + 18 <= 127; at += 18) {
      if (b[at] == 0 && b[at + 1] == 0) break;   // padding
      if (decode_detailed_timing(b.subview(at, 18), &t)) add_mode(t, out, &count);
    }
  }
  if (count == 0) return 0;
  // The preferred mode: the one parse_edid says (by size and refresh), else the first detailed timing of the base block.
  int32_t preferred = first_base ? 0 : -1;
  EdidInfo info;
  if (parse_edid(edid, &info) && info.preferred >= 0) {
    const EdidTiming& p = info.timings[info.preferred];
    for (uint32_t i = 0; i < count; ++i)
      if (out[i].timing.hdisplay == p.width && out[i].timing.vdisplay == p.height) {
        preferred = static_cast<int32_t>(i);
        break;
      }
  }
  if (preferred >= 0) out[static_cast<uint32_t>(preferred)].preferred = true;
  return count;
}

uint32_t max_output_clock_khz(const DcbOutput& output) {
  switch (output.type) {
    case DcbOutputType::Tmds: return output.sorconf_link == 3 ? kTmdsDualLinkKhz : kTmdsSingleLinkKhz;
    case DcbOutputType::Dp: {
      const uint32_t rate = output.dp_max_link_rate ? output.dp_max_link_rate : kDefaultDpRate;
      const uint32_t lanes = output.dp_max_lanes ? output.dp_max_lanes : kDefaultDpLanes;
      return static_cast<uint32_t>(uint64_t{lanes} * rate * 27000 * 8 / NvSecondaryDisplay::kDepthBpp);
    }
    default: return 0;
  }
}

Optional<uint32_t> choose_mode(ArrayView<const EdidMode> modes, uint32_t max_clock_khz) {
  Optional<uint32_t> best;
  for (uint32_t i = 0; i < modes.size(); ++i) {
    const EdidMode& m = modes[i];
    if (m.timing.clock_khz == 0 || m.timing.clock_khz > max_clock_khz) continue;
    if (m.preferred) return i;
    if (!best) {
      best = i;
      continue;
    }
    const EdidMode& b = modes[*best];
    const uint64_t area = uint64_t{m.timing.hdisplay} * m.timing.vdisplay, best_area = uint64_t{b.timing.hdisplay} * b.timing.vdisplay;
    if (area > best_area || (area == best_area && m.refresh_millihz > b.refresh_millihz)) best = i;
  }
  return best;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Bar1LinearVram
// ---------------------------------------------------------------------------------------------------------------------------------------
Bar1LinearVram::Bar1LinearVram(INvRegisters& registers, const Chip& chip, const VramInfo& vram, ByteView window, const Bar1Locks& locks,
                               NvTimeouts timeouts)
    : r_(registers),
      chip_(chip),
      vram_(vram),
      window_(window),
      fb_(*locks.heap),
      pramin_(registers, vram.bytes, *locks.pramin),
      tables_(fb_, pramin_),
      tlb_(registers, chip, *locks.tlb, timeouts),
      bar_(registers, chip, tables_, tlb_, *locks.bar1, *locks.bar2, timeouts) {}

Bar1LinearVram::~Bar1LinearVram() { fini(); }

uint64_t Bar1LinearVram::linear_bytes_for(const VramInfo& vram) {
  const uint64_t top = vram.bytes > vram.reserved_tail ? vram.bytes - vram.reserved_tail : 0;
  uint64_t linear = top < kMaxLinear ? top : kMaxLinear;
  linear &= ~uint64_t{0xffff};
  if (linear < vram.reserved_head || linear - vram.reserved_head < (8u << 20)) return 0;   // (not even one small surface)
  return linear;
}

VramInfo Bar1LinearVram::heap_info(const VramInfo& vram, uint64_t linear) {
  VramInfo info = vram;
  info.bytes = linear;                           // (everything this driver allocates lies in the linear part of BAR1)
  info.reserved_tail = 0;
  return info;
}

BarStatus Bar1LinearVram::init() {
  if (bar_.ready()) return BarStatus::Ok;
  const uint64_t linear = linear_bytes_for(vram_);
  if (!linear || window_.size() < linear) return BarStatus::BadSize;
  window_ = window_.first(linear);
  if (!fb_.init(heap_info(vram_, linear))) return BarStatus::NoMemory;
  const BarStatus s = bar_.init(kBar1VirtualBytes, kBar2VirtualBytes, linear);
  if (s != BarStatus::Ok) return s;
  if (!bar_.bar2().flush() || !bar_.bar1().flush()) {
    bar_.fini();
    return BarStatus::Timeout;
  }
  table_allocations_ = fb_.allocation_count();
  return BarStatus::Ok;
}

void Bar1LinearVram::fini() { bar_.fini(); }

GpuMemory Bar1LinearVram::allocate(MemoryTarget target, uint64_t bytes, uint64_t alignment) {
  GpuMemory m;
  if (target != MemoryTarget::Vram || bytes == 0 || bytes > ~uint64_t{0} - 4095 || !bar_.ready()) return m;   // (no system memory here)
  const uint64_t granule = alignment >= 4096 ? 4096 : 256;
  const uint64_t rounded = align_up(bytes, granule);
  const Optional<uint64_t> at = fb_.allocate(rounded, alignment);
  if (!at) return m;
  ByteView view;
  if (!window_.try_subview(*at, rounded, &view)) {
    (void)fb_.release(*at);
    return m;
  }
  view.fill(0);
  m.gpu_address = *at;
  m.bytes = rounded;
  m.target = MemoryTarget::Vram;
  m.cpu = view;
  return m;
}

void Bar1LinearVram::release(GpuMemory& memory) {
  if (memory.valid() && memory.target == MemoryTarget::Vram) (void)fb_.release(memory.gpu_address);
  memory = GpuMemory();
}

void Bar1LinearVram::flush(const GpuMemory& memory) {
  (void)memory;
  (void)flush_writes();
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// NvVpllClock
// ---------------------------------------------------------------------------------------------------------------------------------------
bool NvVpllClock::set_vpll(uint32_t head, uint32_t khz) {
  if (head > kPllVpll3 - kPllVpll0 || khz == 0) return false;
  NvPllSetInputs in;
  in.bios = &bios_;
  in.chip = chip_;
  in.registers = &r_;
  last_ = nv_pll_set(in, kPllVpll0 + head, khz);
  return last_.ok();
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// NvSecondaryDisplay
// ---------------------------------------------------------------------------------------------------------------------------------------
DisplayStatus NvSecondaryDisplay::fail(DisplayStatus status, const char* why) {
  last_ = status;
  status_ = why ? why : display_status_text(status);
  return status;
}

bool NvSecondaryDisplay::pick_output() {
  const BoardInfo& board = *in_.board;
  const uint32_t outputs = board.output_count < BoardInfo::kMaxOutputs ? board.output_count : BoardInfo::kMaxOutputs;
  for (uint32_t i = 0; i < in_.connectors.size(); ++i) {
    const NvidiaNative::Connector& c = in_.connectors[i];
    if (!c.connected || c.edid_bytes < 128 || is_panel_connector(c.dcb_type)) continue;   // (panels belong to the integrated GPU)
    // The output the monitor is on: DisplayPort when its EDID came over AUX, TMDS when over DDC (a DP++ port has both in the DCB).
    const DcbOutputType want = c.edid_source == EdidSource::Aux ? DcbOutputType::Dp : DcbOutputType::Tmds;
    int32_t found = -1;
    for (uint32_t o = 0; o < outputs; ++o) {
      const DcbOutput& out = board.outputs[o];
      if (out.connector != i || out.location != 0 || (out.type != DcbOutputType::Tmds && out.type != DcbOutputType::Dp)) continue;
      if (found < 0 || out.type == want) found = static_cast<int32_t>(o);
      if (out.type == want) break;
    }
    if (found < 0) continue;
    const DcbOutput& output = board.outputs[static_cast<uint32_t>(found)];
    EdidMode all[kMaxModes];
    const uint32_t n = edid_modes(ConstByteView(c.edid).first(c.edid_bytes), ArrayView<EdidMode>(all));
    const uint32_t limit = max_output_clock_khz(output);
    mode_count_ = 0;
    for (uint32_t m = 0; m < n; ++m) {
      HeadRaster raster;
      const DisplayTiming& t = all[m].timing;
      if (t.clock_khz > limit || !compute_head_raster(t, &raster) || uint64_t{t.hdisplay} * 4 > kMaxPitch) continue;
      modes_[mode_count_++] = all[m];
    }
    if (mode_count_ == 0) continue;
    const Optional<uint32_t> start = choose_mode(ArrayView<const EdidMode>(modes_).first(mode_count_), limit);
    OutputRoute route;
    if (!start || !route_output(output, engine_->sor_mask(), engine_->head_mask(), 0, modes_[*start].timing.clock_khz, &route)) continue;
    connector_ = static_cast<int32_t>(i);
    output_ = output;
    route_ = route;
    current_ = *start;
    return true;
  }
  mode_count_ = 0;
  connector_ = -1;
  return false;
}

bool NvSecondaryDisplay::allocate_surface(const EdidMode& mode, Surface* out) {
  const uint32_t pitch = static_cast<uint32_t>(align_up(uint64_t{mode.timing.hdisplay} * 4, 256));
  if (pitch > kMaxPitch) return false;
  const uint64_t bytes = align_up(uint64_t{pitch} * mode.timing.vdisplay, kSurfaceAlign);
  Surface s;
  s.memory = memory_->allocate(MemoryTarget::Vram, bytes, kSurfaceAlign);
  if (!s.memory.valid() || s.memory.cpu.size() < bytes) {
    memory_->release(s.memory);
    return false;
  }
  s.scanout.offset = s.memory.gpu_address;
  s.scanout.width = mode.timing.hdisplay;
  s.scanout.height = mode.timing.vdisplay;
  s.scanout.pitch_bytes = pitch;
  s.scanout.format = disp_hw::kFormatX8R8G8B8;
  s.scanout.pitch_linear = true;
  *out = s;
  return true;
}

void NvSecondaryDisplay::release_surface(Surface& surface) {
  if (memory_ && surface.memory.valid()) memory_->release(surface.memory);
  surface = Surface();
}

DisplayStatus NvSecondaryDisplay::program(const EdidMode& mode, const Surface& surface) {
  OutputRoute route;
  if (!route_output(output_, engine_->sor_mask(), engine_->head_mask(), route_.head, mode.timing.clock_khz, &route)) return DisplayStatus::NoOutput;
  NvModeSetter setter(*engine_, in_.clock, nullptr, aux_);
  NvModeSetter::Request request;
  request.output = output_;
  request.head = route.head;
  request.timing = mode.timing;
  request.surface = surface.scanout;
  request.depth_bpp = kDepthBpp;
  const DisplayStatus s = setter.set_mode(request);
  if (engine_->claimed()) mode_set_ = true;           // (from the first UPDATE on, the head may show something: shutdown blanks it)
  if (s == DisplayStatus::Ok) route_ = route;
  return s;
}

DisplayStatus NvSecondaryDisplay::start() {
  shutdown();
  if (!in_.registers || !in_.board || !in_.allocator) return fail(DisplayStatus::BadArgument, "no registers, board tables or allocator");
  Bar1Locks locks = in_.locks;
  if (!locks.complete()) locks = Bar1Locks{&kernel_locks_[0], &kernel_locks_[1], &kernel_locks_[2], &kernel_locks_[3], &kernel_locks_[4]};
  memory_ = UniquePointer<Bar1LinearVram>::make(*in_.allocator, *in_.registers, in_.chip, in_.vram, in_.bar1, locks, in_.memory_timeouts);
  if (!memory_) return fail(DisplayStatus::NoMemory, "out of memory");
  engine_ = UniquePointer<NvDisplayEngine>::make(*in_.allocator, *in_.registers, *memory_, in_.chip, in_.timeouts);
  if (!engine_) return fail(DisplayStatus::NoMemory, "out of memory");
  // Reads only up to the BAR set-up.
  const DisplayStatus probed = engine_->probe();
  if (probed != DisplayStatus::Ok) {
    shutdown();
    return fail(probed, nullptr);
  }
  if (engine_->display_class() != DisplayClass::Gf119) {
    shutdown();
    return fail(DisplayStatus::NotImplemented, "mode setting from scratch is done for the GF119..Pascal display only");
  }
  if (!pick_output()) {
    shutdown();
    return fail(DisplayStatus::NoOutput, "no monitor on an output this driver drives (HDMI, DVI or DisplayPort with an EDID)");
  }
  const uint64_t linear = Bar1LinearVram::linear_bytes_for(in_.vram);
  if (!linear || in_.bar1.size() < linear) {
    shutdown();
    return fail(DisplayStatus::NoMemory, "too little VRAM or BAR1 for a scanout surface");
  }
  if (output_.type == DcbOutputType::Dp) {
    aux_ = in_.aux;
    if (!aux_) {
      const I2cRoute r = i2c_route(*in_.board, output_.i2c_index);
      if (r.has_aux()) own_aux_ = UniquePointer<NvAuxChannel>::make(*in_.allocator, *in_.registers, i2c_variant_for(in_.chip), r.aux);
      if (own_aux_ && own_aux_->valid()) aux_ = own_aux_.get();
    }
    if (!aux_) {
      shutdown();
      return fail(DisplayStatus::NoOutput, "the DisplayPort output has no AUX channel");
    }
  }
  // Writes from here on.
  if (memory_->init() != BarStatus::Ok) {
    shutdown();
    return fail(DisplayStatus::NoMemory, "BAR1 could not be set up");
  }
  running_ = true;
  // The chosen mode, and when the link cannot carry it, the others from the fastest down.
  uint32_t tried = 0, next = current_;
  DisplayStatus s = DisplayStatus::NoOutput;
  while (tried < mode_count_) {
    ++tried;
    Surface surface;
    if (!allocate_surface(modes_[next], &surface)) {
      s = DisplayStatus::NoMemory;
    } else {
      s = program(modes_[next], surface);
      if (s == DisplayStatus::Ok) {
        surface_ = surface;
        current_ = next;
        last_ = s;
        status_ = "running";
        return s;
      }
      release_surface(surface);
    }
    if (s != DisplayStatus::LinkTraining && s != DisplayStatus::NoMemory) break;
    // the fastest mode left that is slower than the one that failed
    const uint32_t failed_clock = modes_[next].timing.clock_khz;
    int32_t pick = -1;
    for (uint32_t i = 0; i < mode_count_; ++i) {
      const uint32_t clock = modes_[i].timing.clock_khz;
      if (clock < failed_clock && (pick < 0 || clock > modes_[static_cast<uint32_t>(pick)].timing.clock_khz)) pick = static_cast<int32_t>(i);
    }
    if (pick < 0) break;
    next = static_cast<uint32_t>(pick);
  }
  shutdown();
  return fail(s, nullptr);
}

DisplayStatus NvSecondaryDisplay::set_mode(uint32_t index) {
  if (!running_) return fail(DisplayStatus::NotClaimed, "not running");
  if (index >= mode_count_) return DisplayStatus::BadArgument;
  if (index == current_) return DisplayStatus::Ok;
  Surface next;
  if (!allocate_surface(modes_[index], &next)) return fail(DisplayStatus::NoMemory, "no VRAM for the new mode's surface");
  const DisplayStatus s = program(modes_[index], next);
  if (s == DisplayStatus::Ok) {
    release_surface(surface_);
    surface_ = next;
    current_ = index;
    last_ = s;
    status_ = "running";
    return s;
  }
  (void)program(modes_[current_], surface_);       // back to the mode that worked (before its surface's replacement goes)
  release_surface(next);
  return fail(s, nullptr);
}

void NvSecondaryDisplay::shutdown() {
  if (engine_ && mode_set_ && engine_->claimed()) {
    NvModeSetter setter(*engine_, nullptr, nullptr, nullptr);
    (void)setter.disable(route_.head, route_.sor);
  }
  mode_set_ = false;
  engine_ = UniquePointer<NvDisplayEngine>();         // stops the channels; its push buffer, instance memory and notifier go back to the heap
  release_surface(surface_);
  aux_ = nullptr;
  own_aux_ = UniquePointer<NvAuxChannel>();
  if (memory_) memory_->fini();                       // (the tables stay allocated until the next start or the destructor)
  running_ = false;
}

void NvSecondaryDisplay::flush() {
  if (memory_) (void)memory_->flush_writes();
}

}  // namespace nv

// ---------------------------------------------------------------------------------------------------------------------------------------
// NvidiaSecondaryGpu
// ---------------------------------------------------------------------------------------------------------------------------------------
NvidiaSecondaryGpu::NvidiaSecondaryGpu(const GpuContext& context) : context_(context) {}

NvidiaSecondaryGpu::~NvidiaSecondaryGpu() { shutdown(); }

bool NvidiaSecondaryGpu::fail(const char* why) {
  status_ = why;
  display_ = UniquePointer<nv::NvSecondaryDisplay>();
  clock_ = UniquePointer<nv::NvVpllClock>();
  native_ = UniquePointer<nv::NvidiaNative>();
  ordered_ = 0;
  set_framebuffer(ByteView(), 0, 0, 0, true);
  return false;
}

bool NvidiaSecondaryGpu::initialize() {
#ifndef TCOS_NVIDIA_NATIVE
  // (Nothing is read or written: the pool's probe does the read-only part.)
  return fail("built without TCOS_NVIDIA_NATIVE: the display of a secondary NVIDIA GPU is not driven");
#else
  if (!context_.allocator || !context_.registers || !context_.pci) return fail("no allocator or register mapper");
  allocators::IAllocator& allocator = *context_.allocator;
  native_ = UniquePointer<nv::NvidiaNative>::make(allocator, context_, allocator);
  if (!native_) return fail("out of memory");
  native_->configure_secondary(nullptr);           // (the ACPI _ROM method is not reachable when the pool starts: the VBIOS comes from the card)
  if (!native_->start()) return fail(native_->status());
  const nv::Chip& chip = native_->chip();
  const char* const parts[] = {"NVIDIA ", chip.name, " (", nv::family_name(chip.family), ")"};
  const ArrayView<char> out(name_);
  uint32_t n = 0;
  for (const char* part : parts)
    for (const char* p = part; p && *p && n + 1 < out.size(); ++p) out[n++] = *p;
  out[n] = 0;
  if (nv::display_class(chip) != nv::DisplayClass::Gf119) return fail("mode setting from scratch is done for GF119..Pascal displays only");
  if (!native_->vram().ok() || !native_->registers()) return fail("the VRAM size could not be read");
  const nv::VramInfo& vram = native_->vram().info;
  const uint64_t linear = nv::Bar1LinearVram::linear_bytes_for(vram);
  const uint64_t bar1 = context_.pci->memory_bar(context_.device.address, 1);
  if (!linear || !bar1) return fail("no BAR1 or too little VRAM");
  MmioRegion bar1_region;
  if (!context_.registers->map_registers(bar1, linear, &bar1_region)) return fail("BAR1 not mapped");
  nv::NvSecondaryDisplay::Inputs in;
  in.registers = native_->registers();
  in.chip = chip;
  in.board = &native_->board();
  in.connectors = native_->connectors();
  in.vram = vram;
  in.bar1 = nv::mmio_bytes(bar1_region);
  in.allocator = &allocator;
  if (!native_->bios_image().empty()) clock_ = UniquePointer<nv::NvVpllClock>::make(allocator, native_->bios_image(), chip, *native_->registers());
  if (!clock_) return fail("no video BIOS: the pixel clock's PLL limits are not known");
  in.clock = clock_.get();
  display_ = UniquePointer<nv::NvSecondaryDisplay>::make(allocator, in);
  if (!display_) return fail("out of memory");
  if (display_->start() != nv::DisplayStatus::Ok) return fail(display_->status());
  set_video_memory_total(vram.bytes);
  order();
  show();
  status_ = "running";
  return true;
#endif
}

void NvidiaSecondaryGpu::shutdown() {
  if (display_) display_->shutdown();
  display_ = UniquePointer<nv::NvSecondaryDisplay>();
  clock_ = UniquePointer<nv::NvVpllClock>();
  native_ = UniquePointer<nv::NvidiaNative>();
  ordered_ = 0;
  set_framebuffer(ByteView(), 0, 0, 0, true);
}

void NvidiaSecondaryGpu::show() {
  if (display_ && display_->running()) set_framebuffer(display_->pixels(), display_->width(), display_->height(), display_->pitch_bytes(), true);
  else set_framebuffer(ByteView(), 0, 0, 0, true);
}

uint32_t NvidiaSecondaryGpu::mode_count() const { return display_ && display_->running() ? display_->mode_count() : 0; }

bool NvidiaSecondaryGpu::mode_info(uint32_t index, DisplayMode* out) const {
  if (!out || index >= mode_count()) return false;
  const nv::EdidMode* m = display_->mode(index);
  out->width = m->timing.hdisplay;
  out->height = m->timing.vdisplay;
  return true;
}

uint32_t NvidiaSecondaryGpu::current_mode() const { return display_ ? display_->current_mode() : 0; }

bool NvidiaSecondaryGpu::set_mode(uint32_t index) {
  if (!display_ || !display_->running()) return false;
  const bool ok = display_->set_mode(index) == nv::DisplayStatus::Ok;
  show();
  return ok;
}

void NvidiaSecondaryGpu::present() {
  if (display_) display_->flush();
}

bool NvidiaSecondaryGpu::pci_location(PciAddress* out) const {
  if (!out) return false;
  *out = context_.device.address;
  return true;
}

// ------------------------------------------------------------------ the monitors

void NvidiaSecondaryGpu::order() {
  ordered_ = 0;
  if (!native_) return;
  ConstByteView shown;                             // the driven connector's EDID: order_connectors puts it first
  if (display_ && display_->connector() >= 0) {
    const nv::NvidiaNative::Connector* c = native_->connector(static_cast<uint32_t>(display_->connector()));
    if (c) shown = ConstByteView(c->edid).first(c->edid_bytes);
  }
  ordered_ = nv::order_connectors(native_->connectors(), shown, ArrayView<uint8_t>(order_));
}

bool NvidiaSecondaryGpu::driven(uint32_t connector) const {
  return display_ && display_->running() && connector < ordered_ && static_cast<int32_t>(order_[connector]) == display_->connector();
}

bool NvidiaSecondaryGpu::connector_info(uint32_t connector, ConnectorInfo* out) const {
  if (!native_ || !nv::describe_connector(native_->connectors(), ArrayView<const uint8_t>(order_).first(ordered_), connector, out)) return false;
  if (driven(connector)) {
    for (uint32_t i = 0; i < display_->mode_count(); ++i)
      if (display_->mode(i)->preferred) {
        out->preferred_width = display_->mode(i)->timing.hdisplay;
        out->preferred_height = display_->mode(i)->timing.vdisplay;
      }
  }
  return true;
}

uint32_t NvidiaSecondaryGpu::read_edid(uint32_t connector, ByteView out) {
  if (!native_ || connector >= ordered_) return 0;
  const nv::NvidiaNative::Connector* c = native_->connector(order_[connector]);
  if (!c) return 0;
  const ConstByteView edid = ConstByteView(c->edid).first(c->edid_bytes).at_most(out.size());
  out.first(edid.size()).copy_from(edid);
  return static_cast<uint32_t>(edid.size());
}

bool NvidiaSecondaryGpu::refresh_connectors() {
  if (!native_ || !native_->refresh_connectors()) return false;
  order();
  return true;
}

bool NvidiaSecondaryGpu::mode_available(uint32_t connector, uint32_t width, uint32_t height) const {
  if (!driven(connector)) return false;
  for (uint32_t i = 0; i < display_->mode_count(); ++i)
    if (display_->mode(i)->timing.hdisplay == width && display_->mode(i)->timing.vdisplay == height) return true;
  return false;
}

uint32_t NvidiaSecondaryGpu::device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const {
  if (!driven(connector) || !out) return 0;
  const ArrayView<MonitorMode> list(out, capacity);
  uint32_t n = 0;
  for (uint32_t i = 0; i < display_->mode_count() && n < list.size(); ++i) {
    const nv::EdidMode& m = *display_->mode(i);
    MonitorMode& o = list[n++];
    o = MonitorMode();
    o.width = m.timing.hdisplay;
    o.height = m.timing.vdisplay;
    o.refresh_millihz = m.refresh_millihz;
    o.interlaced = m.timing.interlaced;
    o.preferred = m.preferred;
  }
  return n;
}

bool NvidiaSecondaryGpu::set_display_mode(uint32_t connector, uint32_t width, uint32_t height) {
  if (!driven(connector)) return false;
  for (uint32_t i = 0; i < display_->mode_count(); ++i)
    if (display_->mode(i)->timing.hdisplay == width && display_->mode(i)->timing.vdisplay == height) return set_mode(i);
  return false;
}

}  // namespace hardware
