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

// The registers of the AMD Sensor Fusion Hub (the MP2, a small processor in Ryzen mobile SoCs that runs the sensors) and the layout of the commands
// and of the sensor data, for both interface generations:
//
//   SFH 1.0 (PCI 1022:15E4: Renoir, Cezanne, Rembrandt): the host writes a command to the C2P ("CPU to MP2") mailbox registers - enable a sensor
//     with the bus address of a DMA buffer it allocated for that sensor, disable it, stop all. The MP2 says which sensors it has in P2C_MSG3 ("MP2
//     to CPU"); the low nibble of that register is the interface version, and version 2 firmware answers each command in P2C_MSG0 (v1 does not).
//     The firmware writes each enabled sensor's values (32-bit integers, 1000 times the HID report value) into its DMA buffer.
//   SFH 1.1 (PCI 1022:164A: Phoenix, Hawk Point, Strix): the firmware keeps a shared memory area (its physical address is in C2P_MSG22, in 2 MiB
//     units) that starts with metadata (firmware version, the list of sensors) and has one block per sensor with IEEE-754 single-precision values.
//     Commands use a different bit layout and are always answered in P2C_MSG0.
//
// Written from memory of the Linux amd_sfh driver (drivers/hid/amd-sfh-hid), not from AMD documentation, and not yet checked against real
// hardware. EVERY VALUE IN namespace amd_sfh_layout BELOW IS UNCERTAIN; the comments say how sure. Everything else in the driver uses only these
// names, so a correction is a change to this one table. The test's MP2 model decodes with the same table, so the host test checks the driver's
// logic, not these numbers.
#pragma once
#include <stdint.h>
#include "Hardware/Mmio.hpp"

namespace hardware {

// The MP2's register window (BAR 2). A test hands in a model.
class IAmdSfhRegisters {
 public:
  virtual uint32_t read32(uint32_t offset) = 0;
  virtual void write32(uint32_t offset, uint32_t value) = 0;
  // One 64-bit write (Linux uses writeq for the DMA address).
  virtual void write64(uint32_t offset, uint64_t value) = 0;

 protected:
  constexpr IAmdSfhRegisters() = default;
  ~IAmdSfhRegisters() = default;
};

class MmioAmdSfhRegisters final : public IAmdSfhRegisters {
 public:
  explicit MmioAmdSfhRegisters(const MmioRegion& region) : region_(region) {}
  uint32_t read32(uint32_t offset) override { return region_.read32(offset); }
  void write32(uint32_t offset, uint32_t value) override { region_.write32(offset, value); }
  void write64(uint32_t offset, uint64_t value) override { region_.write64(offset, value); }

 private:
  MmioRegion region_;
};

enum class AmdSfhGeneration : uint8_t {
  None,
  Sfh10V1,      // 15E4, firmware interface version 1: commands are not answered
  Sfh10V2,      // 15E4, firmware interface version 2: commands answered in P2C_MSG0
  Sfh11,        // 164A: shared memory with float values, commands answered in P2C_MSG0
};

// The sensors the MP2 knows (only those that map to an ISensor kind are used).
enum class AmdSfhSensorType : uint8_t { Accelerometer, Gyroscope, Magnetometer, AmbientLight, HumanPresence };

namespace amd_sfh_layout {

// ---- PCI -------------------------------------------------------------------------------------------------------------------------------------
constexpr uint16_t kVendorAmd = 0x1022;
constexpr uint16_t kDeviceSfh10 = 0x15E4;          // fairly sure (PCI_DEVICE_ID_AMD_MP2)
constexpr uint16_t kDeviceSfh11 = 0x164A;          // fairly sure (PCI_DEVICE_ID_AMD_MP2_1_1)
constexpr uint8_t kRegisterBar = 2;                // fairly sure (Linux maps BIT(2))
constexpr uint32_t kRegisterBytes = 0x20000;       // UNSURE: only what the registers below need; the BAR is believed to be 1 MiB

// ---- Mailbox registers -----------------------------------------------------------------------------------------------------------------------
constexpr uint32_t c2p(uint32_t n) { return 0x10500 + n * 4; }   // fairly sure (AMD_C2P_MSG)
constexpr uint32_t p2c(uint32_t n) { return 0x10680 + n * 4; }   // fairly sure (AMD_P2C_MSG)
// Some later 1.1 parts move the mailbox (Linux "rver" 1: C2P at 0x10900, P2C at 0x10500). UNSURE; not used, kept here so it is not forgotten.
constexpr uint32_t c2p_rver1(uint32_t n) { return 0x10900 + n * 4; }
constexpr uint32_t p2c_rver1(uint32_t n) { return 0x10500 + n * 4; }

constexpr uint32_t kCommandRegister = c2p(0);
constexpr uint32_t kResponseRegister = p2c(0);
constexpr uint32_t kV1ParameterRegister = c2p(1);      // 1.0 v1: buffer layout and length
constexpr uint32_t kV1AddressRegister = c2p(2);        // 1.0 v1: DMA address (64-bit write)
constexpr uint32_t kV2AddressRegister = c2p(1);        // 1.0 v2: DMA address (64-bit write)
constexpr uint32_t kActiveStatusRegister = p2c(3);     // 1.0: interface version (bits 3:0) and sensor mask (from bit 4). Fairly sure.
constexpr uint32_t kSharedMemoryRegister = c2p(22);    // 1.1: physical address of the shared memory >> 21. Fairly sure.
constexpr uint32_t kSharedMemoryShift = 21;
constexpr uint32_t kSharedMemoryBytes = 128 * 1024;    // fairly sure

// ---- Commands ----------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kCmdEnable = 1, kCmdDisable = 2, kCmdStopAll = 8;   // fairly sure (ENABLE_SENSOR, DISABLE_SENSOR, STOP_ALL_SENSORS)
constexpr uint32_t kStopAllSensorId = 0x1F;                            // UNSURE
constexpr uint32_t kPeriodMs = 200;                                    // the period asked for (Linux polls every 200 ms too)

// 1.0 v1 command (sfh_cmd_base.s): cmd_id 7:0, sensor_id 15:8, period 31:16. Fairly sure.
constexpr uint32_t v1_command(uint32_t cmd, uint32_t sensor, uint32_t period) { return (cmd & 0xFF) | ((sensor & 0xFF) << 8) | ((period & 0xFFFF) << 16); }
// 1.0 v1 parameter (sfh_cmd_param.s): buf_layout 1:0, buf_length 7:2 (in 32-bit words?). UNSURE what length counts.
constexpr uint32_t v1_parameter(uint32_t layout, uint32_t length) { return (layout & 3) | ((length & 0x3F) << 2); }

// 1.0 v2 command (cmd_v2): rsvd 9:0, cmd_id 13:10, intr_disable 14, rsvd 15, length 22:16, mem_type 23, sensor_id 31:24. UNSURE of every position.
constexpr uint32_t v2_command(uint32_t cmd, uint32_t sensor, uint32_t length, bool c2p_memory, bool interrupt_disable) {
  return ((cmd & 0xF) << 10) | (interrupt_disable ? 1u << 14 : 0) | ((length & 0x7F) << 16) | (c2p_memory ? 1u << 23 : 0) | ((sensor & 0xFF) << 24);
}
// 1.0 v2 response (response_v2): status 3:0, out_in_c2p 4, response 11:8, sub_cmd 19:12, sensor_id 25:20. UNSURE.
constexpr uint32_t v2_response_status(uint32_t r) { return r & 0xF; }
constexpr uint32_t v2_response_code(uint32_t r) { return (r >> 8) & 0xF; }
constexpr uint32_t v2_response_sensor(uint32_t r) { return (r >> 20) & 0x3F; }
constexpr uint32_t kV2SensorEnabled = 4, kV2SensorDisabled = 5;   // UNSURE (SENSOR_ENABLED / SENSOR_DISABLED)
constexpr uint32_t kV2InterfaceVersion = 2;                        // fairly sure (V2_STATUS in the low nibble of P2C_MSG3)
constexpr uint32_t kV2BufferLength = 16;                           // fairly sure (cmd_v2.length = 16)

// 1.1 command (sfh_cmd_base.cmd): sensor_id 7:0, cmd_id 11:8, sub_cmd_id 19:12, sub_cmd_value 30:20, intr_disable 31. UNSURE (Linux's field
// widths are remembered as 4/4/6/12/5/1, which cannot hold sensor id 19, so the widths here are a guess that can).
constexpr uint32_t v11_command(uint32_t cmd, uint32_t sensor, uint32_t sub_value, bool interrupt_disable) {
  return (sensor & 0xFF) | ((cmd & 0xF) << 8) | ((sub_value & 0x7FF) << 20) | (interrupt_disable ? 1u << 31 : 0);
}
// 1.1 response (sfh_cmd_response): response 7:0 (0 = done), sensor_id 15:8, cmd_id 19:16. UNSURE.
constexpr uint32_t v11_response_code(uint32_t r) { return r & 0xFF; }
constexpr uint32_t v11_response_sensor(uint32_t r) { return (r >> 8) & 0xFF; }
constexpr uint32_t v11_response_command(uint32_t r) { return (r >> 16) & 0xF; }

// Waiting for an answer: Linux polls every 500 us, for up to 1.6 s (1.0 v2) or 10 s (1.1). Here 1.6 s for both, and start-up gives up at the first
// command that is not answered, so a dead MP2 costs one timeout, not one per sensor.
constexpr uint32_t kResponsePollUs = 500, kResponseAttempts = 3200;
// 1.1: Linux sleeps 5 s before it reads the metadata ("time for the firmware to process the configuration"). Here: until the metadata is filled
// in, for at most 2 s. UNSURE whether filled-in metadata means ready.
constexpr uint32_t kFirmwarePollUs = 1000, kFirmwareAttempts = 2000;

// ---- Sensors -----------------------------------------------------------------------------------------------------------------------------------
struct SensorEntry {
  AmdSfhSensorType type;
  uint8_t id;              // the MP2's sensor index (in commands and, in 1.1, the data block number). Fairly sure: 0, 1, 2, 19, 5.
  uint32_t v10_mask;       // the bit in (P2C_MSG3 >> 4) on 1.0. Fairly sure: ACEL_EN BIT(0), GYRO_EN BIT(1), MAGNO_EN BIT(2), HPD_EN BIT(16), ALS_EN BIT(19)
  uint32_t v11_mask;       // the bit in the 1.1 sensor list. UNSURE: acc, gyro, mag, als, hpd as bits 0-4
  uint8_t axes;
  float v10_scale;         // 1.0: raw int32 * scale = the ISensor unit. UNSURE: Linux divides by 1000 and reports with the HID descriptor's unit exponent (-2 for motion)
  float v11_scale;         // 1.1: float * scale. UNSURE: Linux divides accel by 100 and gyro by 1000 before the same exponent; magnetometer and lux as they are
};
constexpr SensorEntry kSensors[] = {
    {AmdSfhSensorType::Accelerometer, 0, 1u << 0, 1u << 0, 3, 1e-5f, 1e-4f},     // g
    {AmdSfhSensorType::Gyroscope, 1, 1u << 1, 1u << 1, 3, 1e-5f, 1e-5f},         // degrees per second
    {AmdSfhSensorType::Magnetometer, 2, 1u << 2, 1u << 2, 3, 1e-5f, 1e-2f},      // gauss
    {AmdSfhSensorType::AmbientLight, 19, 1u << 19, 1u << 3, 1, 1e-3f, 1.0f},     // lux
    {AmdSfhSensorType::HumanPresence, 5, 1u << 16, 1u << 4, 1, 1.0f, 1.0f},      // non-zero = someone there (UNSURE of the encoding)
};
constexpr uint32_t kSensorCount = sizeof(kSensors) / sizeof(kSensors[0]);

// 1.0 DMA buffer per sensor: Linux allocates 8 ints; the values are words 0..2. Fairly sure.
constexpr uint32_t kV10BufferWords = 8;
// 1.0 v2: the ALS is enabled with mem_type "C2P register" (Linux does that for the ALS only). UNSURE what it changes; the driver still reads the
// DMA buffer.
constexpr bool kV2AlsUsesC2pMemory = true;

// 1.1 shared memory: metadata at 0 (word 0 firmware version, word 1 sensor list), sensor block n at n * 256 + 96 (sizeof sfh_base_info, 24 words);
// in a block: u64 timestamp, u32 interrupt count, u32 state, u32 reserved, then the values (float x, y, z, or float lux). UNSURE of the offsets.
constexpr uint32_t kV11FirmwareVersion = 0, kV11SensorList = 4;
constexpr uint32_t kV11BlockBytes = 256, kV11BlocksStart = 96;
constexpr uint32_t kV11InterruptCount = 8, kV11Values = 20;
constexpr uint32_t v11_block(uint8_t sensor_id) { return kV11BlocksStart + uint32_t{sensor_id} * kV11BlockBytes; }

}  // namespace amd_sfh_layout

// Plausibility limits for values from the firmware (untrusted): a sample outside is dropped. Wide enough for any real sensor.
namespace amd_sfh_limits {
constexpr float kMaxAccelerationG = 64.0f;
constexpr float kMaxAngularDps = 10000.0f;
constexpr float kMaxFluxGauss = 100.0f;
constexpr float kMaxLux = 200000.0f;          // full sunlight is about 120 000
}  // namespace amd_sfh_limits

}  // namespace hardware
