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

// Follows nouveau (MIT license): nvkm/subdev/bios/shadow.c (nvbios_shadow, shadow_method, shadow_image, shadow_fetch), shadowramin.c,
// shadowrom.c, shadowacpi.c, shadowpci.c, image.c, pcir.c, npde.c, and nvkm/subdev/pci (nvkm_pci_rom_shadow). Stricter than nouveau in one
// place: the first x86 image's PCI data structure must name NVIDIA (0x10de), and the legacy shadow must name the card's device ID.
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBios.hpp"
#include "Kernel/Sync.hpp"

namespace hardware {
namespace nv {

namespace {

bool rd16(ConstByteView d, uint64_t at, uint16_t* out) {
  uint8_t a = 0, b = 0;
  if (!d.try_get(at, &a) || at == UINT64_MAX || !d.try_get(at + 1, &b)) return false;
  *out = static_cast<uint16_t>(a | (b << 8));
  return true;
}
bool rd32(ConstByteView d, uint64_t at, uint32_t* out) {
  uint16_t lo = 0, hi = 0;
  if (!rd16(d, at, &lo) || at > UINT64_MAX - 2 || !rd16(d, at + 2, &hi)) return false;
  *out = lo | (uint32_t{hi} << 16);
  return true;
}

void put_le32(ByteView out, uint64_t at, uint32_t value) {
  for (uint32_t i = 0; i < 4 && at + i < out.size(); ++i) out[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

// One source: `read` fills `out` with the image's bytes from `offset` on and returns how many it could (nouveau's func->read).
class Reader {
 public:
  virtual uint32_t read(uint32_t offset, ByteView out) = 0;

 protected:
  Reader() = default;
  ~Reader() = default;
};

// A window of BAR0 (PRAMIN at 0x700000, PROM at 0x300000) read 32 bits at a time, refused past the 1 MiB window.
class Bar0Reader final : public Reader {
 public:
  Bar0Reader(INvRegisters& registers, uint32_t base) : registers_(registers), base_(base) {}
  uint32_t read(uint32_t offset, ByteView out) override {
    if (uint64_t{offset} + out.size() > NvBiosShadow::kMaxImageBytes) return 0;
    for (uint64_t i = 0; i < out.size(); i += 4) put_le32(out, i, registers_.read32(base_ + offset + static_cast<uint32_t>(i)));
    return static_cast<uint32_t>(out.size());
  }

 private:
  INvRegisters& registers_;
  uint32_t base_;
};

// acpi_read_fast: one call for everything asked (the ACPI spec says 4 KiB at most, but this is much faster where it works, nouveau notes the
// Lenovo W530). acpi_read_slow: page by page until one comes back short.
class AcpiReader final : public Reader {
 public:
  AcpiReader(INvAcpiRom& rom, bool fast) : rom_(rom), fast_(fast) {}
  uint32_t read(uint32_t offset, ByteView out) override {
    if (fast_) {
      const uint32_t got = rom_.read_rom(offset, out);
      return got == out.size() ? got : 0;    // (nouveau: "if (ret == fetch) return fetch;")
    }
    uint32_t fetched = 0;
    while (fetched < out.size()) {
      const uint64_t piece = out.size() - fetched < NvBiosShadow::kFetchGranule ? out.size() - fetched : NvBiosShadow::kFetchGranule;
      const uint32_t got = rom_.read_rom(offset + fetched, out.subview(fetched, piece));
      if (got != piece) break;
      fetched += got;
    }
    return fetched;
  }

 private:
  INvAcpiRom& rom_;
  bool fast_;
};

// A mapped window (the expansion ROM, the legacy shadow) read 32 bits at a time.
class RegionReader final : public Reader {
 public:
  explicit RegionReader(const MmioRegion& region) : region_(region) {}
  uint32_t read(uint32_t offset, ByteView out) override {
    if (offset >= region_.length()) return 0;
    const uint64_t available = region_.length() - offset;
    const uint64_t count = (out.size() < available ? out.size() : available) & ~uint64_t{3};
    for (uint64_t i = 0; i < count; i += 4) put_le32(out, i, region_.read32(offset + i));
    return static_cast<uint32_t>(count);
  }

 private:
  MmioRegion region_;
};

// The traits nouveau gives each source (struct nvbios_source).
struct SourceTraits {
  bool rw = false;                 // a copy in RAM that the VBIOS may have patched: a bad checksum is less suspicious
  bool require_checksum = false;   // ACPI fast: a bad image means the method returned junk past 4 KiB
};

// shadow_fetch: makes sure `data` holds at least `upto` bytes, read in 4 KiB pieces. False if the source has no more.
bool fetch(Reader& reader, ByteBuffer& data, uint64_t upto) {
  if (data.size() >= upto) return true;
  if (upto > NvBiosShadow::kMaxImageBytes) return false;
  const uint64_t start = data.size();
  uint64_t limit = (upto + NvBiosShadow::kFetchGranule - 1) & ~uint64_t{NvBiosShadow::kFetchGranule - 1};
  if (limit > NvBiosShadow::kMaxImageBytes) limit = NvBiosShadow::kMaxImageBytes;
  if (!data.put_zeros(limit - start)) return false;
  const uint32_t got = reader.read(static_cast<uint32_t>(start), data.writable_view().subview(start, limit - start));
  data.truncate(start + (got < limit - start ? got : limit - start));
  return data.size() >= upto;
}

// The PCI data structure (its pointer at 0x18 of the image) far enough to read the fields nvbios_pcirTp and nvbios_npdeTp need.
bool fetch_header(Reader& reader, ByteBuffer& data, uint32_t base) {
  if (!fetch(reader, data, uint64_t{base} + 0x1a)) return false;
  uint16_t pcir = 0;
  if (!rd16(data.view(), uint64_t{base} + 0x18, &pcir)) return false;
  // The PCIR (0x18 bytes) and the NPDE after it ((pcir + header length + 15) & ~15, 0x0c bytes): fetched as far as there is.
  const uint64_t pcir_end = uint64_t{base} + pcir + 0x18;
  if (!fetch(reader, data, pcir_end)) return false;
  uint16_t pcir_length = 0;
  if (rd16(data.view(), uint64_t{base} + pcir + 0x0a, &pcir_length)) {
    const uint64_t npde_end = ((uint64_t{base} + pcir + pcir_length + 0x0f) & ~uint64_t{0x0f}) + 0x0c;
    (void)fetch(reader, data, npde_end);   // (an image without an NPDE may well end before it)
  }
  return true;
}

struct Score {
  uint32_t score = 0;
  const char* reason = "ok";
  uint64_t end = 0;   // the end of the last good image
};

// shadow_image, for the chain of images from 0 on.
Score score_chain(Reader& reader, ByteBuffer& data, const SourceTraits& traits, uint16_t want_device) {
  Score result;
  uint32_t base = 0;
  for (uint32_t index = 0; index < NvBiosShadow::kMaxImages; ++index) {
    NvBiosImageHeader image;
    const char* problem = nullptr;
    if (!fetch_header(reader, data, base)) problem = "the image header could not be read";
    else problem = NvBiosShadow::parse_image_header(data.view(), base, &image);
    if (!problem && !fetch(reader, data, uint64_t{image.base} + image.size)) problem = "the image is shorter than its header says";
    if (!problem && index == 0 && image.vendor_id != NvBiosShadow::kNvidiaVendor) problem = "the PCI data structure's vendor is not NVIDIA";
    if (!problem && index == 0 && want_device != 0 && image.device_id != want_device) problem = "the PCI data structure names another device";
    if (problem) {
      if (index == 0) result.reason = problem;
      return result;
    }
    uint32_t score = 1;
    if (image.type == 0x00) {
      if (!NvBiosShadow::checksum_ok(data.view().subview(image.base, image.size))) {
        if (traits.require_checksum) {
          if (index == 0) {
            result.reason = "checksum failed (this source must have a good one)";
            return result;
          }
          return result;
        }
        score += traits.rw ? 2 : 1;
        if (index == 0) result.reason = "checksum failed";
      } else {
        score += 3;
      }
    } else {
      score += 3;
    }
    result.score += score;
    result.end = uint64_t{image.base} + image.size;
    if (image.last) break;
    base = image.base + image.size;
  }
  return result;
}

// --- The sources' init and fini (nvbios_source.init/fini) ---------------------------------------------------------------------------------

// pramin_init: the VBIOS image the x86 code left in VRAM, through the 1 MiB PRAMIN window after pointing NV_PBUS_BAR0_WINDOW (0x1700) at it.
const char* pramin_init(INvRegisters& r, const Chip& chip, uint32_t* saved_window) {
  if (chip.card_type < 0x50) return "PRAMIN has no VBIOS copy before NV50";
  uint32_t addr = 0;
  if (chip.card_type >= 0x170) addr = chip.chipset == 0x170 ? 1 : 0;   // nouveau: "XXX: find the fuse reg for this" (GA100 has no display)
  else if (chip.card_type >= 0x110) addr = r.read32(NvBiosShadow::kDispFuseGm100);
  else if (chip.card_type >= 0xc0) addr = r.read32(NvBiosShadow::kDispFuseGf100);
  if (addr & 1) return "the display is disabled (fused off): no image in VRAM";
  addr = r.read32(chip.card_type >= 0x140 ? NvBiosShadow::kVbiosWindowGv100 : NvBiosShadow::kVbiosWindowNv50);
  if (!(addr & 0x8)) return "the display's VBIOS window is not enabled";
  if ((addr & 0x3) != 1) return "the display's VBIOS window is not in VRAM";
  uint64_t vram = uint64_t{addr & 0xffffff00u} << 8;
  if (!vram) vram = (uint64_t{r.read32(NvBiosShadow::kPraminWindow)} << 16) + 0xf0000;   // inherited from xf86-video-nv
  *saved_window = r.read32(NvBiosShadow::kPraminWindow);
  r.write32(NvBiosShadow::kPraminWindow, static_cast<uint32_t>(vram >> 16));
  return nullptr;
}

// nvkm_pci_rom_shadow: bit 0 of PCI config 0x50 (through BAR0's mirror of the config space) maps the ROM shadow over PROM; clear it to see
// the ROM itself.
void rom_shadow(INvRegisters& r, const Chip& chip, bool shadow) {
  const uint32_t reg = (chip.card_type < 0x40 ? NvBiosShadow::kPciMirrorNv04 : NvBiosShadow::kPciMirrorNv40) + NvBiosShadow::kPciRomShadow;
  r.mask(reg, 1, shadow ? 1 : 0);
}

// What the expansion ROM BAR gives: the window, switched on (pci_enable_rom + pci_map_rom). False and a reason if it cannot be.
const char* pcirom_init(const NvBiosSources& s, uint32_t* original, MmioRegion* region) {
  if (!s.pci || !s.mapper) return "no PCI access";
  *original = s.pci->read32(s.function, NvBiosShadow::kRomAddressRegister);
  const uint64_t base = *original & NvBiosShadow::kRomAddressMask;
  if (base == 0) return "no expansion ROM base assigned";
  s.pci->write32(s.function, NvBiosShadow::kRomAddressRegister, NvBiosShadow::kRomAddressMask);
  const uint32_t sized = s.pci->read32(s.function, NvBiosShadow::kRomAddressRegister) & NvBiosShadow::kRomAddressMask;
  s.pci->write32(s.function, NvBiosShadow::kRomAddressRegister, *original & ~NvBiosShadow::kRomEnable);
  if (sized == 0) return "the expansion ROM BAR is not implemented";
  const uint64_t window = (~uint64_t{sized} + 1) & 0xFFFFFFFFull;
  if (window == 0 || window > NvBiosShadow::kMaxRomWindowBytes) return "the expansion ROM window has an impossible size";
  if (!s.mapper->map_registers(base, window, region)) return "the expansion ROM window cannot be mapped";
  s.pci->write32(s.function, NvBiosShadow::kRomAddressRegister, static_cast<uint32_t>(base) | NvBiosShadow::kRomEnable);
  return nullptr;
}

constexpr BiosSource kOrder[kBiosSourceCount] = {BiosSource::Pramin, BiosSource::Prom, BiosSource::AcpiFast, BiosSource::AcpiSlow,
                                                 BiosSource::PciRom, BiosSource::Platform};
// nvbios_shadow's mthds[].skip: tried only while the best score is below it (0: always).
constexpr uint32_t kSkip[kBiosSourceCount] = {0, 0, 0, 4, 1, 1};

// Tries one source into `data`; the score (0 = nothing usable) and the reason.
Score try_source(BiosSource source, const NvBiosSources& s, ByteBuffer& data) {
  Score none;
  data.clear();
  switch (source) {
    case BiosSource::Pramin: {
      if (!s.registers) { none.reason = "no BAR0"; return none; }
      uint32_t saved = 0;
      if (const char* why = pramin_init(*s.registers, s.chip, &saved)) { none.reason = why; return none; }
      Bar0Reader reader(*s.registers, NvBiosShadow::kPraminBase);
      const Score score = score_chain(reader, data, SourceTraits{true, false}, 0);
      s.registers->write32(NvBiosShadow::kPraminWindow, saved);   // pramin_fini
      return score;
    }
    case BiosSource::Prom: {
      if (!s.registers) { none.reason = "no BAR0"; return none; }
      if (s.chip.card_type == 0x40 && s.chip.chipset >= 0x4c) { none.reason = "the NV4x IGPs have no PROM"; return none; }
      rom_shadow(*s.registers, s.chip, false);
      Bar0Reader reader(*s.registers, NvBiosShadow::kPromBase);
      const Score score = score_chain(reader, data, SourceTraits{false, false}, 0);
      rom_shadow(*s.registers, s.chip, true);                      // prom_fini
      return score;
    }
    case BiosSource::AcpiFast:
    case BiosSource::AcpiSlow: {
      if (!s.acpi_rom) { none.reason = "the GPU has no ACPI _ROM method"; return none; }
      AcpiReader reader(*s.acpi_rom, source == BiosSource::AcpiFast);
      return score_chain(reader, data, SourceTraits{false, source == BiosSource::AcpiFast}, 0);
    }
    case BiosSource::PciRom: {
      uint32_t original = 0;
      MmioRegion region;
      if (const char* why = pcirom_init(s, &original, &region)) { none.reason = why; return none; }
      RegionReader reader(region);
      const Score score = score_chain(reader, data, SourceTraits{true, false}, 0);
      s.pci->write32(s.function, NvBiosShadow::kRomAddressRegister, original);   // pci_disable_rom (the register as the firmware left it)
      return score;
    }
    case BiosSource::Platform: {
      if (!s.boot_display) { none.reason = "not the boot display: the legacy shadow is another card's"; return none; }
      if (!s.mapper) { none.reason = "no mapper"; return none; }
      MmioRegion region;
      if (!s.mapper->map_registers(NvBiosShadow::kLegacyShadowBase, NvBiosShadow::kLegacyShadowBytes, &region)) {
        none.reason = "the legacy shadow cannot be mapped";
        return none;
      }
      RegionReader reader(region);
      return score_chain(reader, data, SourceTraits{true, false}, s.device_id);
    }
    case BiosSource::None: break;
  }
  return none;
}

}  // namespace

const char* bios_source_name(BiosSource source) {
  switch (source) {
    case BiosSource::None: return "none";
    case BiosSource::Pramin: return "PRAMIN (the copy in VRAM)";
    case BiosSource::Prom: return "PROM (the ROM through BAR0)";
    case BiosSource::AcpiFast: return "ACPI _ROM (in one call)";
    case BiosSource::AcpiSlow: return "ACPI _ROM (4 KiB at a time)";
    case BiosSource::PciRom: return "PCI expansion ROM";
    case BiosSource::Platform: return "legacy shadow at 0xC0000";
  }
  return "unknown";
}

const char* NvBiosShadow::parse_image_header(ConstByteView data, uint32_t base, NvBiosImageHeader* out) {
  NvBiosImageHeader image;
  image.base = base;
  uint16_t signature = 0;
  if (!rd16(data, base, &signature)) return "the image is too short for a signature";
  if (signature != 0xaa55 && signature != 0xbb77 && signature != 0x4e56) return "no ROM signature (0x55 0xAA)";
  uint16_t pointer = 0;
  if (!rd16(data, uint64_t{base} + 0x18, &pointer) || pointer == 0) return "no PCI data structure pointer";
  const uint64_t pcir = uint64_t{base} + pointer;
  uint32_t pcir_signature = 0;
  if (!rd32(data, pcir, &pcir_signature)) return "the PCI data structure lies outside the image";
  if (pcir_signature != 0x52494350 && pcir_signature != 0x53494752 && pcir_signature != 0x5344504e) return "the PCI data structure has no PCIR signature";
  uint16_t pcir_length = 0, units = 0;
  uint8_t type = 0, indicator = 0;
  if (!rd16(data, pcir + 0x04, &image.vendor_id) || !rd16(data, pcir + 0x06, &image.device_id) || !rd16(data, pcir + 0x0a, &pcir_length) ||
      !rd16(data, pcir + 0x10, &units) || !data.try_get(pcir + 0x14, &type) || !data.try_get(pcir + 0x15, &indicator))
    return "the PCI data structure is cut short";
  image.size = uint32_t{units} * 512;
  image.type = type;
  image.last = (indicator & 0x80) != 0;
  if (image.type != 0x70) {
    // The NPDE (NVIDIA's PCI data extension) right after the PCIR, 16 byte aligned, overrides the size and the last flag.
    const uint64_t npde = (pcir + pcir_length + 0x0f) & ~uint64_t{0x0f};
    uint32_t npde_signature = 0;
    if (rd32(data, npde, &npde_signature) && npde_signature == 0x4544504e) {
      uint16_t npde_units = 0;
      uint8_t npde_last = 0;
      if (!rd16(data, npde + 0x08, &npde_units) || !data.try_get(npde + 0x0a, &npde_last)) return "the NPDE is cut short";
      image.size = uint32_t{npde_units} * 512;
      image.last = (npde_last & 0x80) != 0;
    }
  } else {
    image.last = true;
  }
  if (image.size == 0) return "the image length is 0";
  if (uint64_t{base} + image.size > kMaxImageBytes) return "the image is longer than 1 MiB";
  *out = image;
  return nullptr;
}

bool NvBiosShadow::checksum_ok(ConstByteView image) {
  uint8_t sum = 0;
  for (const uint8_t b : image) sum = static_cast<uint8_t>(sum + b);
  return sum == 0;
}

BiosSource NvBiosShadow::fetch(const NvBiosSources& sources, ByteBuffer* out, NvBiosFetchReport* report) {
  NvBiosFetchReport local;
  NvBiosFetchReport& r = report ? *report : local;
  r = NvBiosFetchReport{};
  out->clear();
  if (!sources.allocator) return BiosSource::None;
  ByteBuffer candidate(*sources.allocator);
  uint32_t best_score = 0;
  BiosSource best = BiosSource::None;
  for (uint32_t i = 0; i < kBiosSourceCount; ++i) {
    NvBiosAttempt& attempt = r.attempts[i];
    attempt.source = kOrder[i];
    if (kSkip[i] != 0 && best_score >= kSkip[i]) {
      attempt.reason = "skipped: a better image was found already";
      continue;
    }
    attempt.tried = true;
    const Score score = try_source(kOrder[i], sources, candidate);
    attempt.score = score.score;
    attempt.reason = score.reason;
    if (score.score > best_score) {
      candidate.truncate(score.end);
      out->clear();
      if (!out->append(candidate.view())) {
        attempt.reason = "out of memory copying the image";
        continue;
      }
      best_score = score.score;
      best = kOrder[i];
    }
  }
  r.chosen = best;
  r.score = best_score;
  return best;
}

// --- AmlNvAcpiRom -------------------------------------------------------------------------------------------------------------------------

namespace {
constexpr uint32_t kMaxAmlWalk = 4096, kMaxAmlDepth = 16;

acpi::Node* rom_method_of(acpi::AmlInterpreter& aml, acpi::Node* device) {
  for (acpi::Node* n = aml.first_child(device); n; n = aml.next_sibling(n)) {
    const char* name = aml.name_of(n);
    if (aml.is_method(n) && name[0] == '_' && name[1] == 'R' && name[2] == 'O' && name[3] == 'M') return n;
  }
  return nullptr;
}

acpi::Node* find_rom(acpi::AmlInterpreter& aml, acpi::Node* node, uint32_t depth, uint64_t adr, uint32_t* visited) {
  for (acpi::Node* n = aml.first_child(node); n; n = aml.next_sibling(n)) {
    if (++*visited > kMaxAmlWalk) return nullptr;
    if (aml.is_device(n)) {
      if (acpi::Node* method = rom_method_of(aml, n)) {
        uint64_t device_adr = 0;
        acpi::Node* adr_node = aml.find("_ADR", n);
        if (adr_node && aml.parent(adr_node) == n && aml.evaluate_integer(adr_node, &device_adr) && device_adr == adr) return method;
      }
    }
    if (depth < kMaxAmlDepth) {
      if (acpi::Node* found = find_rom(aml, n, depth + 1, adr, visited)) return found;
    }
  }
  return nullptr;
}
}  // namespace

bool AmlNvAcpiRom::find(acpi::AmlInterpreter& aml, PciAddress function) {
  kernel::ScopedRecursiveMutex guard(aml.mutex());
  uint32_t visited = 0;
  method_ = find_rom(aml, aml.root(), 0, (uint64_t{function.device} << 16) | function.function, &visited);
  aml_ = method_ ? &aml : nullptr;
  return method_ != nullptr;
}

uint32_t AmlNvAcpiRom::read_rom(uint32_t offset, ByteView out) {
  if (!aml_ || !method_ || out.empty()) return 0;
  kernel::ScopedRecursiveMutex guard(aml_->mutex());   // the result stays valid only until the next evaluation
  acpi::Value arguments[2];
  arguments[0].kind = acpi::ValueKind::Integer;
  arguments[0].integer = offset;
  arguments[1].kind = acpi::ValueKind::Integer;
  arguments[1].integer = out.size();
  acpi::Value result;
  if (!aml_->evaluate(method_, arguments, 2, &result)) return 0;
  if (result.kind != acpi::ValueKind::Buffer || result.bytes == nullptr) return 0;
  const uint64_t count = result.length < out.size() ? result.length : out.size();   // nouveau_rom_call: min(len, buffer length)
  if (count == 0) return 0;
  (void)out.copy_from(ConstByteView(result.bytes, count));
  return static_cast<uint32_t>(count);
}

}  // namespace nv
}  // namespace hardware
