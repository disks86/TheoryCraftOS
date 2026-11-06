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

#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaMessages.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

namespace {
constexpr uint32_t kRevisionFirst = 1, kRevisionUnknown = 10;  // AIE2_DEV_REVISION_STXA .. AIE2_DEV_REVISION_UNKN
}

AmdXdnaError AmdXdnaMessages::exchange(AmdXdnaOpcode opcode, ConstByteView request, ByteView response) {
  last_status_ = 0;
  const AmdXdnaError e = mailbox_.call(static_cast<uint32_t>(opcode), request, response);
  if (e != AmdXdnaError::None) return e;
  ByteReader status(response);
  last_status_ = status.le32();  // every response starts with the status
  return last_status_ == 0 ? AmdXdnaError::None : AmdXdnaError::DeviceStatus;
}

AmdXdnaError AmdXdnaMessages::suspend() {
  uint8_t req[4] = {}, resp[4] = {};
  return exchange(AmdXdnaOpcode::Suspend, ConstByteView(req), ByteView(resp));
}

AmdXdnaError AmdXdnaMessages::resume() {
  uint8_t req[4] = {}, resp[4] = {};
  return exchange(AmdXdnaOpcode::Resume, ConstByteView(req), ByteView(resp));
}

AmdXdnaError AmdXdnaMessages::assign_mgmt_pasid(uint16_t pasid) {
  uint8_t req[4] = {}, resp[4] = {};
  ByteWriter w{ByteView(req)};
  w.le16(pasid);
  w.le16(0);
  return exchange(AmdXdnaOpcode::AssignMgmtPasid, ConstByteView(req), ByteView(resp));
}

AmdXdnaError AmdXdnaMessages::set_runtime_config(uint32_t type, uint64_t value) {
  uint8_t req[kSetRuntimeConfigRequest] = {}, resp[4] = {};
  ByteWriter w{ByteView(req)};
  w.le32(type);
  w.le64(value);
  return exchange(AmdXdnaOpcode::SetRuntimeConfig, ConstByteView(req), ByteView(resp));
}

AmdXdnaError AmdXdnaMessages::get_runtime_config(uint32_t type, uint64_t* value) {
  uint8_t req[4] = {}, resp[kGetRuntimeConfigResponse] = {};
  ByteWriter w{ByteView(req)};
  w.le32(type);
  const AmdXdnaError e = exchange(AmdXdnaOpcode::GetRuntimeConfig, ConstByteView(req), ByteView(resp));
  if (e != AmdXdnaError::None) return e;
  ByteReader r{ConstByteView(resp)};
  r.skip(4);
  *value = r.le64();
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaMessages::query_firmware_version(AmdXdnaFirmwareVersion* out) {
  uint8_t req[4] = {}, resp[kFirmwareVersionResponse] = {};
  const AmdXdnaError e = exchange(AmdXdnaOpcode::GetFirmwareVersion, ConstByteView(req), ByteView(resp));
  if (e != AmdXdnaError::None) return e;
  ByteReader r{ConstByteView(resp)};
  r.skip(4);
  out->major = r.le32();
  out->minor = r.le32();
  out->sub = r.le32();
  out->build = r.le32();
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaMessages::query_aie_version(uint16_t* major, uint16_t* minor) {
  uint8_t req[4] = {}, resp[kAieVersionResponse] = {};
  const AmdXdnaError e = exchange(AmdXdnaOpcode::QueryAieVersion, ConstByteView(req), ByteView(resp));
  if (e != AmdXdnaError::None) return e;
  ByteReader r{ConstByteView(resp)};
  r.skip(4);
  *major = r.le16();
  *minor = r.le16();
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaMessages::query_aie_tile_info(AmdXdnaTileInfo* out) {
  uint8_t req[4] = {}, resp[kTileInfoResponse] = {};
  const AmdXdnaError e = exchange(AmdXdnaOpcode::QueryAieTileInfo, ConstByteView(req), ByteView(resp));
  if (e != AmdXdnaError::None) return e;
  ByteReader r{ConstByteView(resp)};
  r.skip(4);
  AmdXdnaTileInfo t;
  t.col_size = r.le32();
  t.major = r.le16(); t.minor = r.le16(); t.cols = r.le16(); t.rows = r.le16();
  t.core_rows = r.le16(); t.mem_rows = r.le16(); t.shim_rows = r.le16();
  t.core_row_start = r.le16(); t.mem_row_start = r.le16(); t.shim_row_start = r.le16();
  t.core_dma_channels = r.le16(); t.mem_dma_channels = r.le16(); t.shim_dma_channels = r.le16();
  t.core_locks = r.le16(); t.mem_locks = r.le16(); t.shim_locks = r.le16();
  t.core_events = r.le16(); t.mem_events = r.le16(); t.shim_events = r.le16();
  // The shape the rest of the driver relies on: rows of each kind fit in the array, and there is at least one column.
  if (t.cols == 0 || t.rows == 0 || uint32_t{t.core_rows} + t.mem_rows + t.shim_rows > t.rows) return AmdXdnaError::BadResponse;
  *out = t;
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaMessages::query_protocol_version(uint32_t* major, uint32_t* minor) {
  uint8_t req[4] = {}, resp[kProtocolVersionResponse] = {};
  const AmdXdnaError e = exchange(AmdXdnaOpcode::GetProtocolVersion, ConstByteView(req), ByteView(resp));
  if (e != AmdXdnaError::None) return e;
  ByteReader r{ConstByteView(resp)};
  r.skip(4);
  *major = r.le32();
  *minor = r.le32();
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaMessages::get_dev_revision(uint32_t* revision, uint32_t* raw_fuse) {
  uint8_t req[4] = {}, resp[kDevRevisionResponse] = {};
  const AmdXdnaError e = exchange(AmdXdnaOpcode::GetDevRevision, ConstByteView(req), ByteView(resp));
  if (e != AmdXdnaError::None) return e;
  ByteReader r{ConstByteView(resp)};
  r.skip(4);
  const uint32_t rev = r.le32();
  const uint32_t fuse = r.le32();
  if (raw_fuse) *raw_fuse = fuse;
  if (rev < kRevisionFirst || rev >= kRevisionUnknown) return AmdXdnaError::BadResponse;
  *revision = rev;
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaMessages::update_time_quota(uint32_t microseconds) {
  uint8_t req[kUpdatePropertyRequest] = {}, resp[4] = {};
  ByteWriter w{ByteView(req)};
  w.le32(kUpdatePropertyTimeQuota);
  w.u8(static_cast<uint8_t>(kAllContexts));
  w.zeros(7);
  w.le32(microseconds);
  w.le32(0);
  return exchange(AmdXdnaOpcode::UpdateProperty, ConstByteView(req), ByteView(resp));
}

AmdXdnaError AmdXdnaMessages::create_context(const AmdXdnaContextRequest& request, AmdXdnaContext* out) {
  if (request.num_col == 0) return AmdXdnaError::BadArgument;
  uint8_t req[kCreateContextRequest] = {}, resp[kCreateContextResponse] = {};
  ByteWriter w{ByteView(req)};
  w.le32(1);  // aie_type
  w.u8(request.start_col);
  w.u8(request.num_col);
  w.u8(request.num_unused_col);
  w.u8(0);
  w.u8(1);    // command queue pairs requested
  w.u8(0);
  w.le16(request.pasid);
  w.zeros(8);
  w.le32(0);  // sec_comm_target_type
  w.le32(static_cast<uint32_t>(request.priority));
  const AmdXdnaError e = exchange(AmdXdnaOpcode::CreateContext, ConstByteView(req), ByteView(resp));
  if (e != AmdXdnaError::None) return e;
  ByteReader r{ConstByteView(resp)};
  r.skip(4);
  AmdXdnaContext c;
  c.id = r.le32();
  c.msix = r.le16();
  c.queue_pairs = r.u8();
  r.skip(1);
  for (uint32_t i = 0; i < AmdXdnaContext::kMaxQueuePairs; ++i) {
    AmdXdnaQueueInfo* pair[2] = {&c.x2i[i], &c.i2x[i]};
    for (AmdXdnaQueueInfo* q : pair) {
      q->head_addr = r.le32();
      q->tail_addr = r.le32();
      q->buf_addr = r.le32();
      q->buf_size = r.le32();
    }
  }
  // The driver rejects id -1 and uses the first pair; a context with no pair, or claiming more than fit in the answer, is malformed.
  if (c.id == 0xFFFFFFFFu || c.queue_pairs == 0 || c.queue_pairs > AmdXdnaContext::kMaxQueuePairs) {
    destroy_context(c.id);
    return AmdXdnaError::BadResponse;
  }
  *out = c;
  return AmdXdnaError::None;
}

AmdXdnaError AmdXdnaMessages::destroy_context(uint32_t context_id) {
  uint8_t req[4] = {}, resp[4] = {};
  ByteWriter w{ByteView(req)};
  w.le32(context_id);
  return exchange(AmdXdnaOpcode::DestroyContext, ConstByteView(req), ByteView(resp));
}

AmdXdnaError AmdXdnaMessages::map_host_buffer(uint32_t context_id, uint64_t addr, uint64_t size, uint64_t chunk, bool add) {
  if (chunk == 0 || size == 0 || size % chunk != 0 || addr > UINT64_MAX - size) return AmdXdnaError::BadArgument;
  AmdXdnaOpcode opcode = add ? AmdXdnaOpcode::AddHostBuffer : AmdXdnaOpcode::MapHostBuffer;
  for (uint64_t done = 0; done < size; done += chunk) {
    uint8_t req[kMapHostBufferRequest] = {}, resp[4] = {};
    ByteWriter w{ByteView(req)};
    w.le32(context_id);
    w.le64(addr + done);
    w.le64(chunk);
    const AmdXdnaError e = exchange(opcode, ConstByteView(req), ByteView(resp));
    if (e != AmdXdnaError::None) return e;
    opcode = AmdXdnaOpcode::AddHostBuffer;
  }
  return AmdXdnaError::None;
}

}  // namespace hardware
