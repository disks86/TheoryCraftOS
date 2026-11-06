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
#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaMailbox.hpp"

namespace hardware {

// Management message opcodes of AIE2 firmware (aie2_msg_priv.h: aie2_msg_opcode). Only the ones AmdXdnaMessages sends are listed with their purpose.
enum class AmdXdnaOpcode : uint32_t {
  CreateContext = 0x2,
  DestroyContext = 0x3,
  QueryAieTileInfo = 0xE,
  QueryAieVersion = 0xF,
  Suspend = 0x101,
  Resume = 0x102,
  AssignMgmtPasid = 0x103,
  MapHostBuffer = 0x106,
  GetFirmwareVersion = 0x108,
  SetRuntimeConfig = 0x10A,
  GetRuntimeConfig = 0x10B,
  UpdateProperty = 0x113,
  AddHostBuffer = 0x115,
  GetDevRevision = 0x117,
  GetProtocolVersion = 0x301,
};

struct AmdXdnaFirmwareVersion {
  uint32_t major = 0, minor = 0, sub = 0, build = 0;
};

// aie_tile_info: the AIE array's shape and resources per tile type.
struct AmdXdnaTileInfo {
  uint32_t col_size = 0;
  uint16_t major = 0, minor = 0, cols = 0, rows = 0;
  uint16_t core_rows = 0, mem_rows = 0, shim_rows = 0;
  uint16_t core_row_start = 0, mem_row_start = 0, shim_row_start = 0;
  uint16_t core_dma_channels = 0, mem_dma_channels = 0, shim_dma_channels = 0;
  uint16_t core_locks = 0, mem_locks = 0, shim_locks = 0;
  uint16_t core_events = 0, mem_events = 0, shim_events = 0;
};

// The context priorities (PRIORITY_*).
enum class AmdXdnaPriority : uint32_t { Realtime = 1, High = 2, Normal = 3, Low = 4 };

struct AmdXdnaContextRequest {
  uint8_t start_col = 0, num_col = 0, num_unused_col = 0;
  uint16_t pasid = 0;
  AmdXdnaPriority priority = AmdXdnaPriority::High;  // what the driver sends when the firmware cannot preempt
};

// A command queue pair the firmware set up for a context: device addresses of its head/tail registers and its ring (cq_info).
struct AmdXdnaQueueInfo {
  uint32_t head_addr = 0, tail_addr = 0, buf_addr = 0, buf_size = 0;
};
struct AmdXdnaContext {
  static constexpr uint32_t kMaxQueuePairs = 2;
  uint32_t id = 0;
  uint16_t msix = 0;
  uint8_t queue_pairs = 0;
  AmdXdnaQueueInfo x2i[kMaxQueuePairs], i2x[kMaxQueuePairs];
};

// Encoders and decoders for the management messages that bring an AIE2 device up and query it (aie2_message.c), over a running mailbox channel. Each
// request and response is laid out exactly as the packed structs of aie2_msg_priv.h, little endian; a response must be exactly its struct's size. A non-zero
// status from the firmware is DeviceStatus and kept in last_status().
class AmdXdnaMessages {
 public:
  // Wire sizes (bytes) of the packed structs.
  static constexpr uint32_t kSetRuntimeConfigRequest = 12, kGetRuntimeConfigResponse = 12, kAieVersionResponse = 8, kTileInfoResponse = 48;
  static constexpr uint32_t kFirmwareVersionResponse = 20, kCreateContextRequest = 28, kCreateContextResponse = 76, kMapHostBufferRequest = 20;
  static constexpr uint32_t kUpdatePropertyRequest = 20, kDevRevisionResponse = 12, kProtocolVersionResponse = 12;
  static constexpr uint32_t kUpdatePropertyTimeQuota = 0, kAllContexts = 0xFF;

  explicit AmdXdnaMessages(AmdXdnaMailbox& mailbox) : mailbox_(mailbox) {}

  AmdXdnaError suspend();
  AmdXdnaError resume();
  AmdXdnaError assign_mgmt_pasid(uint16_t pasid);
  AmdXdnaError set_runtime_config(uint32_t type, uint64_t value);
  AmdXdnaError get_runtime_config(uint32_t type, uint64_t* value);
  AmdXdnaError query_firmware_version(AmdXdnaFirmwareVersion* out);
  AmdXdnaError query_aie_version(uint16_t* major, uint16_t* minor);
  AmdXdnaError query_aie_tile_info(AmdXdnaTileInfo* out);
  AmdXdnaError query_protocol_version(uint32_t* major, uint32_t* minor);
  // The firmware's revision (1..9, see amd_xdna_revision_name) and the raw fuse word; BadResponse for a value outside the known range, as the driver.
  AmdXdnaError get_dev_revision(uint32_t* revision, uint32_t* raw_fuse);
  // Sets the execution time quantum of every context (UPDATE_PROPERTY, TIME_QUOTA).
  AmdXdnaError update_time_quota(uint32_t microseconds);
  AmdXdnaError create_context(const AmdXdnaContextRequest& request, AmdXdnaContext* out);
  AmdXdnaError destroy_context(uint32_t context_id);
  // aie2_send_host_buf_msgs(): maps [addr, addr + size) for the context in `chunk`-sized pieces, the first with MAP_HOST_BUFFER (or ADD_HOST_BUFFER when
  // `add`), the rest with ADD_HOST_BUFFER. `size` must be a non-zero multiple of `chunk`; the range must not wrap.
  AmdXdnaError map_host_buffer(uint32_t context_id, uint64_t addr, uint64_t size, uint64_t chunk, bool add);

  uint32_t last_status() const { return last_status_; }

 private:
  AmdXdnaError exchange(AmdXdnaOpcode opcode, ConstByteView request, ByteView response);

  AmdXdnaMailbox& mailbox_;
  uint32_t last_status_ = 0;
};

}  // namespace hardware
