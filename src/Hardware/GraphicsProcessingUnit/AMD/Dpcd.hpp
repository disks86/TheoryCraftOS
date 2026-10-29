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

#pragma once

#include <stdint.h>
#include "Utility/ArrayView.hpp"

namespace hardware {

// Where the DisplayPort configuration data (DPCD) of a sink is read from: its AUX channel.
class IDpcdAux {
 public:
  // `out.size()` bytes from `address`; false unless every byte came.
  virtual bool dpcd_read(uint32_t address, ByteView out) = 0;
  // `data` to `address` (a native write); false unless every byte was taken.
  virtual bool dpcd_write(uint32_t address, ConstByteView data) = 0;

 protected:
  IDpcdAux() = default;
  ~IDpcdAux() = default;
};

// The sink's DPCD (DisplayPort 1.4 section 2.9, and the DRM helper's DP_* register names in drm_dp.h / drm_dp_helper.c): what the receiver can do (revision, the fastest link rate and
// most lanes of the main link, the coding and downstream ports, an eDP panel's own rate table) and the state of the link (the sink count, the service interrupt vector, the training
// status of each lane, the lane alignment). Reading them is what a driver does before it trains the link and when the sink pulses its hot plug pin; nothing here writes.
//
// The register addresses and bit meanings are from memory (the DRM helper headers, drm_dp.h, were not at hand): nothing here is checked against them.
class Dpcd {
 public:
  // Addresses
  static constexpr uint32_t kRevision = 0x000, kMaxLinkRate = 0x001, kMaxLaneCount = 0x002, kMaxDownspread = 0x003, kNorp = 0x004, kDownstreamPortPresent = 0x005,
                            kMainLinkChannelCoding = 0x006, kDownstreamPortCount = 0x007;
  static constexpr uint32_t kSupportedLinkRates = 0x010;         // eDP 1.4: up to eight 16-bit rates
  static constexpr uint32_t kLaneCountSet = 0x101, kSinkCount = 0x200, kDeviceServiceIrqVector = 0x201, kLaneStatus = 0x202, kEdpRevision = 0x700;
  // Link training (DP 1.2 2.9.3.3 / drm_dp.h): the link settings, the training pattern, the per-lane voltage swing and pre-emphasis, the downspread and the sink's power
  static constexpr uint32_t kLinkBwSet = 0x100, kLinkRateSet = 0x115, kEdpConfigurationSet = 0x10A, kEdpConfigurationCap = 0x00D, kTrainingPatternSet = 0x102, kTrainingLane0Set = 0x103, kDownspreadCtrl = 0x107, kSetPower = 0x600, kTrainingAuxRdInterval = 0x00E;
  static constexpr uint8_t kLaneCountEnhancedFraming = 0x80, kSpreadAmp05 = 0x10, kPowerD0 = 1, kPowerD3 = 2;
  static constexpr uint8_t kTrainingPatternDisable = 0, kTrainingPattern1 = 1, kTrainingPattern2 = 2, kTrainingPattern3 = 3;
  // DP_TRAINING_LANEx_SET: voltage swing in bits 1:0 (levels 0..3), max swing reached bit 2, pre-emphasis in bits 4:3 (levels 0..3 as 0, 8, 16, 24), max pre-emphasis reached bit 5
  static constexpr uint8_t kTrainVoltageMask = 0x03, kTrainMaxSwingReached = 0x04, kTrainPreEmphasisMask = 0x18, kTrainPreEmphasisShift = 3, kTrainMaxPreEmphasisReached = 0x20;
  static constexpr uint8_t kVoltageMax = 3, kPreEmphasisMax = 3 << 3;
  static constexpr uint32_t kTestRequest = 0x218, kTestResponse = 0x260, kTestEdidChecksum = 0x261;
  // DP_DEVICE_SERVICE_IRQ_VECTOR: the bits a source handles here (the others are the MCCS and the MST message bits, left for whoever does those)
  static constexpr uint8_t kIrqAutomatedTest = 0x02, kIrqContentProtection = 0x04, kIrqSinkSpecific = 0x40, kIrqHandled = 0x46;
  // DP_TEST_REQUEST (one bit per kind of test; a request is one of them) and DP_TEST_RESPONSE
  static constexpr uint8_t kTestLinkTraining = 0x01, kTestVideoPattern = 0x02, kTestEdidRead = 0x04, kTestPhyPattern = 0x08;
  static constexpr uint8_t kTestAck = 0x01, kTestNak = 0x02, kTestEdidChecksumWrite = 0x04;
  static constexpr uint32_t kReceiverBytes = 0x10;               // 0x000..0x00F, the receiver capability field
  static constexpr uint32_t kStatusBytes = 8;                    // 0x200..0x207: sink count, service irq vector, lane status (2), alignment, sink status, adjust requests (2)
  static constexpr uint32_t kMaxEdpRates = 8;
  static constexpr uint8_t kEdp14 = 0x03;                        // DP_EDP_14: the DPCD revision of an eDP 1.4 panel (has the rate table)
  static constexpr uint8_t kNoEdp = 0xFF;

  struct Capabilities {
    bool valid = false;
    uint8_t revision = 0;                  // DP_DPCD_REV: 0x10 is 1.0, 0x11 1.1, 0x12 1.2, 0x13 1.3, 0x14 1.4
    uint8_t max_link_rate_code = 0;        // DP_MAX_LINK_RATE in units of 0.27 Gbit/s per lane: 0x06 (RBR 1.62), 0x0A (HBR 2.7), 0x14 (HBR2 5.4), 0x1E (HBR3 8.1)
    uint8_t max_lanes = 0;                 // 1, 2 or 4
    bool tps3 = false;                     // TPS3 training pattern (DP_TPS3_SUPPORTED)
    bool enhanced_framing = false;         // DP_ENHANCED_FRAME_CAP
    bool max_downspread = false;           // 0.5 % spread supported
    bool no_aux_handshake = false;         // DP_NO_AUX_HANDSHAKE_LINK_TRAINING: the sink needs no AUX handshake in training
    uint8_t training_read_interval = 0;    // DP_TRAINING_AUX_RD_INTERVAL (0x00E) bits 6:0: 0 for the default delays, else the wait between training steps in units of 4 ms
    uint8_t receiver_ports = 0;            // DP_NORP + 1: 1 or 2
    bool downstream_port_present = false;  // a branch device (a dongle or hub)
    uint8_t downstream_type = 0;           // 0 DisplayPort, 1 analog, 2 DVI/HDMI, 3 other
    bool format_conversion = false;
    uint8_t downstream_port_count = 0;
    bool downstream_oui = false;
    bool coding_8b10b = false;             // DP_MAIN_LINK_CHANNEL_CODING: 8b/10b
    uint8_t edp_revision = kNoEdp;         // DP_EDP_DPCD_REV when read for an eDP connector: 0 is 1.1, 1 is 1.2, 2 is 1.3, 3 is 1.4
    uint32_t edp_rate_count = 0;           // the eDP panel's own link rates (eDP 1.4), the ones listed before the first zero
    uint32_t edp_rates_khz[kMaxEdpRates] = {};   // each a link rate per lane in kHz (the register's 200 kHz units times 200): 162000, 270000, 540000, ...

    uint32_t max_link_rate_khz() const { return uint32_t{max_link_rate_code} * 27000; }          // the link clock the DRM helpers use: 162000, 270000, 540000, 810000
    uint32_t max_link_rate_kbps() const { return uint32_t{max_link_rate_code} * 270000; }    // per lane, raw bits per second / 1000 (0x0A: 2 700 000)
  };

  struct LinkStatus {
    uint8_t sink_count_raw = 0;            // DP_SINK_COUNT
    uint8_t irq_vector = 0;                // DP_DEVICE_SERVICE_IRQ_VECTOR
    uint8_t lane_status[2] = {};           // DP_LANE0_1_STATUS, DP_LANE2_3_STATUS: per lane a nibble (clock recovery done 1, channel equalization done 2, symbol locked 4)
    uint8_t align = 0;                     // DP_LANE_ALIGN_STATUS_UPDATED
    uint8_t sink_status = 0;               // DP_SINK_STATUS
    uint8_t adjust[2] = {};                // DP_ADJUST_REQUEST_LANE0_1, _LANE2_3

    // DP_SINK_COUNT: bits 5:0 and bit 7 (the high bit of the 7-bit count)
    uint32_t sink_count() const { return (static_cast<uint32_t>(sink_count_raw >> 7 & 1) << 6) | (sink_count_raw & 0x3F); }
    bool automated_test_request() const { return irq_vector & 0x02; }
    bool content_protection_irq() const { return irq_vector & 0x04; }
    bool sink_specific_irq() const { return irq_vector & 0x40; }
    bool link_status_updated() const { return align & 0x80; }
    bool downstream_port_status_changed() const { return align & 0x40; }
    bool interlane_aligned() const { return align & 0x01; }
    // drm_dp_clock_recovery_ok / drm_dp_channel_eq_ok for the first `lanes` lanes (1..4; more is 4, 0 is true)
    // drm_dp_get_adjust_request_voltage / _pre_emphasis: what the sink asks lane `lane` (0..3) to be set to, as a DP_TRAINING_LANEx_SET value (voltage swing in bits 1:0, pre-emphasis in 4:3)
    uint8_t adjust_voltage(uint32_t lane) const;
    uint8_t adjust_pre_emphasis(uint32_t lane) const;
    bool clock_recovery_ok(uint32_t lanes) const;
    bool channel_eq_ok(uint32_t lanes) const;
  };

  // Parses the receiver capability field (kReceiverBytes bytes from 0x000) and for an eDP panel the DPCD revision (0x700) and the rate table (up to kMaxEdpRates 16-bit entries from
  // 0x010, 2 * kMaxEdpRates bytes). False if the field is not a DPCD (a revision below 1.0, no lanes, no rate): `out` is then left invalid. `edp_revision` is kNoEdp for a DisplayPort sink.
  static bool parse_capabilities(ConstByteView receiver, uint8_t edp_revision, ConstByteView edp_rates, Capabilities* out);
  static bool parse_link_status(ConstByteView status, LinkStatus* out);

  // The reads. `edp`: also the eDP revision and, from 1.4, the rate table.
  static bool read_capabilities(IDpcdAux& aux, bool edp, Capabilities* out);
  static bool read_link_status(IDpcdAux& aux, LinkStatus* out);
  // Acknowledges the service interrupts a source handles (kIrqHandled) that are set in `vector`, the value read from DP_DEVICE_SERVICE_IRQ_VECTOR: the sink clears a bit when a 1 is
  // written to it. Nothing is written for a vector without such bits. `acked` gets the bits written.
  static bool acknowledge_service_irq(IDpcdAux& aux, uint8_t vector, uint8_t* acked);
  // The automated test request (DP_TEST_REQUEST) of a sink, and the answer to it (DP_TEST_RESPONSE). Of the tests only the EDID read can be done without a trained link: the checksum of the
  // last block of the EDID (`edid`, the whole EDID the source read, a multiple of 128 bytes; may be empty) goes to DP_TEST_EDID_CHECKSUM and the answer is ACK with the checksum-write bit.
  // Anything else - link training, video pattern, PHY test pattern, no request, several bits, an EDID that is not there - is answered NAK. False if a read or write failed (no answer given).
  static bool read_test_request(IDpcdAux& aux, uint8_t* request);
  static bool answer_test_request(IDpcdAux& aux, uint8_t request, ConstByteView edid, uint8_t* response);
  // DP_LANE_COUNT_SET: the lanes the source has set the link to (bits 4:0; 0 until the link has been trained), the number the link status is judged by.
  static bool read_lane_count(IDpcdAux& aux, uint32_t* lanes);
};

}  // namespace hardware
