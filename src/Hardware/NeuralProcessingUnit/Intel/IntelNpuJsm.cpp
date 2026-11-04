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


#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuJsm.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware {

uint32_t IntelNpuJsmMessage::header32(uint32_t offset) const {
  ByteReader r(view());
  r.seek(offset);
  return r.le32();
}
void IntelNpuJsmMessage::set_header32(uint32_t offset, uint32_t value) {
  ByteWriter w(view().subview(offset, 4));
  w.le32(value);
}
uint32_t IntelNpuJsmMessage::get32(uint32_t payload_offset) const {
  if (payload_offset > kPayloadSize - 4) return 0;
  return header32(kPayloadOffset + payload_offset);
}
uint64_t IntelNpuJsmMessage::get64(uint32_t payload_offset) const {
  if (payload_offset > kPayloadSize - 8) return 0;
  return get32(payload_offset) | (uint64_t{get32(payload_offset + 4)} << 32);
}
void IntelNpuJsmMessage::put32(uint32_t payload_offset, uint32_t value) {
  if (payload_offset <= kPayloadSize - 4) set_header32(kPayloadOffset + payload_offset, value);
}
void IntelNpuJsmMessage::put64(uint32_t payload_offset, uint64_t value) {
  if (payload_offset > kPayloadSize - 8) return;  // not even half of it
  put32(payload_offset, static_cast<uint32_t>(value));
  put32(payload_offset + 4, static_cast<uint32_t>(value >> 32));
}

namespace {
IntelNpuJsmMessage make(uint32_t type) {
  IntelNpuJsmMessage m;
  m.set_type(type);
  return m;
}
}  // namespace

// Payload offsets below are those of the vpu_ipc_msg_payload_* structs of the API header (checked with the header compiled with 4-byte packing).
IntelNpuJsmMessage IntelNpuJsm::register_db(uint32_t db_id, uint32_t ssid, uint64_t jobq_base, uint32_t jobq_size) {
  IntelNpuJsmMessage m = make(kRegisterDb);
  m.put32(0, db_id);
  m.put64(8, jobq_base);
  m.put32(16, jobq_size);
  m.put32(20, ssid);
  return m;
}
IntelNpuJsmMessage IntelNpuJsm::hws_register_db(uint32_t db_id, uint32_t ssid, uint64_t cmdq_id, uint64_t cmdq_base, uint64_t cmdq_size) {
  IntelNpuJsmMessage m = make(kHwsRegisterDb);
  m.put32(0, db_id);
  m.put32(4, ssid);
  m.put64(8, cmdq_id);
  m.put64(16, cmdq_base);
  m.put64(24, cmdq_size);
  return m;
}
IntelNpuJsmMessage IntelNpuJsm::unregister_db(uint32_t db_id) { IntelNpuJsmMessage m = make(kUnregisterDb); m.put32(0, db_id); return m; }
IntelNpuJsmMessage IntelNpuJsm::query_heartbeat(uint32_t engine) { IntelNpuJsmMessage m = make(kQueryEngineHeartbeat); m.put32(0, engine); return m; }
IntelNpuJsmMessage IntelNpuJsm::engine_reset(uint32_t engine) { IntelNpuJsmMessage m = make(kEngineReset); m.put32(0, engine); return m; }
IntelNpuJsmMessage IntelNpuJsm::engine_preempt(uint32_t engine, uint32_t preempt_id) {
  IntelNpuJsmMessage m = make(kEnginePreempt);
  m.put32(0, engine);
  m.put32(4, preempt_id);
  return m;
}
IntelNpuJsmMessage IntelNpuJsm::ssid_release(uint32_t ssid) { IntelNpuJsmMessage m = make(kSsidRelease); m.put32(0, ssid); return m; }
IntelNpuJsmMessage IntelNpuJsm::pwr_d0i3_enter() { IntelNpuJsmMessage m = make(kPwrD0i3Enter); m.put32(0, 1); return m; }  // send_response = 1
IntelNpuJsmMessage IntelNpuJsm::hws_create_cmdq(uint32_t ssid, uint64_t cmdq_group, uint64_t cmdq_id, uint64_t process_id, uint32_t engine, uint64_t cmdq_base,
                                                uint32_t cmdq_size) {
  IntelNpuJsmMessage m = make(kCreateCmdQueue);
  m.put64(0, process_id);
  m.put32(8, ssid);
  m.put32(12, engine);
  m.put64(16, cmdq_group);
  m.put64(24, cmdq_id);
  m.put64(32, cmdq_base);
  m.put32(40, cmdq_size);
  return m;
}
IntelNpuJsmMessage IntelNpuJsm::hws_destroy_cmdq(uint32_t ssid, uint64_t cmdq_id) {
  IntelNpuJsmMessage m = make(kDestroyCmdQueue);
  m.put32(0, ssid);
  m.put64(8, cmdq_id);
  return m;
}
IntelNpuJsmMessage IntelNpuJsm::hws_set_context_sched_properties(uint32_t ssid, uint64_t cmdq_id, uint32_t priority_band) {
  IntelNpuJsmMessage m = make(kSetContextSchedProperties);
  m.put32(0, ssid);
  m.put64(8, cmdq_id);
  m.put32(16, priority_band);
  m.put32(20, 0);   // realtime_priority_level
  m.put32(24, 0);   // in_process_priority
  m.put64(32, 20000);   // context_quantum
  m.put64(40, 10000);   // grace_period_same_priority
  m.put64(48, 0);       // grace_period_lower_priority
  return m;
}
IntelNpuJsmMessage IntelNpuJsm::hws_resume_engine(uint32_t engine) { IntelNpuJsmMessage m = make(kHwsEngineResume); m.put32(0, engine); return m; }
IntelNpuJsmMessage IntelNpuJsm::priority_band_setup(const PriorityBands& bands) {
  IntelNpuJsmMessage m = make(kSetPriorityBandSetup);
  for (uint32_t i = 0; i < kBandCount; ++i) {
    m.put32(4 * i, bands.grace_period[i]);
    m.put32(16 + 4 * i, bands.process_quantum[i]);
    m.put32(32 + 4 * i, bands.process_grace_period[i]);
  }
  m.put32(48, bands.normal_band_percentage);
  return m;
}
IntelNpuJsmMessage IntelNpuJsm::freq_config(uint32_t min_ratio, uint32_t pn_ratio, uint32_t max_ratio) {
  IntelNpuJsmMessage m = make(kFreqConfig);
  m.put32(0, min_ratio);
  m.put32(4, pn_ratio);
  m.put32(8, max_ratio);
  return m;
}

uint32_t IntelNpuJsm::response_type(uint32_t request_type) {
  switch (request_type) {
    case kEngineReset: return kEngineResetDone;
    case kEnginePreempt: return kEnginePreemptDone;
    case kRegisterDb:
    case kHwsRegisterDb: return kRegisterDbDone;   // the hardware-scheduler variant is answered with the same reply
    case kUnregisterDb: return kUnregisterDbDone;
    case kQueryEngineHeartbeat: return kQueryEngineHeartbeatDone;
    case kSsidRelease: return kSsidReleaseDone;
    case kCreateCmdQueue: return kCreateCmdQueueRsp;
    case kDestroyCmdQueue: return kDestroyCmdQueueRsp;
    case kSetPriorityBandSetup: return kSetPriorityBandSetupRsp;
    case kSetContextSchedProperties: return kSetContextSchedPropertiesRsp;
    case kHwsEngineResume: return kHwsEngineResumeDone;
    case kFreqConfig: return kFreqConfigRsp;
    case kPwrD0i3Enter: return kPwrD0i3EnterDone;
    default: return 0;
  }
}

uint32_t IntelNpuJsm::channel_for(uint32_t request_type) { return request_type == kPwrD0i3Enter ? kChannelGenCmd : kChannelAsyncCmd; }

const char* IntelNpuJsm::type_name(uint32_t type) {
  switch (type) {
    case kEngineReset: return "ENGINE_RESET";
    case kEnginePreempt: return "ENGINE_PREEMPT";
    case kRegisterDb: return "REGISTER_DB";
    case kUnregisterDb: return "UNREGISTER_DB";
    case kQueryEngineHeartbeat: return "QUERY_ENGINE_HB";
    case kSsidRelease: return "SSID_RELEASE";
    case kCreateCmdQueue: return "CREATE_CMD_QUEUE";
    case kDestroyCmdQueue: return "DESTROY_CMD_QUEUE";
    case kSetPriorityBandSetup: return "SET_PRIORITY_BAND_SETUP";
    case kSetContextSchedProperties: return "SET_CONTEXT_SCHED_PROPERTIES";
    case kHwsRegisterDb: return "HWS_REGISTER_DB";
    case kHwsEngineResume: return "HWS_ENGINE_RESUME";
    case kFreqConfig: return "FREQ_CONFIG";
    case kPwrD0i3Enter: return "PWR_D0I3_ENTER";
    case kJobDone: return "JOB_DONE";
    case kEngineResetDone: return "ENGINE_RESET_DONE";
    case kEnginePreemptDone: return "ENGINE_PREEMPT_DONE";
    case kRegisterDbDone: return "REGISTER_DB_DONE";
    case kUnregisterDbDone: return "UNREGISTER_DB_DONE";
    case kQueryEngineHeartbeatDone: return "QUERY_ENGINE_HB_DONE";
    case kSsidReleaseDone: return "SSID_RELEASE_DONE";
    case kCreateCmdQueueRsp: return "CREATE_CMD_QUEUE_RSP";
    case kDestroyCmdQueueRsp: return "DESTROY_CMD_QUEUE_RSP";
    case kSetContextSchedPropertiesRsp: return "SET_CONTEXT_SCHED_PROPERTIES_RSP";
    case kHwsEngineResumeDone: return "HWS_RESUME_ENGINE_DONE";
    case kFreqConfigRsp: return "FREQ_CONFIG_RSP";
    case kPwrD0i3EnterDone: return "PWR_D0I3_ENTER_DONE";
    default: return "UNKNOWN";
  }
}

IntelNpuJsm::JobDone IntelNpuJsm::parse_job_done(const IntelNpuJsmMessage& m) {
  JobDone j;
  j.engine = m.get32(0);
  j.db_id = m.get32(4);
  j.job_id = m.get32(8);
  j.job_status = m.get32(12);
  j.ssid = m.get32(16);
  j.cmdq_id = m.get64(24);
  return j;
}

}  // namespace hardware
