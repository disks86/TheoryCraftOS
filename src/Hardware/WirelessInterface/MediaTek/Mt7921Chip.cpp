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

#include "Hardware/WirelessInterface/MediaTek/Mt7921Chip.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

using namespace mt7921;

namespace {

WifiChipResult from_status(Mt7921Status status) {
  switch (status) {
    case Mt7921Status::Ok: return WifiChipResult::Ok;
    case Mt7921Status::NoResponse: return WifiChipResult::NoResponse;
    case Mt7921Status::Timeout: return WifiChipResult::Timeout;
  }
  return WifiChipResult::Timeout;
}

// "<fw_ver>-<build date>" as Linux shows it in the wiphy firmware version.
void set_version(WifiChipIdentity& identity, const MtConnacRam& ram) {
  ArrayView<char> out(identity.firmware_version);
  out.fill('\0');
  uint64_t n = 0;
  auto append = [&](const char* text) {
    for (const char* c = text; *c && n + 1 < out.size(); ++c) out[n++] = *c;
  };
  append(ram.version());
  append("-");
  append(ram.build_date());
}

}  // namespace

Mt7921Chip::Mt7921Chip(const WifiAdapterModel& model, const WifiChipBus& bus, allocators::IAllocator& allocator)
    : model_(model), bus_(bus), allocator_(allocator), ram_name_(model.firmware), patch_file_(allocator), ram_file_(allocator) {}

WifiChipResult Mt7921Chip::fail(WifiChipResult result, const char* message) {
  message_ = message;
  release_rings();
  state_ = WifiChipState::Failed;
  return result;
}

void Mt7921Chip::release_rings() {
  fwdl_.release();
  wm_.release();
  event_.release();
}

void Mt7921Chip::stop() {
  if (bus_.registers && (state_ != WifiChipState::Off || fwdl_.valid() || event_.valid())) {
    Mt7921Registers regs(*bus_.registers);
    // stop the DMA engine before its rings go away, then hand the chip back to its firmware (which powers it down)
    (void)mt7921_dma_disable(regs, false);
    (void)mt7921_firmware_own(regs);
  }
  release_rings();
  state_ = WifiChipState::Off;
}

WifiChipResult Mt7921Chip::start(IWifiFirmwareSource& firmware) {
  stop();
  identity_ = WifiChipIdentity();
  patch_was_present_ = false;
  sequence_ = MtMcuSequence();
  if (!bus_.registers) return fail(WifiChipResult::NoRegisters, "no register window (BAR0)");
  Mt7921Registers regs(*bus_.registers);

  WifiChipResult result = identify(regs);
  if (result != WifiChipResult::Ok) return result;
  result = load_files(firmware);
  if (result != WifiChipResult::Ok) return result;
  result = init_dma(regs);
  if (result != WifiChipResult::Ok) return result;
  result = download_patch(regs);
  if (result != WifiChipResult::Ok) return result;
  result = download_ram(regs);
  if (result != WifiChipResult::Ok) return result;

  if (!regs.poll(kConnOnMisc, kTopMisc2FwN9Ready, kTopMisc2FwN9Ready, 1500, 1000)) return fail(WifiChipResult::FirmwareRejected, "the N9 firmware did not report that it runs");
  set_version(identity_, ram_);
  state_ = WifiChipState::FirmwareLoaded;
  message_ = "firmware running; the NIC capability query (MAC address) and later commands are not written yet";
  return WifiChipResult::Unsupported;
}

WifiChipResult Mt7921Chip::identify(Mt7921Registers& regs) {
  Mt7921Status status = mt7921_firmware_own(regs);
  if (status != Mt7921Status::Ok) return fail(from_status(status), status == Mt7921Status::NoResponse ? "the chip does not answer (registers read all ones)" : "the firmware did not take the chip (own handshake)");
  status = mt7921_driver_own(regs);
  if (status != Mt7921Status::Ok) return fail(from_status(status), "the driver could not take the chip (own handshake)");
  const uint32_t chip = regs.read(kHwChipId);
  const uint32_t revision = regs.read(kHwRev);
  if (chip == 0xFFFFFFFFu) return fail(WifiChipResult::NoResponse, "the chip id reads all ones");
  identity_.chip_id = chip & 0xFFFF;
  identity_.revision = revision & 0xFF;
  const char* patch = nullptr;
  const char* ram = nullptr;
  // this driver follows mt7921: MT7925 (0x7925) has its own register map and is not driven here
  if (identity_.chip_id == 0x7925 || !mt792x_firmware_names(identity_.chip_id, &patch, &ram)) return fail(WifiChipResult::UnknownRevision, "the chip id is not MT7921, MT7922 or MT7920");
  patch_name_ = patch;
  ram_name_ = ram;
  state_ = WifiChipState::Identified;
  status = mt7921_wfsys_reset(regs, kWfsysSwReset);
  if (status != Mt7921Status::Ok) return fail(from_status(status), "WFSYS reset did not finish");
  message_ = "identified";
  return WifiChipResult::Ok;
}

WifiChipResult Mt7921Chip::load_files(IWifiFirmwareSource& firmware) {
  const char* error = nullptr;
  if (!firmware.read(patch_name_, patch_file_)) return fail(WifiChipResult::FirmwareMissing, "the ROM patch file is missing");
  if (!patch_.parse(patch_file_.view(), &error)) return fail(WifiChipResult::FirmwareInvalid, error ? error : "the ROM patch file is not valid");
  if (!firmware.read(ram_name_, ram_file_)) return fail(WifiChipResult::FirmwareMissing, "the RAM code file is missing");
  if (!ram_.parse(ram_file_.view(), &error)) return fail(WifiChipResult::FirmwareInvalid, error ? error : "the RAM code file is not valid");
  return WifiChipResult::Ok;
}

WifiChipResult Mt7921Chip::init_dma(Mt7921Registers& regs) {
  if (mt7921_dma_disable(regs, true) != Mt7921Status::Ok) return fail(WifiChipResult::Timeout, "WFDMA did not stop");
  if (!wm_.create(allocator_, regs, kTxRingBase + kRingStride * kTxRingMcuWm, kTxMcuRingSize, kCommandBuffer, false) ||
      !fwdl_.create(allocator_, regs, kTxRingBase + kRingStride * kTxRingFwdl, kTxFwdlRingSize, kScatterChunk, false) ||
      !event_.create(allocator_, regs, kRxEventRingBase + kRingStride * kRxRingMcuWm, kRxMcuRingSize, kRxBufferBytes, true))
    return fail(WifiChipResult::NoResponse, "no DMA memory below 4 GiB for the rings");
  mt7921_dma_enable(regs);
  return WifiChipResult::Ok;
}

WifiChipResult Mt7921Chip::command(Mt7921Registers& regs, uint8_t cid, ConstByteView payload, bool wait, uint8_t* status) {
  const uint8_t seq = sequence_.next();
  uint8_t message[kCommandBuffer];
  const uint32_t length = mt_mcu_build_command(cid, 0, false, seq, payload, ByteView(message));
  if (length == 0) return fail(WifiChipResult::FirmwareInvalid, "MCU command does not fit");
  if (!wm_.send(regs, ConstByteView(message).first(length), kTxTries, kTxStepUs)) return fail(WifiChipResult::Timeout, "the chip did not fetch an MCU command");
  if (!wait) return WifiChipResult::Ok;
  for (uint32_t i = 0; i < kResponseTries; ++i) {
    const ConstByteView received = event_.peek_received();
    if (received.empty()) {
      delay_us(kResponseStepUs);
      continue;
    }
    MtMcuEvent event;
    const bool ours = mt_mcu_parse_event(received, &event) && event.seq == seq;
    uint8_t code = 0;
    const bool has_status = ours && mt_mcu_event_status(event, &code);
    event_.release_received(regs);          // the view is gone from here on
    if (!ours) continue;                    // an unsolicited event or an old answer: Linux drops those too
    if (status) {
      if (!has_status) return fail(WifiChipResult::FirmwareRejected, "MCU answer without a status");
      *status = code;
    }
    return WifiChipResult::Ok;
  }
  return fail(WifiChipResult::Timeout, "the MCU did not answer a command");
}

WifiChipResult Mt7921Chip::scatter(Mt7921Registers& regs, ConstByteView data) {
  // FW_SCATTER: the bytes as they are, no header, in chunks on the firmware download ring; no answer is waited for
  for (uint64_t offset = 0; offset < data.size(); offset += kScatterChunk) {
    const ConstByteView chunk = data.subview(offset).at_most(kScatterChunk);
    (void)sequence_.next();                 // mt76_connac2_mcu_fill_message takes a sequence number for FW_SCATTER too
    if (!fwdl_.send(regs, chunk, kTxTries, kTxStepUs)) return fail(WifiChipResult::Timeout, "the chip did not fetch firmware data");
  }
  return WifiChipResult::Ok;
}

WifiChipResult Mt7921Chip::download_patch(Mt7921Registers& regs) {
  uint8_t payload[16];
  uint8_t status = 0;
  WifiChipResult result = command(regs, mt_mcu::kPatchSemControl, ConstByteView(payload).first(mt_mcu_patch_sem_payload(true, ByteView(payload))), true, &status);
  if (result != WifiChipResult::Ok) return result;
  if (status == mt_mcu::kPatchIsDl) {
    patch_was_present_ = true;          // "Patch is already loaded": the semaphore is released all the same
  } else if (status != mt_mcu::kPatchNotDlSemSuccess) {
    return fail(WifiChipResult::FirmwareRejected, "the chip did not give the patch semaphore");
  } else {
    for (uint32_t i = 0; i < patch_.section_count(); ++i) {
      const MtPatchSection& section = patch_.section(i);
      const uint32_t n = mt_mcu_init_download_payload(section.address, section.length, patch_.download_mode(i), ByteView(payload));
      result = command(regs, mt_mcu_init_download_command(section.address, false), ConstByteView(payload).first(n), true, nullptr);
      if (result != WifiChipResult::Ok) return result;
      result = scatter(regs, section.data);
      if (result != WifiChipResult::Ok) return result;
    }
    result = command(regs, mt_mcu::kPatchFinishReq, ConstByteView(payload).first(mt_mcu_patch_finish_payload(ByteView(payload))), true, &status);
    if (result != WifiChipResult::Ok) return result;
    if (status != 0) return fail(WifiChipResult::FirmwareRejected, "the chip did not start the patch");
  }
  result = command(regs, mt_mcu::kPatchSemControl, ConstByteView(payload).first(mt_mcu_patch_sem_payload(false, ByteView(payload))), true, &status);
  if (result != WifiChipResult::Ok) return result;
  if (status != mt_mcu::kPatchRelSemSuccess) return fail(WifiChipResult::FirmwareRejected, "the chip did not release the patch semaphore");
  return WifiChipResult::Ok;
}

WifiChipResult Mt7921Chip::download_ram(Mt7921Registers& regs) {
  uint8_t payload[16];
  for (uint32_t i = 0; i < ram_.region_count(); ++i) {
    const MtRamRegion& region = ram_.region(i);
    if (!region.downloaded()) continue;
    const uint32_t n = mt_mcu_init_download_payload(region.address, region.length, ram_.download_mode(i), ByteView(payload));
    WifiChipResult result = command(regs, mt_mcu_init_download_command(region.address, false), ConstByteView(payload).first(n), true, nullptr);
    if (result != WifiChipResult::Ok) return result;
    result = scatter(regs, region.data);
    if (result != WifiChipResult::Ok) return result;
  }
  const uint32_t n = mt_mcu_fw_start_payload(ram_.start_option(), ram_.start_address(), ByteView(payload));
  return command(regs, mt_mcu::kFwStartReq, ConstByteView(payload).first(n), true, nullptr);
}

}  // namespace hardware
