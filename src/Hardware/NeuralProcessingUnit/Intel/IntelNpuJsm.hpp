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

// A JSM message, the command language between the host and the NPU firmware (vpu_jsm_api.h, API 3.x): 128 bytes, a 32-byte header (type, buffer status,
// request id, result) and a 96-byte payload whose layout depends on the type. Fields are little endian and read and written through the checked accessors; an
// offset is relative to the start of the payload.
struct IntelNpuJsmMessage {
  static constexpr uint32_t kSize = 128;
  static constexpr uint32_t kPayloadOffset = 32;
  static constexpr uint32_t kPayloadSize = 96;
  static constexpr uint32_t kTypeOffset = 8, kStatusOffset = 12, kRequestIdOffset = 16, kResultOffset = 20;
  static constexpr uint32_t kStatusFree = 0, kStatusAllocated = 1;

  uint8_t bytes[kSize] = {};

  ByteView view() { return ByteView(bytes, kSize); }
  ConstByteView view() const { return ConstByteView(bytes, kSize); }
  uint32_t header32(uint32_t offset) const;
  void set_header32(uint32_t offset, uint32_t value);
  uint32_t type() const { return header32(kTypeOffset); }
  uint32_t status() const { return header32(kStatusOffset); }
  uint32_t request_id() const { return header32(kRequestIdOffset); }
  uint32_t result() const { return header32(kResultOffset); }
  void set_type(uint32_t v) { set_header32(kTypeOffset, v); }
  void set_request_id(uint32_t v) { set_header32(kRequestIdOffset, v); }
  void set_result(uint32_t v) { set_header32(kResultOffset, v); }
  uint32_t get32(uint32_t payload_offset) const;
  uint64_t get64(uint32_t payload_offset) const;
  void put32(uint32_t payload_offset, uint32_t value);
  void put64(uint32_t payload_offset, uint64_t value);
};

// The messages the host sends and the replies it reads, as ivpu_jsm_msg.c builds them.
class IntelNpuJsm {
 public:
  // Channels (VPU_IPC_CHAN_*).
  static constexpr uint32_t kChannelAsyncCmd = 0, kChannelGenCmd = 10, kChannelJobReturn = 11;
  // Result codes of a reply (VPU_JSM_STATUS_*).
  static constexpr uint32_t kStatusSuccess = 0, kStatusProcessingError = 2, kStatusAborted = 4, kStatusResetRequiredMin = 0xE, kStatusResetRequiredMax = 0x1F;
  static constexpr uint32_t kEngineCompute = 0;
  // Message types (vpu_ipc_msg_type).
  static constexpr uint32_t kEngineReset = 0x1100, kEnginePreempt = 0x1101, kRegisterDb = 0x1102, kUnregisterDb = 0x1103, kQueryEngineHeartbeat = 0x1104,
                            kSsidRelease = 0x110E, kCreateCmdQueue = 0x1114, kDestroyCmdQueue = 0x1115, kSetContextSchedProperties = 0x1116,
                            kHwsRegisterDb = 0x1117, kHwsEngineResume = 0x111B, kFreqConfig = 0x111F, kPwrD0i3Enter = 0x1202;
  static constexpr uint32_t kJobDone = 0x2100;
  static constexpr uint32_t kEngineResetDone = 0x2200, kEnginePreemptDone = 0x2201, kRegisterDbDone = 0x2202, kUnregisterDbDone = 0x2203,
                            kQueryEngineHeartbeatDone = 0x2204, kSsidReleaseDone = 0x220E, kCreateCmdQueueRsp = 0x2215, kDestroyCmdQueueRsp = 0x2216,
                            kSetContextSchedPropertiesRsp = 0x2217, kHwsEngineResumeDone = 0x221C, kFreqConfigRsp = 0x221F, kPwrD0i3EnterDone = 0x2302;
  // Scheduling priority bands (vpu_job_scheduling_priority_band).
  static constexpr uint32_t kBandIdle = 0, kBandNormal = 1, kBandFocus = 2, kBandRealtime = 3, kBandCount = 4;

  // Requests. Each returns a message with the type and payload set; the IPC layer adds the request id.
  static IntelNpuJsmMessage register_db(uint32_t db_id, uint32_t ssid, uint64_t jobq_base, uint32_t jobq_size);           // OS scheduling
  static IntelNpuJsmMessage hws_register_db(uint32_t db_id, uint32_t ssid, uint64_t cmdq_id, uint64_t cmdq_base, uint64_t cmdq_size);  // hardware scheduling
  static IntelNpuJsmMessage unregister_db(uint32_t db_id);
  static IntelNpuJsmMessage query_heartbeat(uint32_t engine);
  static IntelNpuJsmMessage engine_reset(uint32_t engine);
  static IntelNpuJsmMessage engine_preempt(uint32_t engine, uint32_t preempt_id);
  static IntelNpuJsmMessage ssid_release(uint32_t ssid);
  static IntelNpuJsmMessage pwr_d0i3_enter();
  static IntelNpuJsmMessage hws_create_cmdq(uint32_t ssid, uint64_t cmdq_group, uint64_t cmdq_id, uint64_t process_id, uint32_t engine, uint64_t cmdq_base,
                                            uint32_t cmdq_size);
  static IntelNpuJsmMessage hws_destroy_cmdq(uint32_t ssid, uint64_t cmdq_id);
  static IntelNpuJsmMessage hws_set_context_sched_properties(uint32_t ssid, uint64_t cmdq_id, uint32_t priority_band);
  static IntelNpuJsmMessage hws_resume_engine(uint32_t engine);
  static IntelNpuJsmMessage freq_config(uint32_t min_ratio, uint32_t pn_ratio, uint32_t max_ratio);

  // The reply type a request is answered with, 0 for a type that is not a request this class knows.
  static uint32_t response_type(uint32_t request_type);
  // The channel a request goes out on.
  static uint32_t channel_for(uint32_t request_type);
  static const char* type_name(uint32_t type);

  // A job-return message (kJobDone), decoded.
  struct JobDone {
    uint32_t engine = 0, db_id = 0, job_id = 0, job_status = 0, ssid = 0;
    uint64_t cmdq_id = 0;
  };
  static JobDone parse_job_done(const IntelNpuJsmMessage& message);
  static uint64_t parse_heartbeat(const IntelNpuJsmMessage& message) { return message.get64(8); }
};

}  // namespace hardware
