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

#include "Hardware/WirelessInterface/MediaTek/MtMcu.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

using namespace mt_mcu;

namespace {

// txd[0] and txd[1] of a command of `total` bytes, then six zero words.
void write_txd(ByteWriter& w, uint32_t total) {
  w.le32((total & kTxd0TxBytesMask) | (kTxTypeCmd << kTxd0PacketFormatShift) | (kTxMcuPortRxQ0 << kTxd0QueueShift));
  w.le32(kTxd1LongFormat | (kHeaderFormatCmd << kTxd1HeaderFormatShift));
  w.zeros(6 * 4);
}

bool fits(ConstByteView payload, uint32_t header, ByteView out) {
  return payload.size() <= kTxd0TxBytesMask - header && header + payload.size() <= out.size();
}

}  // namespace

uint32_t mt_mcu_build_command(uint8_t cid, uint8_t ext_cid, bool query, uint8_t seq, ConstByteView payload, ByteView out) {
  if (!fits(payload, kHeaderBytes, out)) return 0;
  const uint32_t total = kHeaderBytes + static_cast<uint32_t>(payload.size());
  ByteWriter w(out);
  write_txd(w, total);
  w.le16(static_cast<uint16_t>(total - kTxdBytes));                                  // len
  w.le16(static_cast<uint16_t>((kTxPortIndexMcu << 15) | (kTxMcuPortRxQ0 << 10)));  // pq_id = MCU_PQ_ID(MT_TX_PORT_IDX_MCU, MT_TX_MCU_PORT_RX_Q0)
  w.u8(cid);
  w.u8(kPacketId);
  // set_query: only extended (and CE) commands say set or query; the legacy download commands are "not applicable"
  w.u8(ext_cid ? (query ? kQueryQuery : kQuerySet) : kQueryNa);
  w.u8(seq);
  w.u8(0);                                                                          // uc_d2b0_rev
  w.u8(ext_cid);
  w.u8(kS2dH2n);
  w.u8(ext_cid ? 1 : 0);                                                            // ext_cid_ack
  w.zeros(5 * 4);
  w.bytes(payload);
  return w.ok() ? total : 0;
}

uint32_t mt_mcu_build_uni_command(uint16_t cid, uint8_t seq, ConstByteView payload, ByteView out) {
  if (!fits(payload, kUniHeaderBytes, out)) return 0;
  const uint32_t total = kUniHeaderBytes + static_cast<uint32_t>(payload.size());
  ByteWriter w(out);
  write_txd(w, total);
  w.le16(static_cast<uint16_t>(total - kTxdBytes));
  w.le16(cid);
  w.u8(0);                                                                          // rsv
  w.u8(kPacketId);
  w.u8(0);                                                                          // frag_n
  w.u8(seq);
  w.le16(0);                                                                        // checksum
  w.u8(kS2dH2n);
  w.u8(kUniExtAck);
  w.zeros(4);
  w.bytes(payload);
  return w.ok() ? total : 0;
}

uint8_t mt_mcu_init_download_command(uint32_t address, bool mt7925) {
  if (address == kPatchAddress || address == kConnac2PatchAddress || (mt7925 && address == kMt7925PatchAddress2)) return kPatchStartReq;
  return kTargetAddressLenReq;
}

uint32_t mt_mcu_patch_sem_payload(bool get, ByteView out) {
  ByteWriter w(out);
  w.le32(get ? kSemGet : kSemRelease);
  return w.ok() ? 4 : 0;
}

uint32_t mt_mcu_init_download_payload(uint32_t address, uint32_t length, uint32_t mode, ByteView out) {
  ByteWriter w(out);
  w.le32(address);
  w.le32(length);
  w.le32(mode);
  return w.ok() ? 12 : 0;
}

uint32_t mt_mcu_patch_finish_payload(ByteView out) {
  ByteWriter w(out);
  w.zeros(4);
  return w.ok() ? 4 : 0;
}

uint32_t mt_mcu_fw_start_payload(uint32_t option, uint32_t address, ByteView out) {
  ByteWriter w(out);
  w.le32(option);
  w.le32(address);
  return w.ok() ? 8 : 0;
}

bool mt_mcu_parse_event(ConstByteView received, MtMcuEvent* out) {
  *out = MtMcuEvent();
  ByteReader r(received);
  const uint32_t rxd0 = r.le32();
  r.skip(5 * 4);
  out->length = r.le16();
  out->packet_type_id = r.le16();
  out->eid = r.u8();
  out->seq = r.u8();
  out->option = r.u8();
  r.skip(1);
  out->ext_eid = r.u8();
  r.skip(2);
  out->s2d_index = r.u8();
  if (!r.ok()) return false;
  out->rx_bytes = rxd0 & kRxd0LengthMask;
  out->packet_type = static_cast<uint8_t>(rxd0 >> kRxd0PacketTypeShift);
  if (out->packet_type != kPacketTypeRxEvent) return false;
  if (out->rx_bytes < kEventHeaderBytes || out->rx_bytes > received.size()) return false;
  out->message = received.first(out->rx_bytes);
  out->data = out->message.subview(kEventHeaderBytes);
  return true;
}

bool mt_mcu_event_status(const MtMcuEvent& event, uint8_t* status) {
  return event.message.try_get(kEventHeaderBytes - 4, status);
}

}  // namespace hardware
