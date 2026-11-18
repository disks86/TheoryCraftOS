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

// BMI, the Bootloader Messaging Interface of the ath10k boot ROM, after ath10k/bmi.h and bmi.c: the host sends commands (a little-endian command id and its
// arguments) and the ROM answers some of them. With it the host learns the target version, reads and writes the target's memory (the host interest area, the board
// data), streams the OTP program and the firmware image into memory (LZ_STREAM_START, then LZ_DATA in chunks, which the ROM unpacks), runs the OTP program
// (EXECUTE) and finally lets the firmware start (DONE). Every message is at most BMI_MAX_DATA_SIZE (256) bytes, header included.
//
// On PCIe the messages travel over copy engine 0 (host to target) and the answers come back on copy engine 1 (ath10k_pci_hif_exchange_bmi_msg); Ath10kPciBmi is
// that transport, a test can put another one in.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Hardware/WirelessInterface/Qualcomm/Ath10kPci.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware::ath {

enum class BmiCmd : uint32_t {
  NoCommand = 0, Done = 1, ReadMemory = 2, WriteMemory = 3, Execute = 4, SetAppStart = 5, ReadSocRegister = 6, WriteSocRegister = 7, GetTargetInfo = 8,
  RompatchInstall = 9, RompatchUninstall = 10, RompatchActivate = 11, RompatchDeactivate = 12, LzStreamStart = 13, LzData = 14, NvramProcess = 15,
};

constexpr uint32_t kBmiMaxDataSize = 256;                         // BMI_MAX_DATA_SIZE
constexpr uint32_t kBmiMaxCmdSize = kBmiMaxDataSize + 12;         // BMI_MAX_CMDBUF_SIZE
constexpr uint32_t kBmiWriteChunk = kBmiMaxDataSize - 12;         // payload of one WRITE_MEMORY (244)
constexpr uint32_t kBmiLzChunk = kBmiMaxDataSize - 8;             // payload of one LZ_DATA (248)
// BMI_EXECUTE parameters and the answer of the OTP's board id query.
constexpr uint32_t kBmiParamGetEepromBoardId = 0x10, kBmiParamGetFlashBoardId = 0x8000, kBmiParamFlashSectionAll = 0x10000;
constexpr uint32_t kBmiBoardIdFromOtpMask = 0x7C00, kBmiBoardIdFromOtpShift = 10, kBmiChipIdFromOtpMask = 0x18000, kBmiChipIdFromOtpShift = 15,
                   kBmiBoardIdStatusMask = 0xFF, kBmiExtBoardIdSupport = 0x40000;

// Encoders: the message in `out`, its length as the return value (0 if `out` is too small or the arguments are not valid for the command).
uint32_t bmi_encode_done(ByteView out);
uint32_t bmi_encode_get_target_info(ByteView out);
uint32_t bmi_encode_read_memory(ByteView out, uint32_t address, uint32_t length);             // length <= 256
// One WRITE_MEMORY chunk (<= 244 bytes). The driver rounds the length up to 4 bytes; the padding is zeros here.
uint32_t bmi_encode_write_memory(ByteView out, uint32_t address, ConstByteView data);
uint32_t bmi_encode_execute(ByteView out, uint32_t address, uint32_t param);
uint32_t bmi_encode_read_soc_register(ByteView out, uint32_t address);
uint32_t bmi_encode_write_soc_register(ByteView out, uint32_t address, uint32_t value);
uint32_t bmi_encode_lz_stream_start(ByteView out, uint32_t address);
uint32_t bmi_encode_lz_data(ByteView out, ConstByteView data);                                // a multiple of 4 bytes, <= 248

struct BmiTargetInfo {
  uint32_t version = 0;     // ath10k_hw_params.id
  uint32_t type = 0;        // TARGET_TYPE_*
};
// The answer of GET_TARGET_INFO on PCIe: count, version, type (12 bytes).
[[nodiscard]] bool bmi_decode_target_info(ConstByteView answer, BmiTargetInfo* out);
// A one-word answer (EXECUTE's result, READ_SOC_REGISTER's value).
[[nodiscard]] bool bmi_decode_word(ConstByteView answer, uint32_t* out);

// A command, and when `answer` is not empty the target's answer into it (*received bytes). False if it was not delivered or the answer did not come.
class IAth10kBmiTransport {
 public:
  [[nodiscard]] virtual bool exchange(ConstByteView command, ByteView answer, uint32_t* received) = 0;

 protected:
  constexpr IAth10kBmiTransport() = default;
  ~IAth10kBmiTransport() = default;
};

enum class BmiResult : uint8_t { Ok, NotAllowed /* after DONE */, Transport, BadAnswer, Invalid };

class Ath10kBmi {
 public:
  explicit Ath10kBmi(IAth10kBmiTransport& transport) : transport_(transport) {}

  void start() { done_sent_ = false; }
  bool done_sent() const { return done_sent_; }

  [[nodiscard]] BmiResult get_target_info(BmiTargetInfo* out);
  [[nodiscard]] BmiResult read_memory(uint32_t address, ByteView out);
  [[nodiscard]] BmiResult write_memory(uint32_t address, ConstByteView data);
  [[nodiscard]] BmiResult read32(uint32_t address, uint32_t* value);
  [[nodiscard]] BmiResult write32(uint32_t address, uint32_t value);
  [[nodiscard]] BmiResult execute(uint32_t address, uint32_t param, uint32_t* result);
  [[nodiscard]] BmiResult read_soc_register(uint32_t address, uint32_t* value);
  [[nodiscard]] BmiResult write_soc_register(uint32_t address, uint32_t value);
  [[nodiscard]] BmiResult lz_stream_start(uint32_t address);
  [[nodiscard]] BmiResult lz_data(ConstByteView data);          // a multiple of 4 bytes, sent in 248-byte chunks
  // ath10k_bmi_fast_download: an LZ stream to `address` with the last partial word zero padded, closed by a stream to 0 (which flushes the target's caches).
  [[nodiscard]] BmiResult fast_download(uint32_t address, ConstByteView data);
  [[nodiscard]] BmiResult done();

  uint32_t messages_sent() const { return messages_; }

 private:
  [[nodiscard]] BmiResult send(uint32_t length, ByteView answer, uint32_t* received);

  IAth10kBmiTransport& transport_;
  uint8_t command_[kBmiMaxCmdSize] = {};
  bool done_sent_ = false;
  uint32_t messages_ = 0;
};

// The PCIe transport: copy engine 0 carries the command (16 entries, 256-byte maximum, host_ce_config_wlan[0]), copy engine 1 the answer (a posted 2 KiB buffer).
// The messages go through DMA buffers of its own; a message waits at most 3 s (BMI_COMMUNICATION_TIMEOUT_HZ).
class Ath10kPciBmi final : public IAth10kBmiTransport {
 public:
  Ath10kPciBmi(Ath10kPci& pci, allocators::IAllocator& allocator);
  ~Ath10kPciBmi();
  Ath10kPciBmi(const Ath10kPciBmi&) = delete;
  Ath10kPciBmi& operator=(const Ath10kPciBmi&) = delete;

  // Sets the two pipes up (after a reset). False if memory ran out.
  [[nodiscard]] bool init();
  void release();
  [[nodiscard]] bool exchange(ConstByteView command, ByteView answer, uint32_t* received) override;

  const Ath10kCePipe& tx_pipe() const { return tx_; }
  const Ath10kCePipe& rx_pipe() const { return rx_; }

 private:
  allocators::IAllocator& allocator_;
  Ath10kCePipe tx_, rx_;
  ByteView request_, response_;
};

}  // namespace hardware::ath
