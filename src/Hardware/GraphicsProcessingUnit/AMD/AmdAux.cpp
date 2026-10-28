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

#include "Hardware/GraphicsProcessingUnit/AMD/AmdAux.hpp"

// Follows amdgpu_atombios_dp.c of the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license) and drm_dp_helper.c (Copyright 2010 Red Hat, Inc.; MIT license), both written
// from memory of those files (they were not at hand): the table's parameter layout is from atombios.h (checked by scripts/check_aux_port.py), but the request framing, the limits of
// the table and the retry rules are not checked against the source.

namespace hardware {

namespace {
constexpr uint32_t kHeaderBytes = 4, kBareAddressBytes = 3;
constexpr uint32_t kRequestOffset = 4;       // scratch + 1 dword: the buffer's first byte; lpAuxRequest = 0 + 4
constexpr uint32_t kDataOutOffset = 20;      // 16 bytes on: lpDataOut = 16 + 4
}  // namespace

void AmdAux::scratch_put(uint32_t offset, ConstByteView bytes) {
  for (uint32_t i = 0; i < bytes.size(); ++i) {
    const uint32_t at = offset + i;
    const uint32_t shift = (at % 4) * 8;
    const uint32_t word = bios_.scratch_word(at / 4);
    bios_.set_scratch_word(at / 4, (word & ~(0xFFu << shift)) | (uint32_t{bytes[i]} << shift));
  }
}

void AmdAux::scratch_get(uint32_t offset, ByteView bytes) {
  for (uint32_t i = 0; i < bytes.size(); ++i) {
    const uint32_t at = offset + i;
    bytes[i] = static_cast<uint8_t>(bios_.scratch_word(at / 4) >> ((at % 4) * 8));
  }
}

// amdgpu_atombios_dp_process_aux_ch: the request (at most kHeaderBytes + kMaxWritePayload bytes: aux_transfer's buffer) into the scratch, the table run, the reply status, and the reply's
// payload (at most recv.size(), which is at most kMaxPayload) out of the scratch. A scratch memory that is too small for the buffers reads as zeros and ignores the writes (AtomBios).
int32_t AmdAux::process_aux_ch(ConstByteView send, ByteView recv, uint8_t* ack) {
  scratch_put(kRequestOffset, send);
  // PROCESS_AUX_CHANNEL_TRANSACTION_PARAMETERS_V2: lpAuxRequest, lpDataOut (words), ucChannelID, ucDelay / ucReplyStatus, ucDataOutLen, ucHPD_ID (bytes); the delay is 0
  uint32_t params[2] = {kRequestOffset | (kDataOutOffset << 16), uint32_t{channel_} | (uint32_t{hpd_} << 24)};
  if (bios_.execute_table(kCommandTable, ArrayView<uint32_t>(params, 2)) != AtomBios::Status::Ok) return kIo;
  *ack = static_cast<uint8_t>(params[1] >> 8);
  if (*ack == 1) return kTimedOut;     // the sink did not answer
  if (*ack == 2) return kBusy;         // "flags not zero"
  if (*ack == 3) return kIo;           // error
  uint32_t received = static_cast<uint8_t>(params[1] >> 16);
  if (received > recv.size()) received = static_cast<uint32_t>(recv.size());
  scratch_get(kDataOutOffset, recv.first(received));
  return static_cast<int32_t>(received);
}

// amdgpu_atombios_dp_aux_transfer
int32_t AmdAux::aux_transfer(Msg* msg) {
  const uint32_t size = static_cast<uint32_t>(msg->buffer.size());
  if (size > kMaxPayload) return kInvalid;
  uint8_t tx[kHeaderBytes + kMaxWritePayload] = {};
  tx[0] = static_cast<uint8_t>(msg->address);
  tx[1] = static_cast<uint8_t>(msg->address >> 8);
  tx[2] = static_cast<uint8_t>((msg->request << 4) | ((msg->address >> 16) & 0xF));
  tx[3] = size ? static_cast<uint8_t>(size - 1) : 0;
  int32_t result;
  uint8_t ack = 0;
  switch (msg->request & ~kI2cMot) {
    case kNativeWrite:
    case kI2cWrite:
    case kI2cWriteStatusUpdate: {
      // the table's request length is four bits, header included
      if (size > kMaxWritePayload) return kInvalid;
      const uint32_t tx_size = kHeaderBytes + size;
      tx[3] |= static_cast<uint8_t>((size == 0 ? kBareAddressBytes : tx_size) << 4);
      for (uint32_t i = 0; i < size; ++i) tx[kHeaderBytes + i] = msg->buffer[i];
      result = process_aux_ch(ConstByteView(tx, tx_size), ByteView(), &ack);
      if (result >= 0) result = static_cast<int32_t>(size);     // the payload size
      break;
    }
    case kNativeRead:
    case kI2cRead:
      tx[3] |= static_cast<uint8_t>((size == 0 ? kBareAddressBytes : kHeaderBytes) << 4);     // (a read without data is an address only request: the start of an I2C message)
      result = process_aux_ch(ConstByteView(tx, kHeaderBytes), msg->buffer, &ack);
      break;
    default:
      return kInvalid;
  }
  if (result >= 0) msg->reply = ack >> 4;
  return result;
}

// drm_dp_i2c_do_msg
int32_t AmdAux::do_msg(Msg* msg) {
  uint32_t defers = 0;
  for (uint32_t retry = 0; retry < kTries + defers; ++retry) {
    const int32_t result = aux_transfer(msg);
    if (result < 0) {
      if (result == kBusy) continue;
      return result;
    }
    switch (msg->reply & kNativeReplyMask) {
      case kNativeAck:
        break;
      case kNativeDefer:
        host_.delay_us(kRetryIntervalUs);
        continue;
      default:
        return kRemoteIo;
    }
    // The I2C reply only means something for the I2C requests
    if ((msg->request & ~kI2cMot) > kI2cWriteStatusUpdate) return result;
    switch (msg->reply & kI2cReplyMask) {
      case kI2cAck:
        return result;
      case kI2cDefer:
        if (defers < kTries) ++defers;
        host_.delay_us(kRetryIntervalUs);
        continue;
      default:
        return kRemoteIo;
    }
  }
  return kRemoteIo;
}

// drm_dp_i2c_drain_msg: a message of any size as requests of at most kMaxPayload bytes (for the table, at most kMaxWritePayload of a write); a reply that took fewer bytes goes on from there
int32_t AmdAux::drain_msg(Msg* msg) {
  const bool write = (msg->request & kI2cRead) == 0;      // (I2C write and write status update)
  const uint32_t limit = write ? kMaxWritePayload : kMaxPayload;
  ByteView rest = msg->buffer;
  while (rest.size() > 0) {
    Msg part = *msg;
    part.buffer = rest.first(rest.size() < limit ? rest.size() : limit);
    const int32_t done = do_msg(&part);
    if (done <= 0) return done == 0 ? kProtocol : done;
    rest = rest.subview(static_cast<uint32_t>(done));
  }
  return static_cast<int32_t>(msg->buffer.size());
}

bool AmdAux::dpcd_read(uint32_t address, ByteView out) {
  uint32_t done = 0;
  while (done < out.size()) {
    Msg msg;
    msg.request = kNativeRead;
    msg.address = address + done;
    const uint32_t chunk = out.size() - done < kMaxPayload ? static_cast<uint32_t>(out.size()) - done : kMaxPayload;
    msg.buffer = out.subview(done, chunk);
    const int32_t got = do_msg(&msg);
    if (got <= 0) return false;
    done += static_cast<uint32_t>(got);
  }
  return true;
}

// drm_dp_i2c_xfer
bool AmdAux::transfer(ArrayView<Message> messages) {
  if (messages.size() == 0) return false;
  Msg msg;
  int32_t err = 0;
  for (Message& m : messages) {
    msg.address = m.address;
    msg.request = (m.read ? kI2cRead : kI2cWrite) | kI2cMot;
    // A bare address request starts the transaction
    msg.buffer = ByteView();
    err = do_msg(&msg);
    if (err < 0) break;
    msg.buffer = m.data;
    err = drain_msg(&msg);
    if (err < 0) break;
  }
  // A bare address request without the flag closes it
  msg.request = static_cast<uint8_t>(msg.request & ~kI2cMot);
  msg.buffer = ByteView();
  do_msg(&msg);
  return err >= 0;
}

}  // namespace hardware
