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

#include "Hardware/GraphicsProcessingUnit/AMD/Dpcd.hpp"

namespace hardware {

namespace {
constexpr uint8_t kLaneCrDone = 0x1, kLaneEqDone = 0x2, kLaneSymbolLocked = 0x4;
constexpr uint8_t kAlignDone = 0x01;
constexpr uint8_t kMinRevision = 0x10;
constexpr uint32_t kEdidBlockBytes = 128;
constexpr uint32_t kEdpRateUnitKhz = 200;       // DP_SUPPORTED_LINK_RATES: 200 kHz units
}  // namespace

uint8_t Dpcd::LinkStatus::adjust_voltage(uint32_t lane) const {
  if (lane > 3) lane = 3;
  return static_cast<uint8_t>(adjust[lane / 2] >> (lane % 2 * 4) & kTrainVoltageMask);         // (the adjust request: voltage in the nibble's bits 1:0)
}

uint8_t Dpcd::LinkStatus::adjust_pre_emphasis(uint32_t lane) const {
  if (lane > 3) lane = 3;
  return static_cast<uint8_t>((adjust[lane / 2] >> (lane % 2 * 4 + 2) & 0x03) << kTrainPreEmphasisShift);     // (and the pre-emphasis in bits 3:2)
}

bool Dpcd::LinkStatus::clock_recovery_ok(uint32_t lanes) const {
  if (lanes > 4) lanes = 4;
  for (uint32_t lane = 0; lane < lanes; ++lane)
    if (!(lane_status[lane / 2] >> (lane % 2 * 4) & kLaneCrDone)) return false;
  return true;
}

bool Dpcd::LinkStatus::channel_eq_ok(uint32_t lanes) const {
  if (lanes > 4) lanes = 4;
  if (!(align & kAlignDone)) return false;
  for (uint32_t lane = 0; lane < lanes; ++lane) {
    const uint8_t status = static_cast<uint8_t>(lane_status[lane / 2] >> (lane % 2 * 4));
    if ((status & (kLaneCrDone | kLaneEqDone | kLaneSymbolLocked)) != (kLaneCrDone | kLaneEqDone | kLaneSymbolLocked)) return false;
  }
  return true;
}

bool Dpcd::parse_capabilities(ConstByteView receiver, uint8_t edp_revision, ConstByteView edp_rates, Capabilities* out) {
  *out = Capabilities();
  if (receiver.size() < kReceiverBytes) return false;
  Capabilities c;
  c.revision = receiver[kRevision];
  c.max_link_rate_code = receiver[kMaxLinkRate];
  c.max_lanes = receiver[kMaxLaneCount] & 0x1F;
  c.tps3 = receiver[kMaxLaneCount] & 0x40;
  c.enhanced_framing = receiver[kMaxLaneCount] & 0x80;
  c.max_downspread = receiver[kMaxDownspread] & 0x01;
  c.no_aux_handshake = receiver[kMaxDownspread] & 0x40;
  c.training_read_interval = receiver[kTrainingAuxRdInterval] & 0x7F;
  c.receiver_ports = static_cast<uint8_t>((receiver[kNorp] & 0x01) + 1);
  c.downstream_port_present = receiver[kDownstreamPortPresent] & 0x01;
  c.downstream_type = receiver[kDownstreamPortPresent] >> 1 & 0x03;
  c.format_conversion = receiver[kDownstreamPortPresent] & 0x08;
  c.coding_8b10b = receiver[kMainLinkChannelCoding] & 0x01;
  c.downstream_port_count = receiver[kDownstreamPortCount] & 0x0F;
  c.downstream_oui = receiver[kDownstreamPortCount] & 0x80;
  // Something that is not a DisplayPort sink (a bus that reads zeros or ones) has no revision, lanes or rate that make sense
  if (c.revision < kMinRevision || (c.max_lanes != 1 && c.max_lanes != 2 && c.max_lanes != 4) || c.max_link_rate_code == 0) return false;
  c.edp_revision = edp_revision;
  if (edp_revision != kNoEdp && edp_revision >= kEdp14) {
    for (uint32_t i = 0; i < kMaxEdpRates && i * 2 + 1 < edp_rates.size(); ++i) {
      const uint32_t rate = uint32_t{edp_rates[i * 2]} | (uint32_t{edp_rates[i * 2 + 1]} << 8);
      if (rate == 0) break;
      c.edp_rates_khz[c.edp_rate_count++] = rate * kEdpRateUnitKhz;
    }
  }
  c.valid = true;
  *out = c;
  return true;
}

bool Dpcd::parse_link_status(ConstByteView status, LinkStatus* out) {
  *out = LinkStatus();
  if (status.size() < kStatusBytes) return false;
  out->sink_count_raw = status[0];
  out->irq_vector = status[1];
  out->lane_status[0] = status[2];
  out->lane_status[1] = status[3];
  out->align = status[4];
  out->sink_status = status[5];
  out->adjust[0] = status[6];
  out->adjust[1] = status[7];
  return true;
}

bool Dpcd::read_capabilities(IDpcdAux& aux, bool edp, Capabilities* out) {
  *out = Capabilities();
  uint8_t receiver[kReceiverBytes] = {};
  if (!aux.dpcd_read(kRevision, ByteView(receiver, sizeof receiver))) return false;
  uint8_t edp_revision = kNoEdp;
  uint8_t rates[kMaxEdpRates * 2] = {};
  if (edp) {
    uint8_t rev = 0;
    if (aux.dpcd_read(kEdpRevision, ByteView(&rev, 1))) {
      edp_revision = rev;
      if (rev >= kEdp14 && !aux.dpcd_read(kSupportedLinkRates, ByteView(rates, sizeof rates))) ByteView(rates, sizeof rates).fill(0);     // (a table that does not come is no table)
    }
  }
  return parse_capabilities(ConstByteView(receiver, sizeof receiver), edp_revision, ConstByteView(rates, sizeof rates), out);
}

bool Dpcd::read_link_status(IDpcdAux& aux, LinkStatus* out) {
  *out = LinkStatus();
  uint8_t status[kStatusBytes] = {};
  if (!aux.dpcd_read(kSinkCount, ByteView(status, sizeof status))) return false;
  return parse_link_status(ConstByteView(status, sizeof status), out);
}

bool Dpcd::read_lane_count(IDpcdAux& aux, uint32_t* lanes) {
  *lanes = 0;
  uint8_t value = 0;
  if (!aux.dpcd_read(kLaneCountSet, ByteView(&value, 1))) return false;
  *lanes = value & 0x1F;
  return true;
}

bool Dpcd::acknowledge_service_irq(IDpcdAux& aux, uint8_t vector, uint8_t* acked) {
  *acked = 0;
  const uint8_t bits = vector & kIrqHandled;
  if (bits == 0) return true;
  if (!aux.dpcd_write(kDeviceServiceIrqVector, ConstByteView(&bits, 1))) return false;
  *acked = bits;
  return true;
}

bool Dpcd::read_test_request(IDpcdAux& aux, uint8_t* request) {
  *request = 0;
  uint8_t value = 0;
  if (!aux.dpcd_read(kTestRequest, ByteView(&value, 1))) return false;
  *request = value;
  return true;
}

bool Dpcd::answer_test_request(IDpcdAux& aux, uint8_t request, ConstByteView edid, uint8_t* response) {
  *response = 0;
  uint8_t answer = kTestNak;
  if (request == kTestEdidRead && edid.size() >= kEdidBlockBytes && edid.size() % kEdidBlockBytes == 0) {
    const uint8_t checksum = edid[edid.size() - 1];
    if (!aux.dpcd_write(kTestEdidChecksum, ConstByteView(&checksum, 1))) return false;
    answer = kTestAck | kTestEdidChecksumWrite;
  }
  if (!aux.dpcd_write(kTestResponse, ConstByteView(&answer, 1))) return false;
  *response = answer;
  return true;
}

}  // namespace hardware
