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

// Firmware names and NVRAM preparation after Linux brcmfmac firmware.c; see BrcmfFirmware.hpp.
#include "Hardware/WirelessInterface/Broadcom/BrcmfFirmware.hpp"

namespace hardware {

namespace {

// brcmf_pcie_fwnames, brcmf_sdio_fwnames, brcmf_usb_fwnames as the Linux 7.2 module has them (decoded from brcmfmac.ko: chip id, revision mask, base name), in table order:
// the first match wins, which matters where masks overlap (4365 rev 4+ is "4365c"; 43430 rev 0/1/2+ are a0/plain/b0).
constexpr BrcmfFirmwareMapping kPcieFirmware[] = {
    {0xaa52, 0xffffffff, "brcm/brcmfmac43602-pcie"},  {0xa9c9, 0xfffffff0, "brcm/brcmfmac4366c-pcie"},  {0x4350, 0x000000ff, "brcm/brcmfmac4350c2-pcie"},
    {0x4350, 0xffffff00, "brcm/brcmfmac4350-pcie"},   {0xaa05, 0xfffffff0, "brcm/brcmfmac4365c-pcie"},  {0x4355, 0x000007ff, "brcm/brcmfmac4355-pcie"},
    {0x4355, 0x00002000, "brcm/brcmfmac54591-pcie"},  {0x4355, 0xfffff800, "brcm/brcmfmac4355c1-pcie"}, {0x4356, 0xffffffff, "brcm/brcmfmac4356-pcie"},
    {0xaa2f, 0xffffffff, "brcm/brcmfmac43570-pcie"},  {0xaa31, 0xffffffff, "brcm/brcmfmac43570-pcie"},  {0xaa32, 0xffffffff, "brcm/brcmfmac43570-pcie"},
    {0x4358, 0xffffffff, "brcm/brcmfmac4358-pcie"},   {0x4359, 0x000001ff, "brcm/brcmfmac4359-pcie"},   {0x4359, 0xfffffe00, "brcm/brcmfmac4359c-pcie"},
    {0x4364, 0x0000000f, "brcm/brcmfmac4364b2-pcie"}, {0x4364, 0xfffffff0, "brcm/brcmfmac4364b3-pcie"}, {0x4365, 0x0000000f, "brcm/brcmfmac4365b-pcie"},
    {0x4365, 0xfffffff0, "brcm/brcmfmac4365c-pcie"},  {0x4366, 0x0000000f, "brcm/brcmfmac4366b-pcie"},  {0x4366, 0xfffffff0, "brcm/brcmfmac4366c-pcie"},
    {0xaa90, 0xfffffff0, "brcm/brcmfmac4366c-pcie"},  {0xaa92, 0xfffffff0, "brcm/brcmfmac4366c-pcie"},  {0x4371, 0xffffffff, "brcm/brcmfmac4371-pcie"},
    {0xaae8, 0xffffffff, "brcm/brcmfmac43752-pcie"},  {0x4377, 0xffffffff, "brcm/brcmfmac4377b3-pcie"}, {0x4378, 0x0000000f, "brcm/brcmfmac4378b1-pcie"},
    {0x4378, 0xffffffe0, "brcm/brcmfmac4378b3-pcie"}, {0x4387, 0xffffffff, "brcm/brcmfmac4387c2-pcie"},
};

constexpr BrcmfFirmwareMapping kSdioFirmware[] = {
    {0xa887, 0xffffffff, "brcm/brcmfmac43143-sdio"},   {0x4324, 0x0000001f, "brcm/brcmfmac43241b0-sdio"}, {0x4324, 0x00000020, "brcm/brcmfmac43241b4-sdio"},
    {0x4324, 0xffffffc0, "brcm/brcmfmac43241b5-sdio"}, {0x4329, 0xffffffff, "brcm/brcmfmac4329-sdio"},    {0x4330, 0xffffffff, "brcm/brcmfmac4330-sdio"},
    {0x4334, 0xffffffff, "brcm/brcmfmac4334-sdio"},    {0xa94c, 0xffffffff, "brcm/brcmfmac43340-sdio"},   {0xa94d, 0xffffffff, "brcm/brcmfmac43340-sdio"},
    {0x4335, 0xffffffff, "brcm/brcmfmac4335-sdio"},    {0xa962, 0xfffffffe, "brcm/brcmfmac43362-sdio"},   {0x4339, 0xffffffff, "brcm/brcmfmac4339-sdio"},
    {0xa9a6, 0x00000001, "brcm/brcmfmac43430a0-sdio"}, {0xa9a6, 0x00000002, "brcm/brcmfmac43430-sdio"},   {0xa9a6, 0xfffffffc, "brcm/brcmfmac43430b0-sdio"},
    {0x4345, 0x00000200, "brcm/brcmfmac43456-sdio"},   {0x4345, 0xfffffdc0, "brcm/brcmfmac43455-sdio"},   {0xa9be, 0x00000040, "brcm/brcmfmac43455-sdio"},
    {0x4354, 0xffffffff, "brcm/brcmfmac4354-sdio"},    {0x4356, 0xffffffff, "brcm/brcmfmac4356-sdio"},    {0x4359, 0xffffffff, "brcm/brcmfmac4359-sdio"},
    {0xaae7, 0xffffffff, "brcm/brcmfmac43752-sdio"},   {0xaae8, 0xffffffff, "brcm/brcmfmac43752-sdio"},   {0x4373, 0xffffffff, "brcm/brcmfmac4373-sdio"},
    {0xa804, 0xffffffff, "brcm/brcmfmac43012-sdio"},   {0xa9af, 0xffffffff, "brcm/brcmfmac43439-sdio"},
};

constexpr BrcmfFirmwareMapping kUsbFirmware[] = {
    {0xa887, 0xffffffff, "brcm/brcmfmac43143"},  {0xa8e3, 0x00000008, "brcm/brcmfmac43236b"}, {0xa8e4, 0x00000008, "brcm/brcmfmac43236b"},
    {0xa8e6, 0x00000008, "brcm/brcmfmac43236b"}, {0xa8ea, 0xffffffff, "brcm/brcmfmac43242a"}, {0xaa2e, 0xffffffff, "brcm/brcmfmac43569"},
    {0xaa31, 0xffffffff, "brcm/brcmfmac43569"},  {0x4373, 0xffffffff, "brcm/brcmfmac4373"},
};

bool append_hex_byte(BrcmfFileName& out, uint8_t value) {
  constexpr char kDigits[] = "0123456789abcdef";
  return out.push_back(kDigits[value >> 4]) && out.push_back(kDigits[value & 15]);
}

// The NVRAM text parser (struct nvram_parser and its state handlers). Positions index `data`; reads past the end act as the NUL a C string would end in.
class NvramParser {
 public:
  NvramParser(ConstByteView data, bool strip_mac, ByteBuffer& out) : data_(data), strip_mac_(strip_mac), out_(out) {}

  enum class State : uint8_t { Idle, Key, Value, Comment, End };

  [[nodiscard]] bool run() {
    State state = State::Idle;
    while (pos_ < data_.size()) {
      switch (state) {
        case State::Idle: state = idle(); break;
        case State::Key: state = key(state); break;
        case State::Value: state = value(); break;
        case State::Comment: state = comment(); break;
        case State::End: break;
      }
      if (state == State::End) break;
    }
    return !failed_;
  }

  bool multi_dev_v1 = false;
  bool multi_dev_v2 = false;
  bool boardrev_found = false;

 private:
  // is_nvram_char: printable, not the comment marker
  static bool is_nvram_char(uint8_t c) { return c != '#' && c >= 0x20 && c < 0x7f; }
  static bool is_whitespace(uint8_t c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }
  uint8_t at(uint64_t i) const { return i < data_.size() ? data_[i] : 0; }
  // strncmp(&data[at], prefix, strlen(prefix)) == 0
  bool prefix_at(uint64_t position, StringView prefix) const {
    for (uint64_t i = 0; i < prefix.size(); ++i)
      if (at(position + i) != static_cast<uint8_t>(prefix[i])) return false;
    return true;
  }

  State idle() {
    const uint8_t c = data_[pos_];
    if (c == '\n') return State::Comment;
    if (is_whitespace(c) || c == 0) {
      ++pos_;
      return State::Idle;
    }
    if (c == '#') return State::Comment;
    if (is_nvram_char(c)) {
      entry_ = pos_;
      return State::Key;
    }
    ++pos_;    // an invalid character is ignored
    return State::Idle;
  }

  State key(State state) {
    const uint8_t c = data_[pos_];
    if (c == '=') {
      // RAW1 entries are treated as comments
      state = prefix_at(entry_, "RAW1") ? State::Comment : State::Value;
      if (prefix_at(entry_, "devpath")) multi_dev_v1 = true;
      if (prefix_at(entry_, "pcie/")) multi_dev_v2 = true;
      if (prefix_at(entry_, "boardrev")) boardrev_found = true;
      if (strip_mac_ && prefix_at(entry_, "macaddr")) state = State::Comment;
    } else if (!is_nvram_char(c) || c == ' ') {
      return State::Comment;    // '=' expected: the entry is skipped
    }
    ++pos_;
    return state;
  }

  State value() {
    const uint8_t c = data_[pos_];
    if (!is_nvram_char(c)) {
      // the key=value pair is complete (pos_ is not advanced: the character is looked at again in Idle)
      const uint64_t length = pos_ - entry_;
      if (out_.size() + length + 1 >= brcmf_nvram::kMaxBytes) return State::End;
      if (!out_.append(data_.subview(entry_, length)) || !out_.put8(0)) {
        failed_ = true;
        return State::End;
      }
      return State::Idle;
    }
    ++pos_;
    return State::Value;
  }

  State comment() {
    // strchr(sol, '\n'), and if that is NULL strchr(sol, '\0'): the line ends at the first newline or NUL
    uint64_t i = pos_;
    while (i < data_.size() && data_[i] != '\n' && data_[i] != 0) ++i;
    pos_ = i < data_.size() ? i + 1 : data_.size();
    return State::Idle;
  }

  ConstByteView data_;
  bool strip_mac_;
  ByteBuffer& out_;
  uint64_t pos_ = 0;
  uint64_t entry_ = 0;
  bool failed_ = false;
};

// Reads of a stripped image past its end are zeros (the kernel's buffers are zero-filled past the data).
uint8_t byte_at(ConstByteView image, uint64_t i) { return i < image.size() ? image[i] : 0; }
bool image_prefix_at(ConstByteView image, uint64_t position, StringView prefix) {
  for (uint64_t i = 0; i < prefix.size(); ++i)
    if (byte_at(image, position + i) != static_cast<uint8_t>(prefix[i])) return false;
  return true;
}
// Index just past the zero ending the entry at `i` (while (nvram[i] != 0) i++; i++;).
uint64_t skip_entry(ConstByteView image, uint64_t i) {
  while (i < image.size() && image[i] != 0) ++i;
  return i + 1;
}
// Copies the entry from `i` up to its zero, then a zero.
bool copy_entry(ConstByteView image, uint64_t i, ByteBuffer& out) {
  const uint64_t start = i;
  while (i < image.size() && image[i] != 0) ++i;
  return out.append(image.subview(start, i - start)) && out.put8(0);
}

// brcmf_fw_strip_multi_v1: "devpath<n>=pci[e]/<domain>/<bus>" names the device's index n; its entries are the ones written "<n>:key=value".
[[nodiscard]] bool strip_multi_v1(ConstByteView image, uint16_t domain_nr, uint16_t bus_nr, bool* boardrev_found, ByteBuffer& out) {
  out.clear();
  if (image.size() < brcmf_nvram::kDevpathLength + 6) return true;    // too short: nothing for this device
  BrcmfFileName pci_path, pcie_path;
  if (!pci_path.assign("=pci/") || !pci_path.append_uint(domain_nr) || !pci_path.push_back('/') || !pci_path.append_uint(bus_nr)) return false;
  if (!pcie_path.assign("=pcie/") || !pcie_path.append_uint(domain_nr) || !pcie_path.push_back('/') || !pcie_path.append_uint(bus_nr)) return false;
  bool found = false;
  uint8_t id = 0;
  uint64_t i = 0;
  while (i < image.size() - brcmf_nvram::kDevpathLength) {
    if (image_prefix_at(image, i, "devpath") && (image_prefix_at(image, i + 8, pci_path) || image_prefix_at(image, i + 8, pcie_path))) {
      id = static_cast<uint8_t>(byte_at(image, i + 7) - '0');
      found = true;
      break;
    }
    i = skip_entry(image, i);
  }
  if (!found) return true;
  i = 0;
  while (i < image.size()) {
    if (static_cast<int>(image[i]) - '0' == static_cast<int>(id) && byte_at(image, i + 1) == ':') {
      i += 2;
      if (image_prefix_at(image, i, "boardrev")) *boardrev_found = true;
      if (!copy_entry(image, i, out)) return false;
    }
    i = skip_entry(image, i);
  }
  return true;
}

// brcmf_fw_strip_multi_v2: the device's entries are the ones prefixed "pcie/<domain>/<bus>/".
[[nodiscard]] bool strip_multi_v2(ConstByteView image, uint16_t domain_nr, uint16_t bus_nr, bool* boardrev_found, ByteBuffer& out) {
  out.clear();
  BrcmfFileName prefix;
  if (!prefix.assign("pcie/") || !prefix.append_uint(domain_nr) || !prefix.push_back('/') || !prefix.append_uint(bus_nr) || !prefix.push_back('/')) return false;
  const uint64_t length = prefix.size();
  // (Linux computes nvram_len - len unsigned; an image shorter than the prefix holds no entry for the device)
  if (image.size() <= length) return true;
  uint64_t i = 0;
  while (i < image.size() - length) {
    if (image_prefix_at(image, i, prefix)) {
      i += length;
      if (image_prefix_at(image, i, "boardrev")) *boardrev_found = true;
      if (!copy_entry(image, i, out)) return false;
    }
    i = skip_entry(image, i);
  }
  return true;
}

bool copy_token(StringView text, uint64_t start, ArrayView<char> out) {
  if (out.empty()) return false;
  uint64_t end = start;
  while (end < text.size() && text[end] != ' ' && text[end] != '\n' && text[end] != '\r' && text[end] != 0 && text[end] != '(') ++end;
  if (end == start) return false;
  uint64_t n = end - start;
  if (n > out.size() - 1) n = out.size() - 1;
  for (uint64_t i = 0; i < n; ++i) out[i] = text[start + i];
  out[n] = 0;
  return true;
}

}  // namespace

ArrayView<const BrcmfFirmwareMapping> brcmf_firmware_table(BrcmfBusType bus) {
  switch (bus) {
    case BrcmfBusType::Pcie: return ArrayView<const BrcmfFirmwareMapping>(kPcieFirmware);
    case BrcmfBusType::Sdio: return ArrayView<const BrcmfFirmwareMapping>(kSdioFirmware);
    case BrcmfBusType::Usb: return ArrayView<const BrcmfFirmwareMapping>(kUsbFirmware);
  }
  return ArrayView<const BrcmfFirmwareMapping>();
}

const BrcmfFirmwareMapping* brcmf_find_firmware(BrcmfBusType bus, uint32_t chip, uint32_t revision) {
  if (revision >= 32) return nullptr;
  for (const BrcmfFirmwareMapping& mapping : brcmf_firmware_table(bus))
    if (mapping.chip == chip && (mapping.revmask & (1u << revision)) != 0) return &mapping;
  return nullptr;
}

bool brcmf_file_name(StringView base, StringView extension, BrcmfFileName& out) {
  out.clear();
  if (out.assign(base) && out.append(extension)) return true;
  out.clear();
  return false;
}

bool brcmf_board_file_name(StringView path, StringView board_type, BrcmfFileName& out) {
  out.clear();
  if (board_type.empty()) return false;
  uint64_t dot = StringView::npos;
  for (uint64_t i = 0; i < path.size(); ++i)
    if (path[i] == '.') dot = i;
  if (dot == StringView::npos || dot == 0) return false;
  if (out.assign(path.first(dot)) && out.push_back('.') && out.append(board_type) && out.append(path.substr(dot))) return true;
  out.clear();
  return false;
}

namespace {
bool board_type_from(StringView first, StringView second, BrcmfFileName& out) {
  out.clear();
  if (!out.assign(first) || (!second.empty() && (!out.push_back('-') || !out.append(second)))) {
    out.clear();
    return false;
  }
  for (uint64_t i = 0; i < out.size(); ++i)
    if (out[i] == '/') out.set(i, '-');
  return !out.empty();
}
}  // namespace

bool brcmf_board_type_from_compatible(StringView compatible, BrcmfFileName& out) { return board_type_from(compatible, StringView(), out); }

bool brcmf_board_type_from_dmi(StringView sys_vendor, StringView product_name, BrcmfFileName& out) {
  if (sys_vendor.empty() || product_name.empty()) {
    out.clear();
    return false;
  }
  return board_type_from(sys_vendor, product_name, out);
}

void brcmf_chip_name(uint32_t chip, uint32_t revision, BrcmfChipName& out) {
  out.clear();
  (void)out.assign("BCM");
  if (chip > 0xa000 || chip < 0x4000) {
    (void)out.append_uint(chip);
  } else {
    constexpr char kDigits[] = "0123456789abcdef";
    char digits[8];
    int n = 0;
    uint32_t v = chip;
    do {
      digits[n++] = kDigits[v & 15];
      v >>= 4;
    } while (v != 0 && n < 8);
    while (n > 0) (void)out.push_back(digits[--n]);
  }
  (void)out.push_back('/');
  (void)out.append_uint(revision);
}

BrcmfNvramResult brcmf_nvram_strip(ConstByteView text, const BrcmfNvramOptions& options, ByteBuffer& out) {
  out.clear();
  NvramParser parser(text, options.has_platform_mac, out);
  if (!parser.run()) {
    out.clear();
    return BrcmfNvramResult::NoMemory;
  }
  bool boardrev_found = parser.boardrev_found;
  if (parser.multi_dev_v1 || parser.multi_dev_v2) {
    ByteBuffer stripped(out.allocator());
    boardrev_found = false;
    const bool ok = parser.multi_dev_v1 ? strip_multi_v1(out.view(), options.domain_nr, options.bus_nr, &boardrev_found, stripped)
                                        : strip_multi_v2(out.view(), options.domain_nr, options.bus_nr, &boardrev_found, stripped);
    out.clear();
    if (!ok || !out.append(stripped.view())) {
      out.clear();
      return BrcmfNvramResult::NoMemory;
    }
  }
  if (out.size() == 0) return BrcmfNvramResult::Empty;

  // brcmf_fw_add_defaults
  if (!boardrev_found && (!out.append(StringView(brcmf_nvram::kDefaultBoardrev).bytes()) || !out.put8(0))) {
    out.clear();
    return BrcmfNvramResult::NoMemory;
  }
  // brcmf_fw_add_macaddr: "macaddr=%pM"
  if (options.has_platform_mac) {
    BrcmfFileName entry;
    bool ok = entry.assign("macaddr=");
    for (int i = 0; i < 6 && ok; ++i) ok = (i == 0 || entry.push_back(':')) && append_hex_byte(entry, options.platform_mac[i]);
    if (!ok || !out.append(entry.view().bytes()) || !out.put8(0)) {
      out.clear();
      return BrcmfNvramResult::NoMemory;
    }
  }
  // pad to roundup(len + 1, 4), then the length token
  const uint64_t padded = (out.size() + 1 + 3) & ~uint64_t{3};
  if (!out.put_zeros(padded - out.size())) {
    out.clear();
    return BrcmfNvramResult::NoMemory;
  }
  const uint32_t words = static_cast<uint32_t>(padded / 4);
  const uint32_t token = (~words << 16) | (words & 0xffff);
  if (!out.put32(token)) {
    out.clear();
    return BrcmfNvramResult::NoMemory;
  }
  return BrcmfNvramResult::Ok;
}

bool brcmf_nvram_find(ConstByteView image, StringView key, StringView* value) {
  uint64_t i = 0;
  while (i < image.size()) {
    uint64_t end = i;
    while (end < image.size() && image[end] != 0) ++end;
    const StringView entry(image.subview(i, end - i));
    if (entry.size() > key.size() && entry.starts_with(key) && entry[key.size()] == '=') {
      *value = entry.substr(key.size() + 1);
      return true;
    }
    i = end + 1;
  }
  return false;
}

bool brcmf_image_version(ConstByteView image, ArrayView<char> out) {
  const StringView text(image);
  const StringView marker("Version: ");
  uint64_t at = text.find(marker);
  while (at != StringView::npos) {
    if (copy_token(text, at + marker.size(), out)) {
      // a version starts with a digit ("7.45.286"); format strings ("Version: %s") are skipped
      if (out[0] >= '0' && out[0] <= '9') return true;
    }
    at = text.find(marker, at + 1);
  }
  if (!out.empty()) out[0] = 0;
  return false;
}

bool brcmf_console_version(ConstByteView log, ArrayView<char> out) {
  const StringView text(log);
  uint64_t at = text.find(StringView("RTE ("));
  while (at != StringView::npos) {
    const uint64_t close = text.find(StringView(") "), at);
    if (close != StringView::npos && copy_token(text, close + 2, out) && out[0] >= '0' && out[0] <= '9') return true;
    at = text.find(StringView("RTE ("), at + 1);
  }
  if (!out.empty()) out[0] = 0;
  return false;
}

}  // namespace hardware
