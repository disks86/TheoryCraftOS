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

// Follows nouveau (MIT license): dispnv50/head.c (nv50_head_atomic_check_mode), head907d.c, sor907d.c, disp.c (the SOR protocol choice),
// nvkm/engine/disp/nv50.c (nv50_disp_super_2_1, _2_2, _2_2_dp), gf119.c (gf119_disp_super, gf119_head_rgclk), gf119_sor.c / g94_sor.c
// (the DP link, pattern, drive and power registers) and dp.c (the link configuration loop). Numbers are in NvDisplayHw.hpp.
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplayMode.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplayHw.hpp"

namespace hardware {
namespace nv {

using namespace disp_hw;

namespace {
uint32_t lowest_bit(uint32_t mask) { return static_cast<uint32_t>(__builtin_ctz(mask)); }
}  // namespace

bool compute_head_raster(const DisplayTiming& t, HeadRaster* out) {
  if (!out || t.clock_khz == 0 || t.hdisplay == 0 || t.vdisplay == 0) return false;
  if (t.hsync_start < t.hdisplay || t.hsync_end <= t.hsync_start || t.htotal < t.hsync_end || t.htotal > 0x7fff) return false;
  if (t.vsync_start < t.vdisplay || t.vsync_end <= t.vsync_start || t.vtotal < t.vsync_end || t.vtotal > 0x7fff) return false;
  // drm_mode_set_crtcinfo(mode, CRTC_INTERLACE_HALVE_V): an interlaced mode's vertical values count lines of one field.
  uint32_t vdisplay = t.vdisplay, vsync_start = t.vsync_start, vsync_end = t.vsync_end, vtotal = t.vtotal;
  if (t.interlaced) {
    vdisplay /= 2;
    vsync_start /= 2;
    vsync_end /= 2;
    vtotal /= 2;
    if (vsync_end <= vsync_start || vdisplay == 0) return false;
  }
  const uint32_t hblank_end = t.htotal, vblank_end = vtotal;     // max(sync end, total)
  HeadRaster r;
  r.h_total = t.htotal;
  r.h_sync_end = t.hsync_end - t.hsync_start - 1;
  r.h_blank_end = hblank_end - t.hsync_start - 1;
  r.h_blank_start = r.h_blank_end + t.hdisplay;
  r.v_total = vtotal;
  r.v_sync_end = vsync_end - vsync_start - 1;
  r.v_blank_end = vblank_end - vsync_start - 1;
  r.v_blank_start = r.v_blank_end + vdisplay;
  // "Safe underestimate, even 0 works"
  r.blank_us = vtotal > vdisplay + 2 ? static_cast<uint32_t>(uint64_t{vtotal - vdisplay - 2} * r.h_total * 1000 / t.clock_khz) : 0;
  if (t.interlaced) {
    r.v_blank2_end = r.v_total + r.v_blank_end;
    r.v_blank2_start = r.v_blank2_end + vdisplay;
    r.v_total = r.v_total * 2 + 1;
  } else {
    r.v_blank2_end = 0;
    r.v_blank2_start = 1;
  }
  r.clock_khz = t.clock_khz;
  r.hdisplay = t.hdisplay;
  r.vdisplay = t.vdisplay;
  r.interlaced = t.interlaced;
  *out = r;
  return true;
}

bool route_output(const DcbOutput& output, uint32_t sor_mask, uint32_t head_mask, uint32_t head_wanted, uint32_t clock_khz, OutputRoute* out) {
  if (!out) return false;
  if (output.type != DcbOutputType::Tmds && output.type != DcbOutputType::Dp && output.type != DcbOutputType::Lvds) return false;
  if (output.location != 0) return false;                      // an external encoder (PIOR) is not driven here
  const uint32_t sors = output.or_mask & sor_mask & 0xff;
  const uint32_t heads = output.heads & head_mask & 0xff;
  if (sors == 0 || heads == 0) return false;
  OutputRoute r;
  r.sor = lowest_bit(sors);
  r.head = (head_wanted < 8 && (heads & (1u << head_wanted))) ? head_wanted : lowest_bit(heads);
  const uint8_t link = output.sorconf_link ? output.sorconf_link : 1;
  switch (output.type) {
    case DcbOutputType::Tmds:
      // disp.c: single link A or B, dual link from 165 MHz when the entry has both links (HDMI 2.0's high speed mode is not done here)
      if (link == 3 && clock_khz >= 165000) {
        r.protocol = kProtocolDualTmds;
        r.link = 3;
      } else {
        r.protocol = (link & 1) ? kProtocolTmdsA : kProtocolTmdsB;
        r.link = (link & 1) ? 1 : 2;
      }
      break;
    case DcbOutputType::Dp:
      r.protocol = (link & 1) ? kProtocolDpA : kProtocolDpB;
      r.link = (link & 1) ? 1 : 2;
      r.dp = true;
      break;
    default:
      r.protocol = kProtocolLvds;
      r.link = link;
      r.panel = true;
      break;
  }
  *out = r;
  return true;
}

bool gf119_mode_methods(const NvDisplayEngine& engine, uint32_t head, const HeadRaster& raster, const OutputRoute& route, uint32_t depth_bpp,
                        bool hsync_negative, bool vsync_negative, const ScanoutSurface& surface, MethodList* out) {
  if (!out || engine.display_class() != DisplayClass::Gf119 || head >= NvDisplayEngine::kMaxHeads || route.sor >= NvDisplayEngine::kMaxSors) return false;
  uint32_t depth = 0;
  switch (depth_bpp) {
    case 18: depth = m907d::kDepth18; break;
    case 24: depth = m907d::kDepth24; break;
    case 30: depth = m907d::kDepth30; break;
    default: return false;
  }
  const uint32_t h = head * m907d::kHeadStride;
  const uint32_t hz = raster.clock_khz * 1000;
  const uint32_t view_in = raster.vdisplay << 16 | raster.hdisplay;
  bool ok = out->add(m907d::kSorControl + route.sor * m907d::kSorStride, (1u << head) | route.protocol << 8) &&
            out->add(m907d::kHeadOutputResource + h, (hsync_negative ? 1u << 3 : 0) | (vsync_negative ? 1u << 4 : 0) | depth << 6) &&
            out->add(m907d::kHeadControl + h, m907d::kHeadControlBase | head << 25 | (raster.interlaced ? 1 : 0)) &&
            out->add(m907d::kHeadPresentControl + h, 0) && out->add(m907d::kHeadRasterSize + h, raster.raster_size()) &&
            out->add(m907d::kHeadSyncEnd + h, raster.sync_end()) && out->add(m907d::kHeadBlankEnd + h, raster.blank_end()) &&
            out->add(m907d::kHeadBlankStart + h, raster.blank_start()) && out->add(m907d::kHeadVertBlank2 + h, raster.vert_blank2()) &&
            out->add(m907d::kHeadUnknown42c + h, 0) && out->add(m907d::kHeadUnknown430 + h, 0xffffff00) &&
            out->add(m907d::kHeadPixelClock + h, hz) && out->add(m907d::kHeadPixelClockConfig + h, m907d::kHeadPixelClockConfigValue) &&
            out->add(m907d::kHeadPixelClockMax + h, hz) && out->add(m907d::kHeadViewportPointOut + h, 0) &&
            out->add(m907d::kHeadViewportSizeIn + h, view_in) && out->add(m907d::kHeadViewportSizeOut + h, view_in) &&
            out->add(m907d::kHeadViewportSizeOut + 4 + h, view_in) && out->add(m907d::kHeadViewportSizeOut + 8 + h, view_in);
  if (!ok) return false;
  engine.surface_methods(head, surface, out);
  return !out->overflowed();
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// DisplayPort
// ---------------------------------------------------------------------------------------------------------------------------------------
const DpDriveTable& placeholder_dp_drive_table() {
  // Monotonic in both levels, in the registers' 8-bit fields: a shape, not a board's calibration.
  static const DpDriveTable table = [] {
    DpDriveTable t;
    for (uint32_t s = 0; s < 4; ++s)
      for (uint32_t p = 0; p < 4; ++p) {
        t.level[s][p].drive_current = static_cast<uint8_t>(0x10 + s * 0x0c + p * 0x04);
        t.level[s][p].pre_emphasis = static_cast<uint8_t>(p * 0x0c);
        t.level[s][p].tx_pu = static_cast<uint8_t>(0x20 + s * 0x10);
        t.level[s][p].post_cursor = 0;
      }
    return t;
  }();
  return table;
}

bool NvSorDp::set_link(uint8_t bandwidth_code, uint32_t lanes, bool enhanced_framing) {
  if (lanes != 1 && lanes != 2 && lanes != 4) return false;
  if (bandwidth_code == 0 || (bandwidth_code << kSorClockSpeedShift) & ~kSorClockSpeedMask) return false;
  r_.mask(kGf119SorClock + sor_base(), kSorClockSpeedMask, uint32_t{bandwidth_code} << kSorClockSpeedShift);
  r_.mask(kSorDpCtrl + link_base(), kSorDpCtrlMask, ((1u << lanes) - 1) << 16 | (enhanced_framing ? kSorDpEnhancedFraming : 0));
  // g94_sor_dp_power: the lanes in use powered, then the change kicked and waited for
  uint32_t power = 0;
  for (uint32_t i = 0; i < lanes; ++i) power |= 1u << kGf119DpLaneMap[i];
  r_.mask(kSorDpLaneEnable + link_base(), kSorDpPowerLaneMask, power);
  r_.mask(kSorDpPower + sor_base(), kSorDpPowerPending, kSorDpPowerPending);
  bool done = false;
  for (uint32_t i = 0; i < timeouts_.attempts && !done; ++i) {
    done = !(r_.read32(kSorDpPower + sor_base()) & kSorDpPowerPending);
    if (!done) r_.delay_us(timeouts_.poll_us);
  }
  if (!done) return false;
  lanes_ = lanes;
  bw_ = bandwidth_code;
  enhanced_ = enhanced_framing;
  return true;
}

bool NvSorDp::set_pattern(uint8_t pattern) {
  uint32_t data = 0;
  switch (pattern) {
    case 0: data = 0x10101010; break;
    case 1: data = 0x01010101; break;
    case 2: data = 0x02020202; break;
    case 3: data = 0x03030303; break;
    default: return false;
  }
  r_.mask(((link_ & 1) ? kSorDpPatternA : kSorDpPatternB) + sor_base(), kSorDpPatternMask, data);
  return true;
}

bool NvSorDp::set_drive(uint8_t lane_set) {
  if (lanes_ == 0) return false;
  const uint32_t swing = lane_set & Dpcd::kTrainVoltageMask;
  const uint32_t pre = (lane_set & Dpcd::kTrainPreEmphasisMask) >> Dpcd::kTrainPreEmphasisShift;
  const DpDriveLevel& level = drive_.level[swing][pre];
  const uint32_t l = link_base();
  for (uint32_t lane = 0; lane < lanes_; ++lane) {
    const uint32_t shift = kGf119DpLaneMap[lane] * 8;
    const uint32_t current = r_.read32(kSorDpDriveCurrent + l) & ~(0xffu << shift);
    const uint32_t emphasis = r_.read32(kSorDpPreEmphasis + l) & ~(0xffu << shift);
    uint32_t enable = r_.read32(kSorDpLaneEnable + l);
    if ((enable & 0x0000ff00) < (uint32_t{level.tx_pu} << 8) || lane == 0) enable = (enable & ~0x0000ff00u) | uint32_t{level.tx_pu} << 8;
    r_.write32(kSorDpDriveCurrent + l, current | uint32_t{level.drive_current} << shift);
    r_.write32(kSorDpPreEmphasis + l, emphasis | uint32_t{level.pre_emphasis} << shift);
    r_.write32(kSorDpLaneEnable + l, enable);
    const uint32_t post = r_.read32(kSorDpPostCursor + l) & ~(0xffu << shift);
    r_.write32(kSorDpPostCursor + l, post | uint32_t{level.post_cursor} << shift);
  }
  return true;
}

DpTrainResult train_dp_link(IDpcdAux& aux, NvSorDp& sor, const Dpcd::Capabilities& caps, const DcbOutput& output, uint64_t data_kBps) {
  DpTrainResult result;
  if (!caps.valid) return result;
  static constexpr uint8_t kRates[] = {0x1e, 0x14, 0x0a, 0x06};
  uint8_t max_rate = caps.max_link_rate_code;
  if (output.dp_max_link_rate && output.dp_max_link_rate < max_rate) max_rate = output.dp_max_link_rate;
  uint32_t max_lanes = caps.max_lanes;
  if (output.dp_max_lanes && output.dp_max_lanes < max_lanes) max_lanes = output.dp_max_lanes;
  if (max_lanes >= 4) max_lanes = 4;
  else if (max_lanes >= 2) max_lanes = 2;
  for (const uint8_t rate : kRates) {
    if (rate > max_rate) continue;
    for (uint32_t lanes = max_lanes; lanes >= 1; lanes >>= 1) {
      if (uint64_t{lanes} * rate * 27000 < data_kBps) break;     // fewer lanes carry less still
      if (result.attempts < DpTrainResult::kMaxAttempts) {
        result.tried_bandwidth[result.attempts] = rate;
        result.tried_lanes[result.attempts] = static_cast<uint8_t>(lanes);
      }
      ++result.attempts;
      if (!sor.set_link(rate, lanes, caps.enhanced_framing)) continue;
      const DpLinkTrainer::Outcome o = DpLinkTrainer::train(aux, sor, caps, rate, lanes);
      result.last = o.result;
      if (o.trained()) {
        result.trained = true;
        result.bandwidth_code = rate;
        result.lanes = lanes;
        return result;
      }
    }
  }
  return result;
}

DpTransferUnit compute_dp_transfer_unit(const HeadRaster& raster, uint8_t bandwidth_code, uint32_t lanes, bool enhanced_framing, uint32_t depth_bpp) {
  DpTransferUnit out;
  if (raster.clock_khz == 0 || bandwidth_code == 0 || (lanes != 1 && lanes != 2 && lanes != 4) || depth_bpp == 0) return out;
  const uint64_t khz = raster.clock_khz;
  const uint64_t link_kBps = uint64_t{bandwidth_code} * 27000;
  constexpr uint32_t symbol = 100000;
  // symbols per hblank / vblank ("algorithm taken from comments in tegra driver")
  int64_t h = static_cast<int64_t>(raster.h_blank_end) + raster.h_total - raster.h_blank_start - 7;
  h = h * static_cast<int64_t>(link_kBps) / static_cast<int64_t>(khz);
  h = h - 3 * (enhanced_framing ? 1 : 0) - static_cast<int64_t>(12 / lanes);
  int64_t v = static_cast<int64_t>(raster.v_blank_start) - raster.v_blank_end - 25;
  v = v * static_cast<int64_t>(link_kBps) / static_cast<int64_t>(khz);
  v = v - static_cast<int64_t>(36 / lanes + 3) - 1;
  out.hblank_symbols = h > 0 ? static_cast<uint32_t>(h) : 0;
  out.vblank_symbols = v > 0 ? static_cast<uint32_t>(v) : 0;

  const uint64_t link_data_rate = khz * depth_bpp / 8 / lanes;
  const uint64_t link_ratio = link_data_rate * symbol / link_kBps;
  if (link_ratio >= symbol) return out;      // the link cannot carry the mode
  uint32_t best_diff = 64 * symbol, best_tu = 0, best_a = 0, best_f = 0, best_i = 0;
  for (uint32_t tu = 64; tu >= 32; --tu) {
    const uint32_t tu_valid = static_cast<uint32_t>(link_ratio * tu);
    uint32_t vtu_i = tu_valid / symbol, vtu_f = 0, vtu_a = 0;
    uint32_t calc = vtu_i * symbol;
    uint32_t diff = tu_valid - calc;
    if (diff) {
      if (diff >= symbol / 2) {
        vtu_f = symbol / (symbol - diff);
        if (symbol - vtu_f * diff) ++vtu_f;
        if (vtu_f <= 15) {
          vtu_a = 1;
          calc += symbol - symbol / vtu_f;
        } else {
          vtu_a = 0;
          vtu_f = 1;
          calc += symbol;
        }
      } else {
        vtu_a = 0;
        vtu_f = symbol / diff < 15 ? symbol / diff : 15;
        calc += symbol / vtu_f;
      }
      diff = calc - tu_valid;
    } else {
      // no remainder, but the hardware does not like a zero fraction: one less, and the fraction adds a whole symbol back
      vtu_a = 0;
      vtu_f = 1;
      if (vtu_i) --vtu_i;
    }
    if (diff < best_diff) {
      best_diff = diff;
      best_tu = tu;
      best_a = vtu_a;
      best_f = vtu_f;
      best_i = vtu_i;
      if (diff == 0) break;
    }
  }
  if (best_tu == 0) return out;
  // "XXX close to vbios numbers, but not right"
  uint64_t unk = (symbol - link_ratio) * best_tu;
  unk *= link_ratio;
  unk /= symbol;
  unk /= symbol;
  unk += 6;
  out.valid = true;
  out.tu = best_tu;
  out.vtu_a = best_a;
  out.vtu_f = best_f;
  out.vtu_i = best_i;
  out.watermark = static_cast<uint32_t>(unk);
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// NvModeSetter
// ---------------------------------------------------------------------------------------------------------------------------------------
void NvModeSetter::supervisor_2(uint32_t masks[NvDisplayEngine::kMaxHeads]) {
  INvRegisters& r = engine_.registers();
  // 2.1: the VPLL of every head whose clock changes, at the clock now in its assembly state
  for (uint32_t h = 0; h < NvDisplayEngine::kMaxHeads; ++h) {
    if (!(masks[h] & kSuperHeadClock)) continue;
    const uint32_t hz = r.read32(kGf119Assembly + m907d::kHeadPixelClock + h * m907d::kHeadStride);
    if (hz && clock_ && !clock_->set_vpll(h, hz / 1000)) vpll_failed_ = true;
  }
  // 2.2: the head being set (the IED scripts nouveau runs here are not)
  const uint32_t h = route_.head;
  if (!(masks[h] & kSuperHeadModeset)) return;
  r.mask(kGf119HeadRgClk + h * 0x800, 0xf, 0);
  const uint32_t soff = route_.sor * kSorStride;
  const uint32_t loff = soff + (route_.link == 2 ? kSorLinkBStride : 0);
  if (route_.dp && tu_.valid) {
    r.mask(kGf119DpAudioH + h * 0x800, 0x0000ffff, tu_.hblank_symbols & 0xffff);
    r.mask(kGf119DpAudioV + h * 0x800, 0x00ffffff, tu_.vblank_symbols & 0xffffff);
    r.mask(kSorDpCtrl + loff, kSorDpTuMask, (tu_.tu & 0x7f) << 2);
    r.mask(kSorDpActiveSym + loff, kSorDpActiveSymMask, tu_.vtu_a << 24 | (tu_.vtu_f & 0xf) << 16 | (tu_.vtu_i & 0x7f) << 8);
    r.mask(kGf119DpWatermark + h * 0x800, 0x0800003f, 0x08000000 | (tu_.watermark & 0x3f));
  }
  // gf119_sor_clock
  const uint32_t div1 = route_.link == 3 ? 1 : 0, div2 = div1;
  if (route_.protocol == kProtocolTmdsA || route_.protocol == kProtocolTmdsB || route_.protocol == kProtocolDualTmds)
    r.mask(kGf119SorClock + soff, kSorClockSpeedMask, 0x0au << kSorClockSpeedShift);
  r.mask(kGf119SorClock + soff, kSorClockDivMask, div2 << 8 | div1);
}

// gf119_disp_super, called from the UPDATE's wait: one stage per call.
void NvModeSetter::poll() {
  if (!active_) return;
  INvRegisters& r = engine_.registers();
  const uint32_t pending = r.read32(kGf119SuperStatus) & 0x7;
  if (pending == 0) return;
  r.write32(kGf119SuperStatus, pending);          // as the interrupt handler clears it
  uint32_t masks[NvDisplayEngine::kMaxHeads] = {};
  for (uint32_t h = 0; h < NvDisplayEngine::kMaxHeads; ++h)
    if (engine_.head_mask() & (1u << h)) masks[h] = r.read32(kGf119SuperHeadMask + h * 0x800);
  if (pending & 0x1) {
    stages_ |= 1;     // 1.0: nothing to write
  } else if (pending & 0x2) {
    stages_ |= 2;
    if (!disabling_) supervisor_2(masks);
  } else if (pending & 0x4) {
    stages_ |= 4;     // 3.0: the IED "on" scripts (not done)
  }
  for (uint32_t h = 0; h < NvDisplayEngine::kMaxHeads; ++h)
    if (engine_.head_mask() & (1u << h)) r.write32(kGf119SuperHeadMask + h * 0x800, 0);
  r.write32(kGf119SuperAck, kSuperAckValue);
}

DisplayStatus NvModeSetter::set_mode(const Request& request) {
  stages_ = 0;
  vpll_failed_ = false;
  dp_ = DpTrainResult();
  tu_ = DpTransferUnit();
  if (engine_.display_class() != DisplayClass::Gf119) return DisplayStatus::NotImplemented;
  if (!engine_.claimed()) {
    const DisplayStatus s = engine_.claim();
    if (s != DisplayStatus::Ok) return s;
  }
  HeadRaster raster;
  if (!compute_head_raster(request.timing, &raster)) return DisplayStatus::BadArgument;
  if (request.depth_bpp != 18 && request.depth_bpp != 24 && request.depth_bpp != 30) return DisplayStatus::BadArgument;
  OutputRoute route;
  if (!route_output(request.output, engine_.sor_mask(), engine_.head_mask(), request.head, request.timing.clock_khz, &route)) return DisplayStatus::NoOutput;
  if (request.edp && route.dp) route.panel = true;
  const ScanoutSurface& s = request.surface;
  if (!s.valid() || !s.linear_32bpp() || (s.offset & 0xff) || (s.pitch_bytes & 0xff) || s.pitch_bytes > 0x3ff00 || s.width != request.timing.hdisplay ||
      s.height != request.timing.vdisplay || uint64_t{s.width} * 4 > s.pitch_bytes)
    return DisplayStatus::UnsupportedSurface;

  INvRegisters& r = engine_.registers();
  if (route.panel) {   // nouveau powers an eDP/LVDS panel before it trains or lights it (nvkm_dp_enable / the LVDS script)
    if (!gpio_ || !gpio_->set_function(kGpioPanelPower, true)) return DisplayStatus::NoOutput;
    r.delay_us(200000);
  }
  if (route.dp) {
    if (!aux_) return DisplayStatus::NoOutput;
    Dpcd::Capabilities caps;
    if (!Dpcd::read_capabilities(*aux_, request.edp, &caps)) return DisplayStatus::LinkTraining;
    NvSorDp sor(r, route.sor, route.link, drive_, engine_.timeouts());
    dp_ = train_dp_link(*aux_, sor, caps, request.output, uint64_t{request.timing.clock_khz} * request.depth_bpp / 8);
    if (!dp_.trained) return DisplayStatus::LinkTraining;
    tu_ = compute_dp_transfer_unit(raster, dp_.bandwidth_code, dp_.lanes, caps.enhanced_framing, request.depth_bpp);
  }
  MethodList methods;
  ScanoutSurface surface = s;
  if (surface.context_dma == 0) surface.context_dma = NvDisplayEngine::vram_handle();
  if (!gf119_mode_methods(engine_, route.head, raster, route, request.depth_bpp, request.timing.hsync_negative, request.timing.vsync_negative, surface, &methods))
    return DisplayStatus::BadArgument;
  route_ = route;
  raster_ = raster;
  depth_ = request.depth_bpp;
  active_ = true;
  engine_.set_poller(this);
  const DisplayStatus status = engine_.core_update(methods);
  engine_.set_poller(nullptr);
  active_ = false;
  if (status != DisplayStatus::Ok) return status;
  if (vpll_failed_) return DisplayStatus::NotLatched;
  // The armed state has to hold the new raster.
  const uint32_t h = route.head * m907d::kHeadStride;
  if (r.read32(kGf119Armed + m907d::kHeadRasterSize + h) != raster.raster_size() || r.read32(kGf119Armed + m907d::kHeadPixelClock + h) != raster.clock_khz * 1000)
    return DisplayStatus::NotLatched;
  return DisplayStatus::Ok;
}

DisplayStatus NvModeSetter::disable(uint32_t head, uint32_t sor) {
  stages_ = 0;
  if (engine_.display_class() != DisplayClass::Gf119) return DisplayStatus::NotImplemented;
  if (!engine_.claimed()) return DisplayStatus::NotClaimed;
  if (head >= NvDisplayEngine::kMaxHeads || !(engine_.head_mask() & (1u << head)) || sor >= NvDisplayEngine::kMaxSors) return DisplayStatus::BadArgument;
  MethodList methods;
  if (!methods.add(m907d::kSorControl + sor * m907d::kSorStride, 0) || !methods.add(m907d::kHeadIso + head * m907d::kHeadStride, 0))
    return DisplayStatus::BadArgument;
  disabling_ = true;
  active_ = true;
  engine_.set_poller(this);
  const DisplayStatus status = engine_.core_update(methods);
  engine_.set_poller(nullptr);
  active_ = false;
  disabling_ = false;
  if (status != DisplayStatus::Ok) return status;
  INvRegisters& r = engine_.registers();
  if (r.read32(kGf119Armed + m907d::kSorControl + sor * m907d::kSorStride) != 0 || r.read32(kGf119Armed + m907d::kHeadIso + head * m907d::kHeadStride) != 0)
    return DisplayStatus::NotLatched;
  return DisplayStatus::Ok;
}

}  // namespace nv
}  // namespace hardware
