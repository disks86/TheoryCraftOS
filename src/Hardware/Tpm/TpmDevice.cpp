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

#include "Hardware/Tpm/TpmDevice.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
constexpr uint16_t kStNoSessions = 0x8001;
constexpr uint32_t kCcStartup = 0x144, kCcGetCapability = 0x17A, kCcGetRandom = 0x17B, kCcPcrRead = 0x17E;
constexpr uint16_t kAlgSha256 = 0x000B;

bool put16(ByteBuffer& b, uint32_t v) { return b.put8(static_cast<uint8_t>(v >> 8)) && b.put8(static_cast<uint8_t>(v)); }
bool put32(ByteBuffer& b, uint32_t v) { return put16(b, v >> 16) && put16(b, v & 0xFFFF); }

}  // namespace

bool TpmDevice::run(uint32_t command_code, ConstByteView parameters, ConstByteView* body) {
  ByteBuffer command(allocator_);
  if (!put16(command, kStNoSessions) || !put32(command, 10 + static_cast<uint32_t>(parameters.size())) || !put32(command, command_code) || !command.append(parameters)) return false;
  if (!transport_.transmit(command.view(), &response_)) return false;
  ByteReader in(response_.view());
  in.be16();
  const uint32_t size = in.be32();
  response_code_ = in.be32();
  if (!in.ok() || size != response_.size()) return false;               // (a response that is not as long as it says)
  *body = in.rest();
  return true;
}

bool TpmDevice::startup() {
  ByteBuffer parameters(allocator_);
  put16(parameters, 0);                                                  // TPM_SU_CLEAR
  ConstByteView body;
  return run(kCcStartup, parameters.view(), &body) && (response_code_ == tpm::kRcSuccess || response_code_ == tpm::kRcInitialize);
}

bool TpmDevice::get_random(ByteView out) {
  uint64_t done = 0;
  for (uint32_t guard = 0; done < out.size() && guard < 4096; ++guard) {
    const uint32_t want = out.size() - done < 32 ? static_cast<uint32_t>(out.size() - done) : 32;
    ByteBuffer parameters(allocator_);
    put16(parameters, want);
    ConstByteView body;
    if (!run(kCcGetRandom, parameters.view(), &body) || response_code_ != tpm::kRcSuccess) return false;
    ByteReader in(body);
    const uint32_t got = in.be16();
    ConstByteView bytes = in.take(got);
    if (!in.ok() || got == 0 || got > want) return false;
    out.subview(done, got).copy_from(bytes);
    done += got;
  }
  return done == out.size();
}

bool TpmDevice::pcr_read(uint32_t index, uint8_t digest[32]) {
  if (index > 23) return false;
  ByteBuffer parameters(allocator_);
  put32(parameters, 1);                                                  // one bank selected
  put16(parameters, kAlgSha256);
  parameters.put8(3);                                                    // three bytes of selection
  for (uint32_t i = 0; i < 3; ++i) parameters.put8(static_cast<uint8_t>(index / 8 == i ? 1u << (index % 8) : 0));
  ConstByteView body;
  if (!run(kCcPcrRead, parameters.view(), &body) || response_code_ != tpm::kRcSuccess) return false;
  ByteReader in(body);
  in.be32();                                                             // the update counter
  const uint32_t banks = in.be32();
  for (uint32_t b = 0; b < banks && in.ok() && b < 16; ++b) {            // TPML_PCR_SELECTION out
    in.be16();
    const uint32_t select = in.u8();
    in.skip(select);
  }
  const uint32_t count = in.be32();
  if (!in.ok() || count != 1) return false;
  const uint32_t size = in.be16();
  ConstByteView value = in.take(size);
  if (!in.ok() || size != 32) return false;
  for (uint32_t i = 0; i < 32; ++i) digest[i] = value[i];
  return true;
}

bool TpmDevice::property(uint32_t property_id, uint32_t* value) {
  ByteBuffer parameters(allocator_);
  put32(parameters, 6);                                                  // TPM_CAP_TPM_PROPERTIES
  put32(parameters, property_id);
  put32(parameters, 1);
  ConstByteView body;
  if (!run(kCcGetCapability, parameters.view(), &body) || response_code_ != tpm::kRcSuccess) return false;
  ByteReader in(body);
  in.u8();                                                               // moreData
  if (in.be32() != 6) return false;
  const uint32_t count = in.be32();
  if (!in.ok() || count == 0) return false;
  const uint32_t id = in.be32(), v = in.be32();
  if (!in.ok() || id != property_id) return false;
  *value = v;
  return true;
}

bool tpm2_parse_table(ConstByteView table, Tpm2Table* out) {
  if (table.size() < 52 || table[0] != 'T' || table[1] != 'P' || table[2] != 'M' || table[3] != '2') return false;
  const uint32_t length = table[4] | (table[5] << 8) | (table[6] << 16) | (static_cast<uint32_t>(table[7]) << 24);
  if (length < 52 || length > table.size()) return false;
  out->platform_class = static_cast<uint16_t>(table[36] | (table[37] << 8));
  out->control_area = 0;
  for (int i = 7; i >= 0; --i) out->control_area = (out->control_area << 8) | table[40 + i];
  out->start_method = table[48] | (table[49] << 8) | (table[50] << 16) | (static_cast<uint32_t>(table[51]) << 24);
  return true;
}

}  // namespace hardware
