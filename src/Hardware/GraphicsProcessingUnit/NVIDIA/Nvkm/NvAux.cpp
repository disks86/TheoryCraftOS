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

#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvAux.hpp"

// After nouveau nvkm/subdev/i2c aux.c, auxg94.c, auxgf119.c, auxgm200.c (MIT license) and the DRM DisplayPort helper (drm_dp_helper.c, MIT
// license), written from memory of them.

namespace hardware {
namespace nv {

namespace {

// ---- The registers, all in one place (as remembered from nouveau; (?) marks the least certain). Offsets from the channel's base. ----
constexpr uint32_t kG94Base = 0x00e4c0, kGm200Base = 0x00d930, kChannelStride = 0x50;
constexpr uint32_t kDataOut = 0x00, kDataIn = 0x10, kAddress = 0x20, kControl = 0x24, kStatus = 0x28;
constexpr uint32_t kAutoDpcd = 0x38;          // (?) gf119 0x00e4f8, gm200 0x00d968: bit 16, the hardware's own DPCD polling, off during a transfer
// control register
constexpr uint32_t kCtrlReset = 0x80000000, kCtrlGo = 0x00010000, kCtrlRequestMask = 0x0001f1ff, kCtrlAddressOnly = 0x00000100;
constexpr uint32_t kCtrlTypeShift = 12;
// the claim ("unksel" in nouveau, which does not know what it selects either): request in the 0x00x00000 field, the grant in 0x0x000000
constexpr uint32_t kG94Busy = 0x03010000, kG94ClaimField = 0x00300000, kG94Grant = 0x03000000, kG94Release = 0x00310000;
constexpr uint32_t kGm200Busy = 0x07010000, kGm200ClaimField = 0x00700000, kGm200Grant = 0x07000000, kGm200Release = 0x00710000;
constexpr uint32_t kClaimRequest = 0x00100000, kClaimGranted = 0x01000000;
// status register
constexpr uint32_t kStatNoSinkBit = 0x10000000;   // (?) G94: set when no sink; GM200: set when a sink is there (the sense is inverted)
constexpr uint32_t kStatReplyMask = 0x000f0000, kStatReplyShift = 16, kStatLengthMask = 0x0000001f;
constexpr uint32_t kStatTimeout = 0x00000100, kStatErrors = 0x00000e00;

uint32_t load_le32(ConstByteView bytes, uint32_t at) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t index = at + i;
    if (index < bytes.size()) v |= uint32_t{bytes[index]} << (8 * i);
  }
  return v;
}

}  // namespace

NvAuxChannel::NvAuxChannel(INvRegisters& regs, I2cVariant variant, uint8_t channel) : regs_(regs), variant_(variant), channel_(channel) {
  if (channel >= kMaxChannels) return;
  switch (variant) {
    case I2cVariant::G94: case I2cVariant::Gf119: base_ = kG94Base + uint32_t{channel} * kChannelStride; break;
    case I2cVariant::Gm200: base_ = kGm200Base + uint32_t{channel} * kChannelStride; break;
    default: break;   // (no AUX before G94)
  }
}

void NvAuxChannel::set_autodpcd(bool enable) {
  if (variant_ == I2cVariant::Gf119 || variant_ == I2cVariant::Gm200) regs_.mask(base_ + kAutoDpcd, 0x00010000, enable ? 0x00010000 : 0);
}

// g94_i2c_aux_init / gm200_i2c_aux_init
AuxStatus NvAuxChannel::claim() {
  const bool gm200 = variant_ == I2cVariant::Gm200;
  const uint32_t busy = gm200 ? kGm200Busy : kG94Busy, field = gm200 ? kGm200ClaimField : kG94ClaimField, grant = gm200 ? kGm200Grant : kG94Grant;
  bool idle = false;
  for (uint32_t i = 0; i < kIdlePolls && !idle; ++i) {
    idle = (regs_.read32(base_ + kControl) & busy) == 0;
    if (!idle) regs_.delay_us(1);
  }
  if (!idle) return AuxStatus::Busy;
  regs_.mask(base_ + kControl, field, kClaimRequest);
  for (uint32_t i = 0; i < kClaimPolls; ++i) {
    if ((regs_.read32(base_ + kControl) & grant) == kClaimGranted) return AuxStatus::Ok;
    regs_.delay_us(1);
  }
  release();
  return AuxStatus::Busy;
}

void NvAuxChannel::release() { regs_.mask(base_ + kControl, variant_ == I2cVariant::Gm200 ? kGm200Release : kG94Release, 0); }

AuxStatus NvAuxChannel::xfer(uint8_t request, uint32_t address, ByteView data, uint8_t* reply, uint32_t* done) {
  kernel::ScopedMutex guard(mutex_);
  return xfer_locked(request, address, data, reply, done);
}

AuxStatus NvAuxChannel::xfer_locked(uint8_t request, uint32_t address, ByteView data, uint8_t* reply, uint32_t* done) {
  *reply = 0;
  *done = 0;
  if (!valid() || data.size() > kMaxPayload || request > 0xf || address > 0xfffff) return AuxStatus::Invalid;
  const bool read = (request & 1) != 0;
  const uint32_t size = static_cast<uint32_t>(data.size());
  if (!set_pad_mode(regs_, variant_, channel_, PadMode::Aux)) return AuxStatus::Invalid;
  AuxStatus status = claim();
  if (status != AuxStatus::Ok) return status;

  const uint32_t stat0 = regs_.read32(base_ + kStatus);
  const bool sink = variant_ == I2cVariant::Gm200 ? (stat0 & kStatNoSinkBit) != 0 : (stat0 & kStatNoSinkBit) == 0;
  if (!sink) {
    release();
    return AuxStatus::NoSink;
  }
  set_autodpcd(false);
  if (!read) {
    for (uint32_t i = 0; i < kMaxPayload; i += 4) regs_.write32(base_ + kDataOut + i, load_le32(data, i));
  }
  uint32_t ctrl = regs_.read32(base_ + kControl);
  ctrl &= ~kCtrlRequestMask;
  ctrl |= uint32_t{request} << kCtrlTypeShift;
  ctrl |= size ? size - 1 : kCtrlAddressOnly;
  regs_.write32(base_ + kAddress, address);
  regs_.write32(base_ + kControl, kCtrlReset | ctrl);
  regs_.write32(base_ + kControl, ctrl);
  regs_.write32(base_ + kControl, kCtrlGo | ctrl);
  bool complete = false;
  for (uint32_t i = 0; i < kTransactionPolls && !complete; ++i) {
    complete = (regs_.read32(base_ + kControl) & kCtrlGo) == 0;
    if (!complete) regs_.delay_us(1);
  }
  uint32_t stat = 0;
  if (!complete) {
    status = AuxStatus::Timeout;
  } else {
    stat = regs_.mask(base_ + kStatus, 0, 0);   // (nouveau: read and written back, which clears what is write-one-to-clear)
    if (stat & kStatTimeout) status = AuxStatus::Timeout;
    if (stat & kStatErrors) status = AuxStatus::WireError;
  }
  if (status == AuxStatus::Ok) {
    *reply = static_cast<uint8_t>((stat & kStatReplyMask) >> kStatReplyShift);
    if (read) {
      const uint32_t got = stat & kStatLengthMask;
      if (got > size) {
        status = AuxStatus::Protocol;   // claims more than was asked for: nothing of it is believed
      } else {
        uint8_t buffer[kMaxPayload] = {};
        for (uint32_t i = 0; i < kMaxPayload; i += 4) {
          const uint32_t word = regs_.read32(base_ + kDataIn + i);
          for (uint32_t b = 0; b < 4; ++b) buffer[i + b] = static_cast<uint8_t>(word >> (8 * b));
        }
        data.first(got).copy_from(ConstByteView(buffer, got));
        *done = got;
      }
    } else {
      *done = size;
    }
  }
  set_autodpcd(true);
  release();
  return status;
}

// drm_dp_dpcd_access: one piece, tried until the sink acknowledges all of it
AuxStatus NvAuxChannel::native_locked(uint8_t request, uint32_t address, ByteView data, uint32_t* done) {
  AuxStatus first_error = AuxStatus::Ok;
  for (uint32_t attempt = 0; attempt < kNativeTries; ++attempt) {
    if (attempt) regs_.delay_us(kRetryDelayUs);
    uint8_t reply = 0;
    uint32_t got = 0;
    AuxStatus s = xfer_locked(request, address, data, &reply, &got);
    if (s == AuxStatus::Ok) {
      switch (reply & kNativeReplyMask) {
        case kNativeAck:
          if (got == data.size()) {
            *done = got;
            return AuxStatus::Ok;
          }
          s = AuxStatus::Protocol;   // a short piece: the whole piece again
          break;
        case kNativeNack: return AuxStatus::Nack;
        case kNativeDefer: s = AuxStatus::Defer; break;
        default: s = AuxStatus::Protocol; break;
      }
    }
    if (s == AuxStatus::NoSink || s == AuxStatus::Invalid) return s;
    if (first_error == AuxStatus::Ok) first_error = s;
  }
  return first_error;
}

bool NvAuxChannel::dpcd_read(uint32_t address, ByteView out) {
  kernel::ScopedMutex guard(mutex_);
  last_ = AuxStatus::Ok;
  for (uint32_t at = 0; at < out.size();) {
    const uint32_t left = static_cast<uint32_t>(out.size()) - at;
    const uint32_t chunk = left < kMaxPayload ? left : kMaxPayload;
    if (address > 0xfffff - at) { last_ = AuxStatus::Invalid; return false; }
    uint32_t done = 0;
    last_ = native_locked(kNativeRead, address + at, out.subview(at, chunk), &done);
    if (last_ != AuxStatus::Ok) return false;
    at += chunk;
  }
  return true;
}

bool NvAuxChannel::dpcd_write(uint32_t address, ConstByteView data) {
  kernel::ScopedMutex guard(mutex_);
  last_ = AuxStatus::Ok;
  for (uint32_t at = 0; at < data.size();) {
    const uint32_t left = static_cast<uint32_t>(data.size()) - at;
    const uint32_t chunk = left < kMaxPayload ? left : kMaxPayload;
    if (address > 0xfffff - at) { last_ = AuxStatus::Invalid; return false; }
    uint8_t payload[kMaxPayload] = {};
    ByteView piece(payload, chunk);
    piece.copy_from(data.subview(at, chunk));
    uint32_t done = 0;
    last_ = native_locked(kNativeWrite, address + at, piece, &done);
    if (last_ != AuxStatus::Ok) return false;
    at += chunk;
  }
  return true;
}

// drm_dp_i2c_do_msg: one I2C-over-AUX request with the retries; *done is the bytes the sink took or gave (may be fewer than asked for)
AuxStatus NvAuxChannel::i2c_msg_locked(uint8_t request, uint32_t address, ByteView data, uint32_t* done) {
  uint32_t defers = 0;
  AuxStatus last = AuxStatus::Defer;
  for (uint32_t attempt = 0; attempt < kI2cTries + defers; ++attempt) {
    uint8_t reply = 0;
    uint32_t got = 0;
    const AuxStatus s = xfer_locked(request, address, data, &reply, &got);
    if (s == AuxStatus::Busy) { last = s; continue; }
    if (s != AuxStatus::Ok) return s;
    switch (reply & kNativeReplyMask) {
      case kNativeAck: break;
      case kNativeNack: return AuxStatus::Nack;
      case kNativeDefer: regs_.delay_us(kRetryDelayUs); last = AuxStatus::Defer; continue;
      default: return AuxStatus::Protocol;
    }
    switch (reply & kI2cReplyMask) {
      case kI2cAck:
        *done = got;
        return AuxStatus::Ok;
      case kI2cNack:
        return AuxStatus::Nack;
      case kI2cDefer:
        if (defers < kI2cTries) ++defers;
        regs_.delay_us(kRetryDelayUs);
        // drm_dp_i2c_msg_write_status_update: a deferred write is asked about with a status update request
        if ((request & ~kI2cMot) == kI2cWrite) request = static_cast<uint8_t>((request & kI2cMot) | kI2cWriteStatusUpdate);
        last = AuxStatus::Defer;
        continue;
      default:
        return AuxStatus::Protocol;
    }
  }
  return last;
}

bool NvAuxChannel::transfer(ArrayView<Message> messages) {
  kernel::ScopedMutex guard(mutex_);
  if (messages.size() == 0 || !valid()) {
    last_ = AuxStatus::Invalid;
    return false;
  }
  for (const Message& m : messages) {
    if (m.address > 0x7f) {
      last_ = AuxStatus::Invalid;
      return false;
    }
  }
  // G94's channels cannot send an address-only request (nouveau drops them: no "address_only" in auxg94.c), so its transactions follow
  // nouveau's own adapter (nvkm_i2c_aux_i2c_xfer): MOT on every piece but the last of the last message. GF119 and later follow the DRM helper.
  const bool address_only = variant_ != I2cVariant::G94;
  AuxStatus status = AuxStatus::Ok;
  uint8_t request = 0;
  uint8_t address = 0;
  for (uint32_t i = 0; i < messages.size() && status == AuxStatus::Ok; ++i) {
    const Message& m = messages[i];
    const bool last_message = i + 1 == messages.size();
    address = m.address;
    const uint8_t base_request = m.read ? kI2cRead : kI2cWrite;
    request = static_cast<uint8_t>(base_request | kI2cMot);
    uint32_t done = 0;
    if (address_only) status = i2c_msg_locked(request, address, ByteView(), &done);
    const uint32_t size = static_cast<uint32_t>(m.data.size());
    for (uint32_t at = 0; at < size && status == AuxStatus::Ok;) {
      const uint32_t left = size - at;
      const uint32_t chunk = left < kMaxPayload ? left : kMaxPayload;
      uint8_t piece_request = request;
      if (!address_only && last_message && at + chunk == size) piece_request = base_request;
      uint8_t payload[kMaxPayload] = {};
      ByteView piece(payload, chunk);
      if (!m.read) piece.copy_from(m.data.subview(at, chunk));
      status = i2c_msg_locked(piece_request, address, piece, &done);
      if (status != AuxStatus::Ok) break;
      if (done == 0 || done > chunk) { status = AuxStatus::Protocol; break; }   // no progress (drm_dp_i2c_drain_msg: -EPROTO)
      if (m.read) m.data.subview(at, done).copy_from(piece.first(done));
      at += done;
    }
  }
  // the address-only request without MOT closes the transaction (also after a failure)
  if (address_only) {
    uint32_t done = 0;
    (void)i2c_msg_locked(static_cast<uint8_t>(request & ~kI2cMot), address, ByteView(), &done);
  }
  last_ = status;
  return status == AuxStatus::Ok;
}

}  // namespace nv
}  // namespace hardware
