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

#include "Hardware/WirelessInterface/Qualcomm/Ath10kBmi.hpp"
#include "Kernel/Delay.hpp"
#include "Utility/AllocatedView.hpp"
#include "Utility/ByteStream.hpp"

namespace hardware::ath {

namespace {

constexpr uint32_t kBmiTimeoutUs = 3000000;     // BMI_COMMUNICATION_TIMEOUT_HZ (3 s)
constexpr uint32_t kBmiPollUs = 100;
constexpr uint32_t kAnswerBytes = 2048;         // host_ce_config_wlan[1].src_sz_max
constexpr uint32_t kBmiTxCe = 0, kBmiRxCe = 1;  // BMI_CE_NUM_TO_TARG, BMI_CE_NUM_TO_HOST

uint32_t finish(const ByteWriter& out) { return out.ok() ? static_cast<uint32_t>(out.position()) : 0; }

uint32_t encode_words(ByteView out, BmiCmd command, ArrayView<const uint32_t> words) {
  ByteWriter w(out);
  w.le32(static_cast<uint32_t>(command));
  for (const uint32_t word : words) w.le32(word);
  return finish(w);
}

}  // namespace

uint32_t bmi_encode_done(ByteView out) { return encode_words(out, BmiCmd::Done, {}); }
uint32_t bmi_encode_get_target_info(ByteView out) { return encode_words(out, BmiCmd::GetTargetInfo, {}); }

uint32_t bmi_encode_read_memory(ByteView out, uint32_t address, uint32_t length) {
  if (length == 0 || length > kBmiMaxDataSize) return 0;
  const uint32_t words[] = {address, length};
  return encode_words(out, BmiCmd::ReadMemory, words);
}

uint32_t bmi_encode_write_memory(ByteView out, uint32_t address, ConstByteView data) {
  if (data.empty() || data.size() > kBmiWriteChunk) return 0;
  const uint32_t padded = static_cast<uint32_t>((data.size() + 3) & ~uint64_t{3});
  ByteWriter w(out);
  w.le32(static_cast<uint32_t>(BmiCmd::WriteMemory));
  w.le32(address);
  w.le32(padded);
  w.bytes(data);
  w.zeros(padded - data.size());
  return finish(w);
}

uint32_t bmi_encode_execute(ByteView out, uint32_t address, uint32_t param) {
  const uint32_t words[] = {address, param};
  return encode_words(out, BmiCmd::Execute, words);
}

uint32_t bmi_encode_read_soc_register(ByteView out, uint32_t address) {
  const uint32_t words[] = {address};
  return encode_words(out, BmiCmd::ReadSocRegister, words);
}

uint32_t bmi_encode_write_soc_register(ByteView out, uint32_t address, uint32_t value) {
  const uint32_t words[] = {address, value};
  return encode_words(out, BmiCmd::WriteSocRegister, words);
}

uint32_t bmi_encode_lz_stream_start(ByteView out, uint32_t address) {
  const uint32_t words[] = {address};
  return encode_words(out, BmiCmd::LzStreamStart, words);
}

uint32_t bmi_encode_lz_data(ByteView out, ConstByteView data) {
  if (data.empty() || data.size() > kBmiLzChunk || (data.size() & 3)) return 0;
  ByteWriter w(out);
  w.le32(static_cast<uint32_t>(BmiCmd::LzData));
  w.le32(static_cast<uint32_t>(data.size()));
  w.bytes(data);
  return finish(w);
}

bool bmi_decode_target_info(ConstByteView answer, BmiTargetInfo* out) {
  if (answer.size() < 12) return false;
  ByteReader in(answer);
  (void)in.le32();          // the count of bytes that follow (the driver does not check it on PCIe)
  out->version = in.le32();
  out->type = in.le32();
  return in.ok();
}

bool bmi_decode_word(ConstByteView answer, uint32_t* out) {
  if (answer.size() < 4) return false;
  ByteReader in(answer);
  *out = in.le32();
  return in.ok();
}

BmiResult Ath10kBmi::send(uint32_t length, ByteView answer, uint32_t* received) {
  if (length == 0) return BmiResult::Invalid;
  uint32_t got = 0;
  ++messages_;
  if (!transport_.exchange(ConstByteView(command_).first(length), answer, &got)) return BmiResult::Transport;
  if (received) *received = got;
  return BmiResult::Ok;
}

BmiResult Ath10kBmi::get_target_info(BmiTargetInfo* out) {
  if (done_sent_) return BmiResult::NotAllowed;
  uint8_t answer[12];
  uint32_t got = 0;
  const BmiResult r = send(bmi_encode_get_target_info(ByteView(command_)), ByteView(answer), &got);
  if (r != BmiResult::Ok) return r;
  return bmi_decode_target_info(ConstByteView(answer).first(got < 12 ? got : 12), out) ? BmiResult::Ok : BmiResult::BadAnswer;
}

BmiResult Ath10kBmi::read_memory(uint32_t address, ByteView out) {
  if (done_sent_) return BmiResult::NotAllowed;
  uint64_t done = 0;
  while (done < out.size()) {
    const uint32_t chunk = static_cast<uint32_t>(out.size() - done < kBmiMaxDataSize ? out.size() - done : kBmiMaxDataSize);
    uint32_t got = 0;
    const BmiResult r = send(bmi_encode_read_memory(ByteView(command_), address + static_cast<uint32_t>(done), chunk), out.subview(done, chunk), &got);
    if (r != BmiResult::Ok) return r;
    if (got < chunk) return BmiResult::BadAnswer;
    done += chunk;
  }
  return BmiResult::Ok;
}

BmiResult Ath10kBmi::write_memory(uint32_t address, ConstByteView data) {
  if (done_sent_) return BmiResult::NotAllowed;
  if (data.size() > 0xFFFFFFFFull - address) return BmiResult::Invalid;
  uint64_t done = 0;
  while (done < data.size()) {
    const uint64_t left = data.size() - done;
    const uint32_t chunk = static_cast<uint32_t>(left < kBmiWriteChunk ? left : kBmiWriteChunk);
    const BmiResult r = send(bmi_encode_write_memory(ByteView(command_), address + static_cast<uint32_t>(done), data.subview(done, chunk)), ByteView(), nullptr);
    if (r != BmiResult::Ok) return r;
    done += chunk;
  }
  return BmiResult::Ok;
}

BmiResult Ath10kBmi::read32(uint32_t address, uint32_t* value) {
  uint8_t word[4];
  const BmiResult r = read_memory(address, ByteView(word));
  if (r != BmiResult::Ok) return r;
  return bmi_decode_word(ConstByteView(word), value) ? BmiResult::Ok : BmiResult::BadAnswer;
}

BmiResult Ath10kBmi::write32(uint32_t address, uint32_t value) {
  const uint8_t word[4] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  return write_memory(address, ConstByteView(word));
}

BmiResult Ath10kBmi::execute(uint32_t address, uint32_t param, uint32_t* result) {
  if (done_sent_) return BmiResult::NotAllowed;
  uint8_t answer[4];
  uint32_t got = 0;
  const BmiResult r = send(bmi_encode_execute(ByteView(command_), address, param), ByteView(answer), &got);
  if (r != BmiResult::Ok) return r;
  if (got < 4) return BmiResult::BadAnswer;
  return bmi_decode_word(ConstByteView(answer), result) ? BmiResult::Ok : BmiResult::BadAnswer;
}

BmiResult Ath10kBmi::read_soc_register(uint32_t address, uint32_t* value) {
  if (done_sent_) return BmiResult::NotAllowed;
  uint8_t answer[4];
  uint32_t got = 0;
  const BmiResult r = send(bmi_encode_read_soc_register(ByteView(command_), address), ByteView(answer), &got);
  if (r != BmiResult::Ok) return r;
  if (got < 4) return BmiResult::BadAnswer;
  return bmi_decode_word(ConstByteView(answer), value) ? BmiResult::Ok : BmiResult::BadAnswer;
}

BmiResult Ath10kBmi::write_soc_register(uint32_t address, uint32_t value) {
  if (done_sent_) return BmiResult::NotAllowed;
  return send(bmi_encode_write_soc_register(ByteView(command_), address, value), ByteView(), nullptr);
}

BmiResult Ath10kBmi::lz_stream_start(uint32_t address) {
  if (done_sent_) return BmiResult::NotAllowed;
  return send(bmi_encode_lz_stream_start(ByteView(command_), address), ByteView(), nullptr);
}

BmiResult Ath10kBmi::lz_data(ConstByteView data) {
  if (done_sent_) return BmiResult::NotAllowed;
  if (data.size() & 3) return BmiResult::Invalid;
  uint64_t done = 0;
  while (done < data.size()) {
    const uint64_t left = data.size() - done;
    const uint32_t chunk = static_cast<uint32_t>(left < kBmiLzChunk ? left : kBmiLzChunk);
    const BmiResult r = send(bmi_encode_lz_data(ByteView(command_), data.subview(done, chunk)), ByteView(), nullptr);
    if (r != BmiResult::Ok) return r;
    done += chunk;
  }
  return BmiResult::Ok;
}

BmiResult Ath10kBmi::fast_download(uint32_t address, ConstByteView data) {
  const uint64_t head = data.size() & ~uint64_t{3};
  BmiResult r = lz_stream_start(address);
  if (r != BmiResult::Ok) return r;
  if (head) {
    r = lz_data(data.first(head));
    if (r != BmiResult::Ok) return r;
  }
  if (head < data.size()) {
    uint8_t trailer[4] = {};
    ByteView(trailer).copy_from(data.subview(head));
    r = lz_data(ConstByteView(trailer));
    if (r != BmiResult::Ok) return r;
  }
  return lz_stream_start(0);
}

BmiResult Ath10kBmi::done() {
  if (done_sent_) return BmiResult::Ok;     // the driver skips a second DONE
  done_sent_ = true;
  return send(bmi_encode_done(ByteView(command_)), ByteView(), nullptr);
}

Ath10kPciBmi::Ath10kPciBmi(Ath10kPci& pci, allocators::IAllocator& allocator) : allocator_(allocator), tx_(pci, allocator), rx_(pci, allocator) {}

Ath10kPciBmi::~Ath10kPciBmi() { release(); }

void Ath10kPciBmi::release() {
  tx_.release();
  rx_.release();
  release_view(allocator_, request_);
  release_view(allocator_, response_);
}

bool Ath10kPciBmi::init() {
  release();
  request_ = allocate_dma_view<uint8_t>(allocator_, kBmiMaxCmdSize, 64);
  response_ = allocate_dma_view<uint8_t>(allocator_, kAnswerBytes, 64);
  uint32_t address = 0;
  if (!ath10k_dma_address32(request_, &address) || !ath10k_dma_address32(response_, &address)) {
    release();
    return false;
  }
  // host_ce_config_wlan[0] and [1] (the answer ring is smaller than the driver's 512 entries: BMI has one answer outstanding)
  if (!tx_.init(kBmiTxCe, 16, kBmiMaxDataSize, 0) || !rx_.init(kBmiRxCe, 0, kAnswerBytes, 16)) {
    release();
    return false;
  }
  return true;
}

bool Ath10kPciBmi::exchange(ConstByteView command, ByteView answer, uint32_t* received) {
  *received = 0;
  uint32_t request_address = 0, response_address = 0;
  if (command.empty() || command.size() > request_.size() || answer.size() > response_.size()) return false;
  if (!ath10k_dma_address32(request_, &request_address) || !ath10k_dma_address32(response_, &response_address)) return false;
  if (!request_.copy_from(command)) return false;
  const bool wants_answer = !answer.empty();
  if (wants_answer) {
    response_.fill(0);
    if (!rx_.post_receive(response_address)) return false;
  }
  if (!tx_.send(request_address, static_cast<uint32_t>(command.size()), 0xFFFFFFFFu)) return false;
  bool sent = false, answered = !wants_answer;
  uint32_t length = 0;
  for (uint32_t waited = 0; waited <= kBmiTimeoutUs; waited += kBmiPollUs) {
    bool gone = false;
    if (!sent && tx_.completed_send(&gone)) sent = true;
    if (gone) return false;
    if (!answered && rx_.completed_receive(&length)) answered = true;
    if (sent && answered) break;
    delay_us(kBmiPollUs);
  }
  if (!sent || !answered) return false;
  if (wants_answer) {
    const uint64_t take = length < answer.size() ? length : answer.size();
    if (!answer.copy_from(ConstByteView(response_).first(take))) return false;
    *received = static_cast<uint32_t>(take);
  }
  return true;
}

}  // namespace hardware::ath
