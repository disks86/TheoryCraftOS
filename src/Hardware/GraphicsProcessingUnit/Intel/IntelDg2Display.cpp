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

// Follows the i915 driver (Copyright Intel Corporation; MIT license), Linux v6.12: display/intel_display_power.c, intel_display_power_well.c,
// intel_cdclk.c, intel_dp_aux.c, intel_gmbus.c, intel_ddi.c, intel_display.c, skl_watermark.c; and the DRM DP helper's AUX retry rules.
#include "Hardware/GraphicsProcessingUnit/Intel/IntelDg2Display.hpp"

namespace hardware {
namespace intel_dg2 {

namespace {
// DP_AUX_CH_CTL (intel_dp_aux_regs.h)
constexpr uint32_t kAuxSendBusy = 1u << 31, kAuxDone = 1u << 30, kAuxInterrupt = 1u << 29, kAuxTimeOutError = 1u << 28, kAuxTimeOutMax = 3u << 26,
                   kAuxReceiveError = 1u << 25, kAuxSizeShift = 20, kAuxSizeMask = 0x1Fu << 20;
constexpr uint32_t kAuxFwSyncPulse = (18u - 1) << 5;   // intel_dp_aux_fw_sync_len: precharge 10 + preamble 8
constexpr uint32_t kAuxSyncPulse = 32u - 1;            // intel_dp_aux_sync_len: precharge 16 + preamble 16
constexpr uint32_t kAuxDoneTimeoutUs = 10000;          // intel_dp_aux_wait_done
constexpr uint32_t kAuxXferTries = 5;                  // "must try at least 3 times according to DP spec"
constexpr uint32_t kDpcdTries = 32;                    // drm_dp_dpcd_access
constexpr uint32_t kI2cTries = 7, kAuxRetryUs = 500;   // drm_dp_i2c_do_msg; AUX_RETRY_INTERVAL

constexpr uint32_t kWellTimeoutUs = 1000, kFuseTimeoutUs = 1000;   // (i915: 1 ms for the state, 1 ms for the fuses)
constexpr uint32_t kDbufSettleUs = 10;
constexpr uint32_t kCdclkLockTimeoutUs = 1000;
constexpr uint32_t kTransConfTimeoutUs = 100000;
constexpr uint32_t kIdlePatternTimeoutUs = 2000;
constexpr uint32_t kGmbusWaitUs = 50000, kGmbusIdleUs = 10000;

// dg2_cdclk_table: 38.4 MHz reference, ratio 34
struct CdclkEntry {
  uint32_t khz;
  uint16_t waveform;
};
constexpr CdclkEntry kCdclk[] = {
    {163200, 0x8888}, {204000, 0x9248}, {244800, 0xA4A4}, {285600, 0xA54A}, {326400, 0xAAAA}, {367200, 0xAD5A}, {408000, 0xB6B6},
    {448800, 0xDBB6}, {489600, 0xEEEE}, {530400, 0xF7DE}, {571200, 0xFEFE}, {612000, 0xFFFE}, {652800, 0xFFFF},
};

uint32_t pack_be(ConstByteView bytes) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < 4; ++i) v |= uint32_t{i < bytes.size() ? bytes[i] : uint8_t{0}} << ((3 - i) * 8);
  return v;
}
void unpack_be(uint32_t v, ByteView out) {
  for (uint32_t i = 0; i < 4 && i < out.size(); ++i) out[i] = static_cast<uint8_t>(v >> ((3 - i) * 8));
}
uint32_t div_round_up(uint64_t a, uint64_t b) { return static_cast<uint32_t>((a + b - 1) / b); }

// compute_m_n with a constant N and intel_reduce_m_n_ratio
void compute_m_n(uint64_t m, uint64_t n, uint32_t constant_n, uint32_t* ret_m, uint32_t* ret_n) {
  uint64_t rn = constant_n, rm = n ? m * rn / n : 0;
  while (rm > kDataLinkMnMask || rn > kDataLinkMnMask) {
    rm >>= 1;
    rn >>= 1;
  }
  *ret_m = static_cast<uint32_t>(rm);
  *ret_n = static_cast<uint32_t>(rn);
}
}  // namespace

const char* stage_name(Stage stage) {
  switch (stage) {
    case Stage::Done: return "done";
    case Stage::Unsupported: return "the mode or output is not supported";
    case Stage::Cdclk: return "CDCLK";
    case Stage::Pll: return "the port PLL (MPLLB) did not lock";
    case Stage::DdiPower: return "the DDI IO power well";
    case Stage::LinkTraining: return "DisplayPort link training";
    case Stage::Buffer: return "the DDI buffer did not become active";
    case Stage::Transcoder: return "the transcoder did not start";
  }
  return "?";
}

LinkMn compute_link_mn(uint32_t bits_per_pixel, uint32_t lanes, uint32_t pixel_khz, uint32_t link_khz) {
  LinkMn mn;
  compute_m_n(uint64_t{pixel_khz} * bits_per_pixel, uint64_t{link_khz} * lanes * 8, 0x8000000, &mn.data_m, &mn.data_n);
  compute_m_n(pixel_khz, link_khz, 0x80000, &mn.link_m, &mn.link_n);
  return mn;
}

uint32_t cdclk_for(uint32_t pixel_khz) {
  const uint32_t need = (pixel_khz + 1) / 2;    // intel_pixel_rate_to_cdclk: two pixels per CDCLK from display version 10
  for (const CdclkEntry& e : kCdclk)
    if (e.khz >= need) return e.khz;
  return 0;
}

uint16_t cdclk_waveform(uint32_t cdclk_khz) {
  for (const CdclkEntry& e : kCdclk)
    if (e.khz == cdclk_khz) return e.waveform;
  return 0;
}

Watermark plane_watermark(const CrtcMode& m, uint32_t ddb_blocks) {
  // skl_wm_method1 for a linear plane: bytes fetched during the latency, in 512-byte blocks, plus one; lines as the latency over the line time, plus one.
  Watermark wm;
  if (m.pixel_clock_khz == 0 || m.h_total == 0) return wm;
  const uint64_t bytes = uint64_t{m.pixel_clock_khz} * 4 * kAssumedLatencyUs / 1000;
  wm.blocks = div_round_up(bytes, 512) + 1;
  const uint64_t line_ns = uint64_t{m.h_total} * 1000000 / m.pixel_clock_khz;
  wm.lines = line_ns ? div_round_up(uint64_t{kAssumedLatencyUs} * 1000, line_ns) + 1 : 1;
  if (wm.blocks >= ddb_blocks) wm.blocks = ddb_blocks - 1;
  if (wm.lines > kPlaneWmLinesMask) wm.lines = kPlaneWmLinesMask;
  return wm;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// AUX
// ---------------------------------------------------------------------------------------------------------------------------------------
int32_t DpAux::xfer(ConstByteView send, ByteView recv) {
  if (send.empty() || send.size() > 20 || recv.size() > 20) return kInvalid;
  const uint64_t ctl = aux_ch_ctl(port_.port);
  uint32_t status = 0;
  uint32_t idle_try = 0;
  for (; idle_try < 3; ++idle_try) {
    status = r_.read32(ctl);
    if (!(status & kAuxSendBusy)) break;
    delay_us(clock_, 1000);
  }
  if (idle_try == 3) return kBusy;
  const uint32_t send_ctl = kAuxSendBusy | kAuxDone | kAuxInterrupt | kAuxTimeOutError | kAuxTimeOutMax | kAuxReceiveError |
                            static_cast<uint32_t>(send.size()) << kAuxSizeShift | kAuxFwSyncPulse | kAuxSyncPulse;
  bool done = false;
  for (uint32_t attempt = 0; attempt < kAuxXferTries && !done; ++attempt) {
    for (uint32_t i = 0; i < send.size(); i += 4) r_.write32(aux_ch_data(port_.port, i / 4), pack_be(send.subview(i)));
    r_.write32(ctl, send_ctl);
    (void)wait_for(r_, clock_, ctl, kAuxSendBusy, 0, kAuxDoneTimeoutUs);
    status = r_.read32(ctl);
    r_.write32(ctl, status | kAuxDone | kAuxTimeOutError | kAuxReceiveError);   // clear done and the errors
    if (status & kAuxTimeOutError) continue;
    if (status & kAuxReceiveError) {
      delay_us(clock_, 400);
      continue;
    }
    done = (status & kAuxDone) != 0;
  }
  if (!done) return (status & kAuxTimeOutError) ? kTimedOut : kBusy;
  const uint32_t received = (status & kAuxSizeMask) >> kAuxSizeShift;
  if (received == 0 || received > 20) return kBusy;
  const uint32_t n = received < recv.size() ? received : static_cast<uint32_t>(recv.size());
  for (uint32_t i = 0; i < n; i += 4) unpack_be(r_.read32(aux_ch_data(port_.port, i / 4)), recv.subview(i, n - i < 4 ? n - i : 4));
  return static_cast<int32_t>(n);
}

int32_t DpAux::transfer(uint8_t request, uint32_t address, ByteView buffer, uint8_t* reply) {
  if (buffer.size() > kMaxPayload) return kInvalid;
  const uint32_t size = static_cast<uint32_t>(buffer.size());
  uint8_t tx_bytes[20] = {}, rx_bytes[20] = {};
  const ByteView tx(tx_bytes), rx(rx_bytes);
  tx[0] = static_cast<uint8_t>(request << 4 | ((address >> 16) & 0xF));
  tx[1] = static_cast<uint8_t>(address >> 8);
  tx[2] = static_cast<uint8_t>(address);
  tx[3] = static_cast<uint8_t>(size ? size - 1 : 0);
  const uint8_t kind = request & ~kI2cMot;
  if (kind == kNativeWrite || kind == kI2cWrite) {
    if (size && !tx.subview(4, size).copy_from(buffer)) return kInvalid;
    const int32_t got = xfer(tx.first(size ? 4 + size : 3), rx.first(2));
    if (got <= 0) return got < 0 ? got : kBusy;
    *reply = rx[0] >> 4;
    return got > 1 ? (rx[1] < size ? rx[1] : static_cast<int32_t>(size)) : static_cast<int32_t>(size);
  }
  if (kind == kNativeRead || kind == kI2cRead) {
    const int32_t got = xfer(tx.first(size ? 4 : 3), rx.first(size + 1));
    if (got <= 0) return got < 0 ? got : kBusy;
    *reply = rx[0] >> 4;
    const uint32_t n = static_cast<uint32_t>(got) - 1;
    if (n > size || !buffer.first(n).copy_from(rx.subview(1, n))) return kInvalid;
    return static_cast<int32_t>(n);
  }
  return kInvalid;
}

bool DpAux::native(uint8_t request, uint32_t address, ByteView buffer) {
  for (uint32_t attempt = 0; attempt < kDpcdTries; ++attempt) {
    uint8_t reply = 0;
    const int32_t n = transfer(request, address, buffer, &reply);
    if (n == kTimedOut) return false;          // (no sink: the DRM helper would try 32 times, each a full AUX timeout)
    if (n < 0) continue;
    const uint8_t native_reply = reply & 3;
    if (native_reply == kReplyAck) return static_cast<uint32_t>(n) == buffer.size();
    if (native_reply == kNativeNack) return false;
    delay_us(clock_, kAuxRetryUs);              // defer
  }
  return false;
}

bool DpAux::dpcd_read(uint32_t address, ByteView out) {
  for (uint32_t at = 0; at < out.size(); at += kMaxPayload) {
    const uint32_t n = out.size() - at < kMaxPayload ? static_cast<uint32_t>(out.size() - at) : kMaxPayload;
    if (!native(kNativeRead, address + at, out.subview(at, n))) return false;
  }
  return true;
}

bool DpAux::dpcd_write(uint32_t address, ConstByteView data) {
  uint8_t copy[kMaxPayload] = {};
  for (uint32_t at = 0; at < data.size(); at += kMaxPayload) {
    const uint32_t n = data.size() - at < kMaxPayload ? static_cast<uint32_t>(data.size() - at) : kMaxPayload;
    const ByteView piece = ByteView(copy).first(n);
    if (!piece.copy_from(data.subview(at, n)) || !native(kNativeWrite, address + at, piece)) return false;
  }
  return true;
}

bool DpAux::i2c_piece(uint8_t request, uint8_t address, ByteView buffer) {
  uint32_t done = 0, defers = 0;
  for (uint32_t attempt = 0; attempt < kI2cTries + defers && defers <= kI2cTries;) {
    uint8_t reply = 0;
    const int32_t n = transfer(request, address, buffer.subview(done), &reply);
    if (n == kTimedOut) return false;
    if (n < 0) {
      ++attempt;
      continue;
    }
    if ((reply & 3) == kNativeNack) return false;
    if ((reply & 3) == kNativeDefer) {
      delay_us(clock_, kAuxRetryUs);
      ++attempt;
      continue;
    }
    if ((reply & 0xC) == kI2cNack) return false;
    if ((reply & 0xC) == kI2cDefer) {
      ++defers;
      delay_us(clock_, kAuxRetryUs);
      ++attempt;
      continue;
    }
    done += static_cast<uint32_t>(n);
    if (done >= buffer.size()) return true;
    if (n == 0) ++attempt;                       // an acknowledged read with no data: try again (drm_dp_i2c_drain_msg)
  }
  return false;
}

bool DpAux::transfer(ArrayView<Message> messages) {
  bool ok = true;
  uint8_t last_request = kI2cWrite, last_address = 0;
  for (Message& m : messages) {
    const uint8_t request = static_cast<uint8_t>((m.read ? kI2cRead : kI2cWrite) | kI2cMot);
    last_request = request;
    last_address = m.address;
    if (!i2c_piece(request, m.address, ByteView())) {
      ok = false;
      break;
    }
    for (uint32_t at = 0; at < m.data.size() && ok; at += kMaxPayload) {
      const uint32_t n = m.data.size() - at < kMaxPayload ? static_cast<uint32_t>(m.data.size() - at) : kMaxPayload;
      ok = i2c_piece(request, m.address, m.data.subview(at, n));
    }
    if (!ok) break;
  }
  // the transaction closed: a bare address without MOT (also after a failure, so the sink's I2C state machine is reset)
  (void)i2c_piece(static_cast<uint8_t>(last_request & ~kI2cMot), last_address, ByteView());
  return ok;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// GMBUS
// ---------------------------------------------------------------------------------------------------------------------------------------
bool Gmbus::wait(uint32_t status) {
  const uint64_t start = clock_.now_us();
  for (;;) {
    const uint32_t v = r_.read32(kGmbus2);
    if (v & kGmbusSatoer) return false;          // NAK
    if (v & status) return true;
    if (clock_.now_us() - start > kGmbusWaitUs) return false;
  }
}

bool Gmbus::read(uint8_t address, ByteView out, uint32_t index_cycle) {
  if (out.size() > kGmbusMaxBytes) return false;
  uint32_t len = static_cast<uint32_t>(out.size());
  r_.write32(kGmbus1, index_cycle | kGmbusCycleWait | len << kGmbusByteCountShift | uint32_t{address} << kGmbusSlaveAddrShift | kGmbusSlaveRead | kGmbusSwRdy);
  uint32_t at = 0;
  while (len) {
    if (!wait(kGmbusHwRdy)) return false;
    uint32_t v = r_.read32(kGmbus3);
    for (uint32_t i = 0; i < 4 && len; ++i, --len, v >>= 8) out[at++] = static_cast<uint8_t>(v);
  }
  return true;
}

bool Gmbus::write(uint8_t address, ConstByteView data) {
  if (data.size() > kGmbusMaxBytes) return false;
  uint32_t len = static_cast<uint32_t>(data.size()), at = 0;
  uint32_t v = 0;
  for (uint32_t i = 0; i < 4 && len; ++i, --len) v |= uint32_t{data[at++]} << (8 * i);
  r_.write32(kGmbus3, v);
  r_.write32(kGmbus1, kGmbusCycleWait | static_cast<uint32_t>(data.size()) << kGmbusByteCountShift | uint32_t{address} << kGmbusSlaveAddrShift | kGmbusSwRdy);
  while (len) {
    if (!wait(kGmbusHwRdy)) return false;
    v = 0;
    for (uint32_t i = 0; i < 4 && len; ++i, --len) v |= uint32_t{data[at++]} << (8 * i);
    r_.write32(kGmbus3, v);
  }
  return true;
}

void Gmbus::reset() {
  (void)wait_for(r_, clock_, kGmbus2, kGmbusActive, 0, kGmbusIdleUs);
  r_.write32(kGmbus1, kGmbusSwClrInt);
  r_.write32(kGmbus1, 0);
  r_.write32(kGmbus0, 0);
}

bool Gmbus::transfer(ArrayView<Message> messages) {
  r_.write32(kGmbus0, pin_ | kGmbusRate100k);
  for (uint32_t i = 0; i < messages.size();) {
    const Message& m = messages[i];
    bool ok;
    uint32_t step = 1;
    const bool index = i + 1 < messages.size() && !m.read && m.data.size() == 1 && messages[i + 1].address == m.address && !messages[i + 1].data.empty();
    if (index) {
      const uint32_t cycle = kGmbusCycleIndex | uint32_t{m.data[0]} << kGmbusSlaveIndexShift;
      const Message& next = messages[i + 1];
      ok = next.read ? read(next.address, next.data, cycle) : write(next.address, next.data);   // (a 1-byte index before a write: the write carries it)
      step = 2;
    } else if (m.read) {
      ok = read(m.address, m.data, 0);
    } else {
      ok = write(m.address, m.data);
    }
    if (ok) ok = wait(kGmbusHwWaitPhase);
    if (!ok) {
      reset();
      return false;
    }
    i += step;
  }
  r_.write32(kGmbus1, kGmbusCycleStop | kGmbusSwRdy);
  const bool idle = wait_for(r_, clock_, kGmbus2, kGmbusActive, 0, kGmbusIdleUs);
  r_.write32(kGmbus0, 0);
  return idle;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------------------------------------------------------------------
// The DDI as DpLinkTrainer's source: DP_TP_CTL of transcoder A for the patterns, DDI_BUF_CTL for the port, the SNPS PHY's TX equalisation for the drive.
class Engine::Source final : public IDpSource {
 public:
  Source(Engine& e, const PortInfo& port, uint32_t lanes, bool enhanced) : e_(e), port_(port), lanes_(lanes), enhanced_(enhanced) {}
  // intel_ddi_prepare_link_retrain: DP_TP_CTL on with pattern 1, then the buffer on and active
  bool start_training() override {
    e_.r_.write32(kDpTpCtlA, kDpTpEnable | kDpTpModeSst | kDpTpPat1 | (enhanced_ ? kDpTpEnhancedFrame : 0));
    e_.r_.write32(ddi_buf_ctl(port_.port), ddi_port_width(lanes_) | kDdiBufEnable);      // (DDI_BUF_TRANS_SELECT(0); no TC PHY ownership on DG2)
    return wait_for(e_.r_, e_.clock_, ddi_buf_ctl(port_.port), kDdiBufIsIdle, 0, kDdiBufActiveTimeoutUs);
  }
  bool set_pattern(uint8_t pattern) override {
    uint32_t bits;
    switch (pattern) {
      case Dpcd::kTrainingPatternDisable: bits = kDpTpNormal; break;
      case Dpcd::kTrainingPattern1: bits = kDpTpPat1; break;
      case Dpcd::kTrainingPattern2: bits = kDpTpPat2; break;
      case Dpcd::kTrainingPattern3: bits = kDpTpPat3; break;
      default: return false;
    }
    rmw(e_.r_, kDpTpCtlA, kDpTpTrainMask, bits);
    return true;
  }
  // intel_ddi_set_idle_link_train (and the wait for the idle patterns), then intel_dp_stop_link_train's normal pattern
  bool complete_training() override {
    rmw(e_.r_, kDpTpCtlA, kDpTpTrainMask, kDpTpIdle);
    const bool idle = wait_for(e_.r_, e_.clock_, kDpTpStatusA, kDpTpStatusIdleDone, kDpTpStatusIdleDone, kIdlePatternTimeoutUs);
    rmw(e_.r_, kDpTpCtlA, kDpTpTrainMask, kDpTpNormal);
    return idle;
  }
  bool set_drive(uint8_t lane_set) override {
    e_.phy_.set_levels(port_, dp_level(lane_set));
    return true;
  }
  void delay_us(uint32_t microseconds) override { intel_dg2::delay_us(e_.clock_, microseconds); }

 private:
  Engine& e_;
  const PortInfo& port_;
  uint32_t lanes_;
  bool enhanced_;
};

bool Engine::enable_well(uint64_t ctl, uint32_t idx, bool fuses, bool fixed_delay) {
  const uint32_t pg = pg_of_well(idx);
  if (fuses && pg == 1 && !wait_for(r_, clock_, kFuseStatus, fuse_pg_dist(0), fuse_pg_dist(0), kFuseTimeoutUs)) return fail("PG0's fuses were not distributed");
  rmw(r_, ctl, 0, pw_request(idx));
  if (fixed_delay) {
    delay_us(clock_, kAuxWellFixedDelayUs);
  } else if (!wait_for(r_, clock_, ctl, pw_state(idx), pw_state(idx), kWellTimeoutUs)) {
    return fail("a power well did not turn on");
  }
  if (fuses && !wait_for(r_, clock_, kFuseStatus, fuse_pg_dist(pg), fuse_pg_dist(pg), kFuseTimeoutUs)) return fail("a power well's fuses were not distributed");
  return true;
}

void Engine::disable_well(uint64_t ctl, uint32_t idx) { rmw(r_, ctl, pw_request(idx), 0); }

bool Engine::init() {
  // icl_display_core_init
  r_.write32(kDcStateEn, 0);
  rmw(r_, kNdeRstwrnOpt, kResetPchHandshakeEnable, kResetPchHandshakeEnable);     // (DG2 has a "PCH": not PCH_NOP)
  if (!enable_well(kPwrWellCtl2, kPwIdx1, true, false)) return false;
  if (!set_cdclk(kCdclk[0].khz)) return false;                                       // bxt_cdclk_init_hw: the lowest
  for (uint32_t s = 0; s < 4; ++s) rmw(r_, kDbufCtlS[s], kDbufTrackerMask, kDbufTracker8);   // gen12_dbuf_slices_config
  for (uint32_t s = 0; s < 2; ++s) {                                                 // gen9_dbuf_enable, with pipe A's slices S1 and S2
    rmw(r_, kDbufCtlS[s], 0, kDbufPowerRequest);
    delay_us(clock_, kDbufSettleUs);
    if (!(r_.read32(kDbufCtlS[s]) & kDbufPowerState)) return fail("a DBUF slice did not power up");
  }
  for (uint64_t abox : kMbusAboxCtl) rmw(r_, abox, kAboxCreditMask, kAboxCredits);  // icl_mbus_init
  uncalibrated_ = phy_.wait_for_calibration();
  rmw(r_, kChickenDcpr2, 0, kDcpr2Wa14011508470);
  r_.write32(kDisplayErrFatalMask, 0xFFFFFFFFu);
  // pipe A's well and PG2 (the tree of Xe_LPD: PGA and PG2 both under PG1)
  if (!enable_well(kPwrWellCtl2, kPwIdxA, true, false)) return false;
  return enable_well(kPwrWellCtl2, kPwIdx2, true, false);
}

bool Engine::enable_aux_power(const PortInfo& port) { return enable_well(kPwrWellCtlAux2, port.aux_well, false, true); }
void Engine::disable_aux_power(const PortInfo& port) { disable_well(kPwrWellCtlAux2, port.aux_well); }
bool Engine::enable_ddi_power(const PortInfo& port) { return enable_well(kPwrWellCtlDdi2, port.ddi_well, false, false); }
void Engine::disable_ddi_power(const PortInfo& port) { disable_well(kPwrWellCtlDdi2, port.ddi_well); }

bool Engine::set_cdclk(uint32_t khz) {
  const uint16_t waveform = cdclk_waveform(khz);
  if (!waveform) return fail("not a CDCLK of DG2's table");
  // _bxt_set_cdclk, display version 11+ without crawl: wa_15010685871 (squash off while the PLL changes), the PLL to the one VCO, the waveform, CDCLK_CTL
  const uint32_t pll = r_.read32(kDePllEnable);
  const bool running = (pll & kDePllEnableBit) && (pll & kDePllLock);
  if (running && (pll & kDePllRatioMask) != kCdclkRatio) {
    r_.write32(kCdclkSquashCtl, 0);
    rmw(r_, kDePllEnable, kDePllEnableBit, 0);
    if (!wait_for(r_, clock_, kDePllEnable, kDePllLock, 0, kCdclkLockTimeoutUs)) return fail("the CDCLK PLL did not unlock");
  }
  if (!running || (pll & kDePllRatioMask) != kCdclkRatio) {
    if (running) r_.write32(kCdclkSquashCtl, 0);
    r_.write32(kDePllEnable, kCdclkRatio);
    r_.write32(kDePllEnable, kCdclkRatio | kDePllEnableBit);
    if (!wait_for(r_, clock_, kDePllEnable, kDePllLock, kDePllLock, kCdclkLockTimeoutUs)) return fail("the CDCLK PLL did not lock");
  }
  r_.write32(kCdclkSquashCtl, kSquashEnable | kSquashWindow15 | waveform);
  // cd2x divider 1 for every entry (vco * popcount(waveform) / (cdclk * 16) is 2), no pipe to sync to, the decimal frequency
  const uint32_t decimal = ((khz - 1000) + 250) / 500;
  r_.write32(kCdclkCtl, kCdclkCd2xDiv1 | kCdclkCd2xPipeNone | (decimal & kCdclkDecimalMask));
  cdclk_ = khz;
  return true;
}

void Engine::program_timings(const CrtcMode& m) {
  // intel_set_transcoder_timings, display version 13: the vblank start through TRANS_SET_CONTEXT_LATENCY (0: it starts at the end of the active
  // lines), TRANS_VBLANK's start field 1 - 1; no borders, so the blank equals the active region's end and the total
  r_.write32(kTransSetContextLatencyA, 0);
  r_.write32(kTransVsyncShiftA, 0);
  r_.write32(kTransHtotalA, (uint32_t{m.h_display} - 1) | (uint32_t{m.h_total} - 1) << 16);
  r_.write32(kTransHblankA, (uint32_t{m.h_display} - 1) | (uint32_t{m.h_total} - 1) << 16);
  r_.write32(kTransHsyncA, (uint32_t{m.h_sync_start} - 1) | (uint32_t{m.h_sync_end} - 1) << 16);
  r_.write32(kTransVtotalA, (uint32_t{m.v_display} - 1) | (uint32_t{m.v_total} - 1) << 16);
  r_.write32(kTransVblankA, 0 | (uint32_t{m.v_total} - 1) << 16);
  r_.write32(kTransVsyncA, (uint32_t{m.v_sync_start} - 1) | (uint32_t{m.v_sync_end} - 1) << 16);
}

void Engine::program_plane(const ModeRequest& q) {
  const CrtcMode& m = q.mode;
  // the plane's share of DBUF and its watermark (skl_ddb_entry_write: the end is inclusive); the other levels and the transition watermark off
  const Watermark wm = plane_watermark(m, kPlaneDdbBlocks);
  r_.write32(kPlaneBufCfgA, (kPlaneDdbBlocks - 1) << 16 | 0);
  r_.write32(kPlaneWmA0, kPlaneWmEnable | (wm.lines & kPlaneWmLinesMask) << kPlaneWmLinesShift | (wm.blocks & kPlaneWmBlocksMask));
  for (uint32_t level = 1; level < kPlaneWmLevels; ++level) r_.write32(kPlaneWmA0 + level * 4, 0);
  r_.write32(kPlaneWmTransA, 0);
  r_.write32(kPipeMbusDboxCtlA, kDboxValue);
  // the plane (skl_program_plane_noarm, then _arm: PLANE_SURF last)
  r_.write32(kPlaneStrideA, q.stride_bytes / 64);
  r_.write32(kPlanePosA, 0);
  r_.write32(kPlaneSizeA, (uint32_t{m.v_display} - 1) << 16 | (uint32_t{m.h_display} - 1));
  r_.write32(kPlaneAuxDistA, 0);
  r_.write32(kPlaneOffsetA, 0);
  r_.write32(kPlaneColorCtlA, kPlaneColorGammaDisable);
  r_.write32(kPlaneCtlA, kPlaneEnable | kPlaneFormatXrgb8888);
  r_.write32(kPlaneSurfA, static_cast<uint32_t>(q.surface));
}

void Engine::flip(uint64_t surface) {
  if (lit_) r_.write32(kPlaneSurfA, static_cast<uint32_t>(surface));
}

void Engine::teardown(const PortInfo& port, Output output, bool pll, bool ddi_power) {
  // intel_disable_transcoder, intel_ddi_disable_transcoder_func, intel_disable_ddi_buf, the clocks (post_disable)
  r_.write32(kPlaneCtlA, 0);
  r_.write32(kPlaneSurfA, 0);
  if (r_.read32(kTransConfA) & kTransConfEnable) {
    rmw(r_, kTransConfA, kTransConfEnable, 0);
    (void)wait_for(r_, clock_, kTransConfA, kTransConfState, 0, kTransConfTimeoutUs);
  }
  r_.write32(kTransDdiFuncCtlA, 0);
  const bool buffer_on = r_.read32(ddi_buf_ctl(port.port)) & kDdiBufEnable;
  rmw(r_, ddi_buf_ctl(port.port), kDdiBufEnable, 0);
  if (output == Output::DisplayPort) rmw(r_, kDpTpCtlA, kDpTpEnable, 0);
  if (buffer_on) (void)wait_for(r_, clock_, ddi_buf_ctl(port.port), kDdiBufIsIdle, kDdiBufIsIdle, kDdiBufActiveTimeoutUs);
  r_.write32(kTransClkSelA, 0);
  if (pll) (void)phy_.disable_pll(port);
  if (ddi_power) disable_ddi_power(port);
}

Stage Engine::enable(const ModeRequest& q) {
  if (lit_ || !q.port || !q.port->drivable) return Stage::Unsupported;
  const PortInfo& port = *q.port;
  const CrtcMode& m = q.mode;
  const bool dp = q.output == Output::DisplayPort;
  if (m.interlaced || m.double_scan || m.h_display == 0 || m.v_display == 0 || m.h_total <= m.h_display || m.v_total <= m.v_display ||
      q.surface == 0 || q.surface >= (uint64_t{1} << 32) || (q.surface & 0xFFF) || q.stride_bytes % 64 || q.stride_bytes < uint32_t{m.h_display} * 4)
    return Stage::Unsupported;
  const MpllbState* pll = dp ? find_dp_mpllb(q.link_khz) : find_hdmi_mpllb(m.pixel_clock_khz);    // (8 bpc: the TMDS clock is the pixel clock)
  if (!pll || (dp && (!q.aux || (q.lanes != 1 && q.lanes != 2 && q.lanes != 4)))) return Stage::Unsupported;
  const uint32_t cdclk = cdclk_for(m.pixel_clock_khz);
  if (!cdclk) return Stage::Unsupported;
  if (cdclk > cdclk_ && !set_cdclk(cdclk)) return Stage::Cdclk;

  // pre-enable (tgl_ddi_pre_enable_dp / intel_ddi_pre_enable_hdmi): the port PLL, the DDI's IO power, the transcoder's clock
  if (!phy_.enable_pll(port, *pll)) {
    teardown(port, q.output, true, false);
    return Stage::Pll;
  }
  if (!enable_ddi_power(port)) {
    teardown(port, q.output, true, false);
    return Stage::DdiPower;
  }
  r_.write32(kTransClkSelA, trans_clk_sel_port(port.phy));
  uint32_t func = trans_ddi_select_port(port.port) | kTransDdiBpc8;
  if (!m.v_sync_negative) func |= kTransDdiPvsync;
  if (!m.h_sync_negative) func |= kTransDdiPhsync;
  if (dp) {
    func |= kTransDdiModeDpSst | ddi_port_width(q.lanes);
    r_.write32(kTransDdiFuncCtlA, func);                 // intel_ddi_config_transcoder_func: everything but the enable
    phy_.set_levels(port, 0);
    const uint8_t d0 = Dpcd::kPowerD0;
    (void)q.aux->dpcd_write(Dpcd::kSetPower, ConstByteView(&d0, 1));
    Source source(*this, port, q.lanes, q.caps.enhanced_framing);
    DpLinkConfig link;
    link.lanes = q.lanes;
    link.rate_khz = q.link_khz;
    training_ = DpLinkTrainer::train(*q.aux, source, q.caps, link.bandwidth_code(), q.lanes);
    if (!training_.trained()) {
      teardown(port, q.output, true, true);
      return Stage::LinkTraining;
    }
    // the transcoder's M/N
    const LinkMn mn = compute_link_mn(24, q.lanes, m.pixel_clock_khz, q.link_khz);
    r_.write32(kPipeDataM1A, kTuSize64 | mn.data_m);
    r_.write32(kPipeDataN1A, mn.data_n);
    r_.write32(kPipeLinkM1A, mn.link_m);
    r_.write32(kPipeLinkN1A, mn.link_n);
  } else {
    func |= q.output == Output::Hdmi ? kTransDdiModeHdmi : kTransDdiModeDvi;
  }

  // the pipe and the transcoder (hsw_crtc_enable: pipe source, PIPE_MISC, hsw_configure_cpu_transcoder)
  r_.write32(kPipeSrcA, (uint32_t{m.h_display} - 1) << 16 | (uint32_t{m.v_display} - 1));
  r_.write32(kPipeMiscA, kPipeMiscBpc8 | kPipeMiscRoundingTrunc);
  program_timings(m);
  r_.write32(kTransMultA, 0);
  rmw(r_, kChickenTransA, kFrameStartDelayMask, 0);     // framestart_delay 1
  r_.write32(kTransConfA, 0);                           // progressive (TRANSCONF_INTERLACE_PF_PD_ILK), not yet enabled
  program_plane(q);

  // intel_enable_ddi: TRANS_DDI_FUNC_CTL2 and _CTL with the enable, the transcoder on
  r_.write32(kTransDdiFuncCtl2A, 0);
  r_.write32(kTransDdiFuncCtlA, func | kTransDdiFuncEnable);
  r_.write32(kTransConfA, kTransConfEnable);
  if (!wait_for(r_, clock_, kTransConfA, kTransConfState, kTransConfState, kTransConfTimeoutUs)) {
    teardown(port, q.output, true, true);
    return Stage::Transcoder;
  }
  if (!dp) {
    // intel_enable_ddi_hdmi: the HDMI drive level, then the buffer (no port width below display version 14)
    phy_.set_levels(port, kHdmiDefaultLevel);
    r_.write32(ddi_buf_ctl(port.port), kDdiBufEnable);
    if (!wait_for(r_, clock_, ddi_buf_ctl(port.port), kDdiBufIsIdle, 0, kDdiBufActiveTimeoutUs)) {
      teardown(port, q.output, true, true);
      return Stage::Buffer;
    }
  }
  lit_ = &port;
  lit_output_ = q.output;
  return Stage::Done;
}

void Engine::disable() {
  if (!lit_) return;
  teardown(*lit_, lit_output_, true, true);
  lit_ = nullptr;
}

}  // namespace intel_dg2
}  // namespace hardware
