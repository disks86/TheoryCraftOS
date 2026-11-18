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

// Follows nouveau (MIT license): nvkm/subdev/bios/init.c (every opcode handler, nvbios_exec, nvbios_post), ramcfg.c and M0203.c (the RAM
// configuration index of INIT_RAM_RESTRICT_*), engine/disp/vga.c (nvkm_rdport/wrport/rdvgai/wrvgai), devinit/gt215.c (the partition filter).
// Where this differs from nouveau on purpose it says so at the place.
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvInitScript.hpp"

namespace hardware {
namespace nv {

// --- VGA over 32-bit registers -----------------------------------------------------------------------------------------------------------

uint8_t RegistersNvVgaIo::read8(uint32_t offset) {
  const uint32_t word = registers_.read32(offset & ~3u);
  return static_cast<uint8_t>(word >> ((offset & 3u) * 8));
}

void RegistersNvVgaIo::write8(uint32_t offset, uint8_t value) {
  const uint32_t shift = (offset & 3u) * 8;
  const uint32_t word = registers_.read32(offset & ~3u);
  registers_.write32(offset & ~3u, (word & ~(0xffu << shift)) | (uint32_t{value} << shift));
}

void RegistersNvVgaIo::write_indexed(uint32_t offset, uint8_t index, uint8_t value) {
  if ((offset & 3u) == 3u) {   // the pair straddles two words (no VGA pair does)
    write8(offset, index);
    write8(offset + 1, value);
    return;
  }
  const uint32_t shift = (offset & 3u) * 8;
  const uint32_t word = registers_.read32(offset & ~3u);
  const uint32_t pair = uint32_t{index} | uint32_t{value} << 8;
  registers_.write32(offset & ~3u, (word & ~(0xffffu << shift)) | (pair << shift));
}

// --- Names -------------------------------------------------------------------------------------------------------------------------------

const char* init_access_name(NvInitAccess access) {
  switch (access) {
    case NvInitAccess::Mmio: return "mmio";
    case NvInitAccess::VgaPort: return "vga port";
    case NvInitAccess::VgaIndexed: return "vga indexed";
    case NvInitAccess::Pll: return "pll";
    case NvInitAccess::I2c: return "i2c";
    case NvInitAccess::Aux: return "aux";
    case NvInitAccess::Gpio: return "gpio";
    case NvInitAccess::MemInit: return "meminit";
    case NvInitAccess::Delay: return "delay";
  }
  return "?";
}

const char* init_error_name(NvInitError error) {
  switch (error) {
    case NvInitError::None: return "none";
    case NvInitError::NoTable: return "no init table";
    case NvInitError::Truncated: return "truncated";
    case NvInitError::UnknownOpcode: return "unknown opcode";
    case NvInitError::StepBudget: return "step budget exhausted";
    case NvInitError::TooDeep: return "nested too deep";
    case NvInitError::DelayBudget: return "delay budget exhausted";
    case NvInitError::Unsupported: return "unsupported";
  }
  return "?";
}

// --- Tables ------------------------------------------------------------------------------------------------------------------------------

NvInitTables find_init_tables(const NvBios& bios) {
  NvInitTables t;
  BitEntry entry;
  if (bios.bit_entry('I', &entry)) {
    t.table = entry.offset;
    t.length = entry.length;
  } else if (bios.bit_offset() == 0 && bios.bmp_version() >= 0x0510) {
    t.table = bios.bmp_offset() + 75;
    t.length = 14;
  }
  if (t.table == 0) return t;
  auto pointer = [&](uint32_t at, uint16_t* out) {
    uint16_t v = 0;
    if (t.length >= at + 2 && bios.rd16(uint64_t{t.table} + at, &v)) *out = v;
  };
  pointer(0x00, &t.script_table);
  pointer(0x02, &t.macro_index_table);
  pointer(0x04, &t.macro_table);
  pointer(0x06, &t.condition_table);
  pointer(0x08, &t.io_condition_table);
  pointer(0x0a, &t.io_flag_condition_table);
  pointer(0x0c, &t.function_table);
  pointer(0x0e, &t.unknown_script);
  pointer(0x10, &t.xlat_table);
  return t;
}

NvInitScript::NvInitScript(const NvBios& bios, const Chip& chip, INvRegisters& registers, INvVgaIo* vga, INvInitHooks* hooks,
                           INvInitTrace* trace, const NvInitOptions& options)
    : bios_(bios), chip_(chip), regs_(registers), vga_(vga), hooks_(hooks ? hooks : &null_hooks_), trace_(trace), options_(options) {
  tables_ = find_init_tables(bios);
  BitEntry info;
  uint8_t major = 0;
  if (bios.bit_entry('i', &info)) {
    if (bios.rd08(uint64_t{info.offset} + 3, &major)) bios_major_ = major;
  } else if (bios.bmp_offset() && bios.rd08(uint64_t{bios.bmp_offset()} + 13, &major)) {
    bios_major_ = major;
  }
}

uint32_t NvInitScript::script_offset(uint32_t index) const {
  if (index >= kMaxScripts) return 0;
  const uint16_t bmp = bios_.bit_offset() == 0 ? bios_.bmp_version() : 0;
  uint16_t data = 0;
  if (bmp && bmp < 0x0510) {
    if (index > 1 || bmp < 0x0100) return 0;
    const uint64_t at = uint64_t{bios_.bmp_offset()} + (bmp < 0x0200 ? 14 : 18) + index * 2;
    return bios_.rd16(at, &data) ? data : 0;
  }
  if (!tables_.script_table) return 0;
  return bios_.rd16(uint64_t{tables_.script_table} + index * 2, &data) ? data : 0;
}

// --- Plumbing ----------------------------------------------------------------------------------------------------------------------------

bool NvInitScript::fail(NvInitError error, uint32_t at, const char* message) {
  if (result_.error == NvInitError::None) {
    result_.error = error;
    result_.error_offset = at;
    result_.message = message;
    uint8_t op = 0;
    if (bios_.rd08(offset_, &op)) result_.opcode = op;
  }
  return false;
}

bool NvInitScript::r8(uint64_t at, uint8_t* out) {
  if (bios_.rd08(at, out)) return true;
  return fail(NvInitError::Truncated, static_cast<uint32_t>(at), "read outside the image");
}
bool NvInitScript::r16(uint64_t at, uint16_t* out) {
  if (bios_.rd16(at, out)) return true;
  return fail(NvInitError::Truncated, static_cast<uint32_t>(at), "read outside the image");
}
bool NvInitScript::r32(uint64_t at, uint32_t* out) {
  if (bios_.rd32(at, out)) return true;
  return fail(NvInitError::Truncated, static_cast<uint32_t>(at), "read outside the image");
}

bool NvInitScript::delay(uint64_t us) {
  if (us > options_.max_delay_us) us = options_.max_delay_us;
  if (us > options_.total_delay_us || result_.delay_us > options_.total_delay_us - us)
    return fail(NvInitError::DelayBudget, offset_, "the script's delays exceed the run's budget");
  result_.delay_us += us;
  trace(NvInitAccess::Delay, 0, 0, static_cast<uint32_t>(us));
  if (!options_.dry_run) regs_.delay_us(static_cast<uint32_t>(us));
  return true;
}

void NvInitScript::trace(NvInitAccess kind, uint32_t address, uint32_t index, uint32_t value) {
  if (!trace_) return;
  NvInitWrite w;
  w.kind = kind;
  w.address = address;
  w.index = index;
  w.value = value;
  w.script_offset = offset_;
  trace_->on_write(w);
}

bool NvInitScript::shadow_get(uint8_t kind, uint32_t key, uint32_t* value) const {
  for (const Shadow& s : shadow_) {
    if (s.kind == 0) return false;
    if (s.kind == kind && s.key == key) {
      *value = s.value;
      return true;
    }
  }
  return false;
}

void NvInitScript::shadow_put(uint8_t kind, uint32_t key, uint32_t value) {
  for (Shadow& s : shadow_) {
    if (s.kind == 0) {
      s.kind = kind;
      s.key = key;
      s.value = value;
      return;
    }
    if (s.kind == kind && s.key == key) {
      s.value = value;
      return;
    }
  }
  result_.shadow_full = true;
}

// --- Registers ---------------------------------------------------------------------------------------------------------------------------

int NvInitScript::init_or() {
  if (exec()) {
    if (options_.or_index >= 0) return options_.or_index;
    warn();   // "script needs OR!!"
  }
  return 0;
}

int NvInitScript::init_link() {
  if (exec()) {
    if (options_.link) return options_.link == 2;
    warn();   // "script needs OR link"
  }
  return 0;
}

int NvInitScript::init_head() {
  if (exec()) {
    if (head_ >= 0) return head_;
    warn();   // "script needs head"
  }
  return 0;
}

uint8_t NvInitScript::init_conn() {
  if (exec()) {
    if (options_.output.present && options_.output.connector_type != 0xff) return options_.output.connector_type;
    warn();   // "script needs connector type"
  }
  return 0xff;
}

bool NvInitScript::nvreg(uint32_t* reg) {
  uint32_t r = *reg & ~3u;
  if (chip_.card_type >= 0x50) {
    if (r & 0x80000000u) {
      r += static_cast<uint32_t>(init_head()) * 0x800u;
      r &= ~0x80000000u;
    }
    if (r & 0x40000000u) {
      r += static_cast<uint32_t>(init_or()) * 0x800u;
      r &= ~0x40000000u;
      if (r & 0x20000000u) {
        r += static_cast<uint32_t>(init_link()) * 0x80u;
        r &= ~0x20000000u;
      }
    }
  }
  if (r & ~0x00fffffcu) {
    // nouveau warns and goes on with the address; one outside BAR0 is not accessed here.
    warn();
    return false;
  }
  if (options_.gt215_partition_filter) {
    static const uint32_t kParts[][3] = {
        {0x100720, 0x1008bc, 4}, {0x100a20, 0x100adc, 4}, {0x100d80, 0x100ddc, 4}, {0x110000, 0x110f9c, 4},
        {0x111000, 0x11103c, 8}, {0x111080, 0x1110fc, 4}, {0x111120, 0x1111fc, 4}, {0x111300, 0x1114bc, 4},
    };
    for (const auto& p : kParts) {
      if (r >= p[0] && r <= p[1]) {
        const uint32_t part = (r / p[2]) & 7;
        if (!r001540_read_) {
          r001540_ = regs_.read32(0x001540);
          r001540_read_ = true;
        }
        if (part >= static_cast<uint32_t>(__builtin_popcount((r001540_ >> 16) & 0xff))) return false;
        break;
      }
    }
  }
  *reg = r;
  return true;
}

uint32_t NvInitScript::raw_read32(uint32_t reg) {
  uint32_t v = 0;
  if (options_.dry_run && shadow_get(1, reg, &v)) return v;
  return regs_.read32(reg);
}

void NvInitScript::raw_write32(uint32_t reg, uint32_t value) {
  trace(NvInitAccess::Mmio, reg, 0, value);
  ++result_.mmio_writes;
  if (options_.dry_run) shadow_put(1, reg, value);
  else regs_.write32(reg, value);
}

uint32_t NvInitScript::rd32(uint32_t reg) {
  if (nvreg(&reg) && exec()) return raw_read32(reg);
  return 0;
}

void NvInitScript::wr32(uint32_t reg, uint32_t value) {
  if (nvreg(&reg) && exec()) raw_write32(reg, value);
}

uint32_t NvInitScript::mask(uint32_t reg, uint32_t clear, uint32_t set) {
  if (nvreg(&reg) && exec()) {
    const uint32_t old = raw_read32(reg);
    raw_write32(reg, (old & ~clear) | set);
    return old;
  }
  return 0;
}

// --- VGA ports ---------------------------------------------------------------------------------------------------------------------------

uint32_t NvInitScript::port_offset(int head, uint16_t port) const {
  const uint32_t h = head <= 0 ? 0 : 1;
  if (chip_.card_type >= 0x50) return 0x601000u + port;
  if (port == 0x03c0 || port == 0x03c1 || port == 0x03c2 || port == 0x03da || port == 0x03d4 || port == 0x03d5)
    return 0x601000u + h * 0x2000u + port;
  if (port == 0x03cc || port == 0x03c4 || port == 0x03c5 || port == 0x03ce || port == 0x03cf)
    return 0x0c0000u + (chip_.card_type < 0x40 ? 0 : h) * 0x2000u + port;   // (before NV40, CR44 selects the head)
  return 0;
}

uint8_t NvInitScript::rdport(uint16_t port) {
  if (!exec()) return 0;
  const uint32_t off = port_offset(head_, port);
  if (!off) return 0;
  if (options_.dry_run) {
    uint32_t v = 0;
    return shadow_get(2, off, &v) ? static_cast<uint8_t>(v) : 0;
  }
  return vga_ ? vga_->read8(off) : 0;
}

void NvInitScript::wrport(uint16_t port, uint8_t value) {
  if (!exec()) return;
  const uint32_t off = port_offset(head_, port);
  if (!off) return;
  trace(NvInitAccess::VgaPort, off, 0, value);
  ++result_.vga_writes;
  if (options_.dry_run) shadow_put(2, off, value);
  else if (vga_) vga_->write8(off, value);
}

static bool indexed_port(uint16_t port) { return port == 0x03c4 || port == 0x03ce || port == 0x03d4; }

uint8_t NvInitScript::rdvgai(uint16_t port, uint8_t index) {
  if (!exec() || !indexed_port(port)) return 0;
  const uint32_t off = port_offset(head_ < 0 ? 0 : head_, port);
  if (!off) return 0;
  if (options_.dry_run) {
    uint32_t v = 0;
    return shadow_get(3, off << 8 | index, &v) ? static_cast<uint8_t>(v) : 0;
  }
  if (!vga_) return 0;
  vga_->write8(off, index);
  return vga_->read8(off + 1);
}

void NvInitScript::wrvgai(uint16_t port, uint8_t index, uint8_t value) {
  const bool cr44 = chip_.card_type < 0x50 && port == 0x03d4 && index == 0x44;
  if (cr44) head_ = 0;   // force head 0 for updates to CR44: it only exists on the first head
  if (exec() && indexed_port(port)) {
    const uint32_t off = port_offset(head_ < 0 ? 0 : head_, port);
    if (off) {
      trace(NvInitAccess::VgaIndexed, off, index, value);
      ++result_.vga_writes;
      if (options_.dry_run) shadow_put(3, off << 8 | index, value);
      else if (vga_) vga_->write_indexed(off, index, value);
    }
  }
  if (cr44 && value == 3) head_ = 1;   // the CR44 write selected head 1
}

// --- I2C, AUX, PLL -----------------------------------------------------------------------------------------------------------------------

II2cBus* NvInitScript::i2c(uint8_t index) {
  if (index == 0xff) index = (options_.output.present && options_.output.i2c_upper_default) ? kInitI2cSecondary : kInitI2cPrimary;
  return hooks_->i2c_bus(index);
}

int NvInitScript::rdi2cr(uint8_t index, uint8_t address, uint8_t reg) {
  II2cBus* bus = i2c(index);
  if (!bus || !exec() || options_.dry_run) return -1;
  uint8_t out[1] = {reg};
  uint8_t in[1] = {0};
  II2cBus::Message msgs[2];
  msgs[0].address = address;
  msgs[0].data = ByteView(out, 1);
  msgs[1].address = address;
  msgs[1].read = true;
  msgs[1].data = ByteView(in, 1);
  if (!bus->transfer(ArrayView<II2cBus::Message>(msgs, 2))) {
    ++result_.i2c_failures;
    return -1;
  }
  return in[0];
}

int NvInitScript::wri2cr(uint8_t index, uint8_t address, uint8_t reg, uint8_t value) {
  II2cBus* bus = i2c(index);
  if (!bus || !exec()) return -1;
  trace(NvInitAccess::I2c, index, uint32_t{address} << 8 | reg, value);
  if (options_.dry_run) return 0;
  uint8_t out[2] = {reg, value};
  II2cBus::Message msg;
  msg.address = address;
  msg.data = ByteView(out, 2);
  if (!bus->transfer(ArrayView<II2cBus::Message>(&msg, 1))) {
    ++result_.i2c_failures;
    return -1;
  }
  return 0;
}

uint8_t NvInitScript::rdauxr(uint32_t address) {
  if (!options_.output.present) {
    if (exec()) warn();   // "script needs output for aux"
    return 0;
  }
  IDpcdAux* aux = hooks_->aux_channel(options_.output.i2c_index);
  if (!aux || !exec() || options_.dry_run) return 0;
  uint8_t data[1] = {0};
  if (!aux->dpcd_read(address, ByteView(data, 1))) {
    ++result_.aux_failures;
    return 0;
  }
  return data[0];
}

void NvInitScript::wrauxr(uint32_t address, uint8_t value) {
  if (!options_.output.present) {
    if (exec()) warn();
    return;
  }
  IDpcdAux* aux = hooks_->aux_channel(options_.output.i2c_index);
  if (!aux || !exec()) return;
  trace(NvInitAccess::Aux, address, 0, value);
  if (options_.dry_run) return;
  uint8_t data[1] = {value};
  if (!aux->dpcd_write(address, ConstByteView(data, 1))) ++result_.aux_failures;
}

void NvInitScript::prog_pll(uint32_t type, uint32_t khz) {
  if (!exec()) return;
  trace(NvInitAccess::Pll, type, 0, khz);
  if (options_.dry_run) return;
  if (!hooks_->set_pll(type, khz)) {
    ++result_.plls_unsupported;
    warn();   // "failed to prog pll"
  }
}

// --- Conditions and lookups --------------------------------------------------------------------------------------------------------------

bool NvInitScript::condition_met(uint8_t cond, bool* met) {
  *met = false;
  const uint32_t table = tables_.condition_table;
  if (!table) {
    warn();   // "condition table pointer invalid"
    return true;
  }
  uint32_t reg = 0, msk = 0, val = 0;
  const uint64_t at = uint64_t{table} + cond * 12u;
  if (!r32(at, &reg) || !r32(at + 4, &msk) || !r32(at + 8, &val)) return false;
  *met = (rd32(reg) & msk) == val;
  return true;
}

bool NvInitScript::io_condition_met(uint8_t cond, bool* met) {
  *met = false;
  const uint32_t table = tables_.io_condition_table;
  if (!table) {
    warn();
    return true;
  }
  const uint64_t at = uint64_t{table} + cond * 5u;
  uint16_t port = 0;
  uint8_t index = 0, msk = 0, value = 0;
  if (!r16(at, &port) || !r8(at + 2, &index) || !r8(at + 3, &msk) || !r8(at + 4, &value)) return false;
  *met = (rdvgai(port, index) & msk) == value;
  return true;
}

bool NvInitScript::io_flag_condition_met(uint8_t cond, bool* met) {
  *met = false;
  const uint32_t table = tables_.io_flag_condition_table;
  if (!table) {
    warn();
    return true;
  }
  const uint64_t at = uint64_t{table} + cond * 9u;
  uint16_t port = 0, data = 0;
  uint8_t index = 0, msk = 0, shift = 0, dmask = 0, value = 0;
  if (!r16(at, &port) || !r8(at + 2, &index) || !r8(at + 3, &msk) || !r8(at + 4, &shift) || !r16(at + 5, &data) || !r8(at + 7, &dmask) ||
      !r8(at + 8, &value))
    return false;
  const uint32_t masked = rdvgai(port, index) & msk;
  const uint32_t ioval = shift < 8 ? masked >> shift : 0;
  uint8_t flag = 0;
  if (!r8(uint64_t{data} + ioval, &flag)) return false;
  *met = (flag & dmask) == value;
  return true;
}

uint8_t NvInitScript::ram_restrict_group_count() {
  BitEntry m;
  uint8_t v = 0;
  if (bios_.bit_entry('M', &m)) {
    if (m.version == 1 && m.length >= 5) return bios_.rd08(uint64_t{m.offset} + 2, &v) ? v : 0;
    if (m.version == 2 && m.length >= 3) return bios_.rd08(m.offset, &v) ? v : 0;
  }
  return 0;
}

uint8_t NvInitScript::ramcfg_index() {
  uint8_t strap = static_cast<uint8_t>((raw_read32(0x101000) & 0x3c) >> 2);
  BitEntry m;
  uint16_t xlat_table = 0;
  if (bios_.bit_entry('M', &m)) {
    if (m.version == 1 && m.length >= 5 && !bios_.rd16(uint64_t{m.offset} + 3, &xlat_table)) xlat_table = 0;
    if (m.version == 2 && m.length >= 3) {
      // nvbios_M0203Em: the M0203 table's entry for this strap gives the group directly
      uint16_t t = 0;
      uint8_t ver = 0, hdr = 0, len = 0, cnt = 0, type = 0;
      if (m.length >= 7 && m.length > 4 && bios_.rd16(uint64_t{m.offset} + 3, &t) && t && bios_.rd08(t, &ver) && ver == 0x10 &&
          bios_.rd08(uint64_t{t} + 1, &hdr) && bios_.rd08(uint64_t{t} + 2, &len) && bios_.rd08(uint64_t{t} + 3, &cnt) &&
          bios_.rd08(uint64_t{t} + 4, &type) && type == 0x00) {
        for (uint32_t i = 0; i < cnt; ++i) {
          uint8_t b0 = 0, b1 = 0;
          const uint64_t e = uint64_t{t} + hdr + uint64_t{i} * len;
          if (!bios_.rd08(e, &b0) || !bios_.rd08(e + 1, &b1)) break;
          if (((b0 & 0xf0) >> 4) == strap) return b1 & 0x0f;
        }
      }
      if (!bios_.rd16(uint64_t{m.offset} + 1, &xlat_table)) xlat_table = 0;
    }
  }
  if (xlat_table) {
    uint8_t v = 0;
    if (bios_.rd08(uint64_t{xlat_table} + strap, &v)) strap = v;
    else warn();
  }
  return strap;
}

uint8_t NvInitScript::ram_restrict() {
  // Cached from BIOS major 0x70 on, as the VBIOS's own parser does: re-reading the strap there upsets the memory controller (nouveau).
  if (!ramcfg_ || bios_major_ < 0x70) ramcfg_ = 0x80000000u | ramcfg_index();
  return static_cast<uint8_t>(ramcfg_ & 0x7fffffffu);
}

bool NvInitScript::xlat(uint8_t index, uint8_t offset, uint8_t* out) {
  *out = 0;
  const uint32_t table = tables_.xlat_table;
  if (!table) return true;
  uint16_t data = 0;
  if (!r16(uint64_t{table} + index * 2u, &data)) return false;
  if (!data) {
    warn();   // "xlat table pointer invalid"
    return true;
  }
  return r8(uint64_t{data} + offset, out);
}

uint32_t NvInitScript::tmds_reg(uint8_t tmds) {
  static const uint32_t kPramdacOffset[13] = {0, 0, 0x8, 0, 0x2000, 0, 0, 0, 0x2008, 0, 0, 0, 0x2000};
  static const uint32_t kPramdacTable[4] = {0x6808b0, 0x6808b8, 0x6828b0, 0x6828b8};
  if (tmds >= 0x80) {
    if (options_.output.present && options_.output.or_mask < 13) {
      uint32_t dac = kPramdacOffset[options_.output.or_mask];
      if (tmds == 0x81) dac ^= 8;
      return 0x6808b0 + dac;
    }
    if (exec()) warn();   // "tmds opcodes need dcb"
  } else {
    if (tmds < 4) return kPramdacTable[tmds];
    warn();   // "tmds selector unknown"
  }
  return 0;
}

// --- The opcodes -------------------------------------------------------------------------------------------------------------------------

struct NvInitOps {
  using S = NvInitScript;

  static bool reserved(S& s) {
    uint8_t op = 0;
    if (!s.r8(s.offset_, &op)) return false;
    const uint32_t length = op == 0xaa ? 4 : 1;
    uint8_t last = 0;
    if (length > 1 && !s.r8(uint64_t{s.offset_} + length - 1, &last)) return false;
    s.offset_ += length;
    return true;
  }

  static bool done(S& s) {
    s.offset_ = 0;
    return true;
  }

  static bool io_restrict_prog(S& s) {
    uint16_t port = 0;
    uint8_t index = 0, msk = 0, shift = 0, count = 0;
    uint32_t reg = 0;
    const uint64_t o = s.offset_;
    if (!s.r16(o + 1, &port) || !s.r8(o + 3, &index) || !s.r8(o + 4, &msk) || !s.r8(o + 5, &shift) || !s.r8(o + 6, &count) ||
        !s.r32(o + 7, &reg))
      return false;
    s.offset_ += 11;
    const uint32_t masked = s.rdvgai(port, index) & msk;
    const uint32_t conf = shift < 8 ? masked >> shift : 0;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t data = 0;
      if (!s.r32(s.offset_, &data)) return false;
      if (i == conf) s.wr32(reg, data);
      s.offset_ += 4;
    }
    return true;
  }

  static bool repeat(S& s) {
    uint8_t count = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &count)) return false;
    const uint32_t saved = s.repeat_;
    s.offset_ += 2;
    s.repeat_ = s.offset_;
    s.repend_ = s.offset_;
    while (count--) {
      s.offset_ = s.repeat_;
      if (!s.exec_loop()) return false;
    }
    s.offset_ = s.repend_;
    s.repeat_ = saved;
    return true;
  }

  static bool io_restrict_pll(S& s) {
    uint16_t port = 0;
    uint8_t index = 0, msk = 0, shift = 0, iofc = 0, count = 0;
    uint32_t reg = 0;
    const uint64_t o = s.offset_;
    if (!s.r16(o + 1, &port) || !s.r8(o + 3, &index) || !s.r8(o + 4, &msk) || !s.r8(o + 5, &shift) || !s.r8(o + 6, &iofc) ||
        !s.r8(o + 7, &count) || !s.r32(o + 8, &reg))
      return false;
    s.offset_ += 12;
    const uint32_t masked = s.rdvgai(port, index) & msk;
    const uint32_t conf = shift < 8 ? masked >> shift : 0;
    for (uint32_t i = 0; i < count; ++i) {
      uint16_t f = 0;
      if (!s.r16(s.offset_, &f)) return false;
      uint32_t freq = uint32_t{f} * 10;
      if (i == conf) {
        bool met = false;
        if (static_cast<int8_t>(iofc) > 0 && !s.io_flag_condition_met(iofc, &met)) return false;
        if (met) freq *= 2;
        s.prog_pll(reg, freq);
      }
      s.offset_ += 2;
    }
    return true;
  }

  static bool end_repeat(S& s) {
    s.offset_ += 1;
    if (s.repeat_) {
      s.repend_ = s.offset_;
      s.offset_ = 0;
    }
    return true;
  }

  static uint32_t shift_value(uint32_t data, uint8_t shift) {
    if (shift < 0x80) return shift < 32 ? data >> shift : 0;
    const uint32_t left = 0x100u - shift;
    return left < 32 ? data << left : 0;
  }

  static bool copy(S& s) {
    uint32_t reg = 0;
    uint8_t shift = 0, smask = 0, index = 0, msk = 0;
    uint16_t port = 0;
    const uint64_t o = s.offset_;
    if (!s.r32(o + 1, &reg) || !s.r8(o + 5, &shift) || !s.r8(o + 6, &smask) || !s.r16(o + 7, &port) || !s.r8(o + 9, &index) ||
        !s.r8(o + 10, &msk))
      return false;
    s.offset_ += 11;
    uint8_t data = static_cast<uint8_t>(s.rdvgai(port, index) & msk);
    data = static_cast<uint8_t>(data | (shift_value(s.rd32(reg), shift) & smask));
    s.wrvgai(port, index, data);
    return true;
  }

  static bool not_(S& s) {
    s.offset_ += 1;
    s.exec_inv();
    return true;
  }

  static bool io_flag_condition(S& s) {
    uint8_t cond = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &cond)) return false;
    s.offset_ += 2;
    bool met = false;
    if (!s.io_flag_condition_met(cond, &met)) return false;
    if (!met) s.exec_set(false);
    return true;
  }

  static bool generic_condition(S& s) {
    uint8_t cond = 0, size = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &cond) || !s.r8(uint64_t{s.offset_} + 2, &size)) return false;
    s.offset_ += 3;
    switch (cond) {
      case 0:   // CONDITION_ID_INT_DP
        if (s.init_conn() != 0x47) s.exec_set(false);
        break;
      case 1:   // CONDITION_ID_USE_SPPLL0
      case 2:   // CONDITION_ID_USE_SPPLL1
        if (s.options_.output.present && s.options_.output.has_dp_info) {
          if (!(s.options_.output.dp_info_flags & cond)) s.exec_set(false);
          break;
        }
        if (s.exec()) s.warn();   // "script needs dp output table data"
        break;
      case 5:   // CONDITION_ID_ASSR_SUPPORT
        if (!(s.rdauxr(0x0d) & 1)) s.exec_set(false);
        break;
      case 7:   // CONDITION_ID_NO_PANEL_SEQ_DELAYS
        s.exec_set(false);
        break;
      default:
        s.warn();
        s.offset_ += size;
        break;
    }
    return true;
  }

  static bool io_mask_or(S& s) {
    uint8_t index = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &index)) return false;
    const uint32_t orx = static_cast<uint32_t>(s.init_or()) & 7;
    s.offset_ += 2;
    const uint8_t data = s.rdvgai(0x03d4, index);
    s.wrvgai(0x03d4, index, static_cast<uint8_t>(data & ~(1u << orx)));
    return true;
  }

  static bool io_or(S& s) {
    uint8_t index = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &index)) return false;
    const uint32_t orx = static_cast<uint32_t>(s.init_or()) & 7;
    s.offset_ += 2;
    const uint8_t data = s.rdvgai(0x03d4, index);
    s.wrvgai(0x03d4, index, static_cast<uint8_t>(data | (1u << orx)));
    return true;
  }

  static bool andn_reg(S& s) {
    uint32_t reg = 0, m = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &reg) || !s.r32(uint64_t{s.offset_} + 5, &m)) return false;
    s.offset_ += 9;
    s.mask(reg, m, 0);
    return true;
  }

  static bool or_reg(S& s) {
    uint32_t reg = 0, m = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &reg) || !s.r32(uint64_t{s.offset_} + 5, &m)) return false;
    s.offset_ += 9;
    s.mask(reg, 0, m);
    return true;
  }

  static bool idx_addr_latched(S& s) {
    uint32_t creg = 0, dreg = 0, m = 0, data = 0;
    uint8_t count = 0;
    const uint64_t o = s.offset_;
    if (!s.r32(o + 1, &creg) || !s.r32(o + 5, &dreg) || !s.r32(o + 9, &m) || !s.r32(o + 13, &data) || !s.r8(o + 17, &count)) return false;
    s.offset_ += 18;
    while (count--) {
      uint8_t iaddr = 0, idata = 0;
      if (!s.r8(s.offset_, &iaddr) || !s.r8(uint64_t{s.offset_} + 1, &idata)) return false;
      s.offset_ += 2;
      s.wr32(dreg, idata);
      s.mask(creg, ~m, data | iaddr);
    }
    return true;
  }

  static bool io_restrict_pll2(S& s) {
    uint16_t port = 0;
    uint8_t index = 0, msk = 0, shift = 0, count = 0;
    uint32_t reg = 0;
    const uint64_t o = s.offset_;
    if (!s.r16(o + 1, &port) || !s.r8(o + 3, &index) || !s.r8(o + 4, &msk) || !s.r8(o + 5, &shift) || !s.r8(o + 6, &count) ||
        !s.r32(o + 7, &reg))
      return false;
    s.offset_ += 11;
    const uint32_t masked = s.rdvgai(port, index) & msk;
    const uint32_t conf = shift < 8 ? masked >> shift : 0;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t freq = 0;
      if (!s.r32(s.offset_, &freq)) return false;
      if (i == conf) s.prog_pll(reg, freq);
      s.offset_ += 4;
    }
    return true;
  }

  static bool pll2(S& s) {
    uint32_t reg = 0, freq = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &reg) || !s.r32(uint64_t{s.offset_} + 5, &freq)) return false;
    s.offset_ += 9;
    s.prog_pll(reg, freq);
    return true;
  }

  static bool i2c_byte(S& s) {
    uint8_t index = 0, addr = 0, count = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &index) || !s.r8(uint64_t{s.offset_} + 2, &addr) || !s.r8(uint64_t{s.offset_} + 3, &count))
      return false;
    addr >>= 1;
    s.offset_ += 4;
    while (count--) {
      uint8_t reg = 0, m = 0, data = 0;
      if (!s.r8(s.offset_, &reg) || !s.r8(uint64_t{s.offset_} + 1, &m) || !s.r8(uint64_t{s.offset_} + 2, &data)) return false;
      s.offset_ += 3;
      const int val = s.rdi2cr(index, addr, reg);
      if (val < 0) continue;
      (void)s.wri2cr(index, addr, reg, static_cast<uint8_t>((val & m) | data));
    }
    return true;
  }

  static bool zm_i2c_byte(S& s) {
    uint8_t index = 0, addr = 0, count = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &index) || !s.r8(uint64_t{s.offset_} + 2, &addr) || !s.r8(uint64_t{s.offset_} + 3, &count))
      return false;
    addr >>= 1;
    s.offset_ += 4;
    while (count--) {
      uint8_t reg = 0, data = 0;
      if (!s.r8(s.offset_, &reg) || !s.r8(uint64_t{s.offset_} + 1, &data)) return false;
      s.offset_ += 2;
      (void)s.wri2cr(index, addr, reg, data);
    }
    return true;
  }

  // (noinline: the 256-byte buffers stay out of dispatch's frame, which every nesting level of exec_loop repeats)
  __attribute__((noinline)) static bool zm_i2c(S& s) {
    uint8_t index = 0, addr = 0, count = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &index) || !s.r8(uint64_t{s.offset_} + 2, &addr) || !s.r8(uint64_t{s.offset_} + 3, &count))
      return false;
    addr >>= 1;
    s.offset_ += 4;
    uint8_t data[256];
    for (uint32_t i = 0; i < count; ++i) {
      if (!s.r8(s.offset_, &data[i])) return false;
      s.offset_ += 1;
    }
    if (s.exec()) {
      II2cBus* bus = s.i2c(index);
      if (bus) {
        s.trace(NvInitAccess::I2c, index, uint32_t{addr} << 8 | count, count ? data[0] : 0);
        if (!s.options_.dry_run) {
          II2cBus::Message msg;
          msg.address = addr;
          msg.data = ByteView(data, count);
          if (!bus->transfer(ArrayView<II2cBus::Message>(&msg, 1))) ++s.result_.i2c_failures;
        }
      }
    }
    return true;
  }

  static bool tmds(S& s) {
    uint8_t sel = 0, addr = 0, m = 0, data = 0;
    const uint64_t o = s.offset_;
    if (!s.r8(o + 1, &sel) || !s.r8(o + 2, &addr) || !s.r8(o + 3, &m) || !s.r8(o + 4, &data)) return false;
    const uint32_t reg = s.tmds_reg(sel);
    s.offset_ += 5;
    if (reg == 0) return true;
    s.wr32(reg + 0, addr | 0x00010000u);
    s.wr32(reg + 4, data | (s.rd32(reg + 4) & m));
    s.wr32(reg + 0, addr);
    return true;
  }

  static bool zm_tmds_group(S& s) {
    uint8_t sel = 0, count = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &sel) || !s.r8(uint64_t{s.offset_} + 2, &count)) return false;
    const uint32_t reg = s.tmds_reg(sel);
    s.offset_ += 3;
    while (count--) {
      uint8_t addr = 0, data = 0;
      if (!s.r8(s.offset_, &addr) || !s.r8(uint64_t{s.offset_} + 1, &data)) return false;
      s.offset_ += 2;
      // (nouveau writes to 0x000004 / 0x000000 when the selector is unknown; skipped here)
      if (reg == 0) continue;
      s.wr32(reg + 4, data);
      s.wr32(reg + 0, addr);
    }
    return true;
  }

  static bool cr_idx_adr_latch(S& s) {
    uint8_t addr0 = 0, addr1 = 0, base = 0, count = 0;
    const uint64_t o = s.offset_;
    if (!s.r8(o + 1, &addr0) || !s.r8(o + 2, &addr1) || !s.r8(o + 3, &base) || !s.r8(o + 4, &count)) return false;
    s.offset_ += 5;
    const uint8_t save0 = s.rdvgai(0x03d4, addr0);
    while (count--) {
      uint8_t data = 0;
      if (!s.r8(s.offset_, &data)) return false;
      s.offset_ += 1;
      s.wrvgai(0x03d4, addr0, base++);
      s.wrvgai(0x03d4, addr1, data);
    }
    s.wrvgai(0x03d4, addr0, save0);
    return true;
  }

  static bool cr(S& s) {
    uint8_t addr = 0, m = 0, data = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &addr) || !s.r8(uint64_t{s.offset_} + 2, &m) || !s.r8(uint64_t{s.offset_} + 3, &data)) return false;
    s.offset_ += 4;
    const uint8_t val = s.rdvgai(0x03d4, addr) & m;
    s.wrvgai(0x03d4, addr, val | data);
    return true;
  }

  static bool zm_cr(S& s) {
    uint8_t addr = 0, data = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &addr) || !s.r8(uint64_t{s.offset_} + 2, &data)) return false;
    s.offset_ += 3;
    s.wrvgai(0x03d4, addr, data);
    return true;
  }

  static bool zm_cr_group(S& s) {
    uint8_t count = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &count)) return false;
    s.offset_ += 2;
    while (count--) {
      uint8_t addr = 0, data = 0;
      if (!s.r8(s.offset_, &addr) || !s.r8(uint64_t{s.offset_} + 1, &data)) return false;
      s.offset_ += 2;
      s.wrvgai(0x03d4, addr, data);
    }
    return true;
  }

  static bool condition_time(S& s) {
    uint8_t cond = 0, retry = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &cond) || !s.r8(uint64_t{s.offset_} + 2, &retry)) return false;
    uint32_t wait = uint32_t{retry} * 50;
    if (wait > 100) wait = 100;
    s.offset_ += 3;
    if (!s.exec()) return true;
    while (wait--) {
      bool met = false;
      if (!s.condition_met(cond, &met)) return false;
      if (met) return true;
      if (!s.delay(20000)) return false;
    }
    s.exec_set(false);
    return true;
  }

  static bool ltime(S& s) {
    uint16_t msec = 0;
    if (!s.r16(uint64_t{s.offset_} + 1, &msec)) return false;
    s.offset_ += 3;
    if (s.exec()) return s.delay(uint64_t{msec} * 1000);
    return true;
  }

  static bool zm_reg_sequence(S& s) {
    uint32_t base = 0;
    uint8_t count = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &base) || !s.r8(uint64_t{s.offset_} + 5, &count)) return false;
    s.offset_ += 6;
    while (count--) {
      uint32_t data = 0;
      if (!s.r32(s.offset_, &data)) return false;
      s.offset_ += 4;
      s.wr32(base, data);
      base += 4;
    }
    return true;
  }

  static bool pll_indirect(S& s) {
    uint32_t reg = 0;
    uint16_t addr = 0, f = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &reg) || !s.r16(uint64_t{s.offset_} + 5, &addr) || !s.r16(addr, &f)) return false;
    s.offset_ += 7;
    s.prog_pll(reg, uint32_t{f} * 1000);
    return true;
  }

  static bool zm_reg_indirect(S& s) {
    uint32_t reg = 0, data = 0;
    uint16_t addr = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &reg) || !s.r16(uint64_t{s.offset_} + 5, &addr) || !s.r32(addr, &data)) return false;
    s.offset_ += 7;
    // nouveau writes to `addr` (the image offset) here, which its own trace line ("R[reg] = VBIOS[addr]") says is not meant; the
    // register is written.
    s.wr32(reg, data);
    return true;
  }

  static bool sub_direct(S& s) {
    uint16_t addr = 0;
    if (!s.r16(uint64_t{s.offset_} + 1, &addr)) return false;
    if (s.exec()) {
      const uint32_t save = s.offset_;
      s.offset_ = addr;
      if (!s.exec_loop()) return false;
      s.offset_ = save;
    }
    s.offset_ += 3;
    return true;
  }

  static bool jump(S& s) {
    uint16_t target = 0;
    if (!s.r16(uint64_t{s.offset_} + 1, &target)) return false;
    if (s.exec()) s.offset_ = target;
    else s.offset_ += 3;
    return true;
  }

  static bool i2c_if(S& s) {
    uint8_t index = 0, addr = 0, reg = 0, m = 0, data = 0;
    const uint64_t o = s.offset_;
    if (!s.r8(o + 1, &index) || !s.r8(o + 2, &addr) || !s.r8(o + 3, &reg) || !s.r8(o + 4, &m) || !s.r8(o + 5, &data)) return false;
    s.offset_ += 6;
    s.exec_force(true);
    const int value = s.rdi2cr(index, addr, reg);
    // (nouveau compares the low byte of -ENODEV when the read fails; a failed read is a condition not met here)
    if (value < 0 || (static_cast<uint8_t>(value) & m) != data) s.exec_set(false);
    s.exec_force(false);
    return true;
  }

  static bool copy_nv_reg(S& s) {
    uint32_t sreg = 0, smask = 0, sxor = 0, dreg = 0, dmask = 0;
    uint8_t shift = 0;
    const uint64_t o = s.offset_;
    if (!s.r32(o + 1, &sreg) || !s.r8(o + 5, &shift) || !s.r32(o + 6, &smask) || !s.r32(o + 10, &sxor) || !s.r32(o + 14, &dreg) ||
        !s.r32(o + 18, &dmask))
      return false;
    s.offset_ += 22;
    const uint32_t data = shift_value(s.rd32(sreg), shift);
    s.mask(dreg, ~dmask, (data & smask) ^ sxor);
    return true;
  }

  static bool zm_index_io(S& s) {
    uint16_t port = 0;
    uint8_t index = 0, data = 0;
    if (!s.r16(uint64_t{s.offset_} + 1, &port) || !s.r8(uint64_t{s.offset_} + 3, &index) || !s.r8(uint64_t{s.offset_} + 4, &data))
      return false;
    s.offset_ += 5;
    s.wrvgai(port, index, data);
    return true;
  }

  static bool compute_mem(S& s) {
    s.offset_ += 1;
    s.exec_force(true);
    if (s.exec()) {
      s.trace(NvInitAccess::MemInit, 0, 0, 0);
      if (!s.options_.dry_run && !s.hooks_->meminit()) ++s.result_.meminit_unsupported;
    }
    s.exec_force(false);
    return true;
  }

  static bool reset(S& s) {
    uint32_t reg = 0, data1 = 0, data2 = 0;
    const uint64_t o = s.offset_;
    if (!s.r32(o + 1, &reg) || !s.r32(o + 5, &data1) || !s.r32(o + 9, &data2)) return false;
    s.offset_ += 13;
    s.exec_force(true);
    const uint32_t savepci19 = s.mask(0x00184c, 0x00000f00, 0);
    s.wr32(reg, data1);
    if (s.exec() && !s.delay(10)) return false;
    s.wr32(reg, data2);
    s.wr32(0x00184c, savepci19);
    s.mask(0x001850, 0x00000001, 0);
    s.exec_force(false);
    return true;
  }

  static bool configure_mem_clk(S& s, const char* what) {
    s.offset_ += 1;
    if (s.bios_major_ > 2) return done(s);
    // The BMP's memory init / SDR / DDR sequence tables of NV04-era boards (bmp_mem_init_table ...) are not read here.
    return s.fail(NvInitError::Unsupported, s.offset_ - 1, what);
  }
  static bool configure_mem(S& s) { return configure_mem_clk(s, "INIT_CONFIGURE_MEM on a BIOS before major 3 (BMP memory tables)"); }
  static bool configure_clk(S& s) { return configure_mem_clk(s, "INIT_CONFIGURE_CLK on a BIOS before major 3 (BMP memory tables)"); }

  static bool configure_preinit(S& s) {
    s.offset_ += 1;
    if (s.bios_major_ > 2) return done(s);
    s.exec_force(true);
    uint32_t strap = s.rd32(0x101000);
    strap = ((strap << 2) & 0xf0) | ((strap & 0x40) >> 6);
    s.wrvgai(0x03d4, 0x3c, static_cast<uint8_t>(strap));
    s.exec_force(false);
    return true;
  }

  static bool io(S& s) {
    uint16_t port = 0;
    uint8_t m = 0, data = 0;
    // (nouveau reads these two bytes as the low bytes of 16-bit reads, the second one past the opcode's end)
    if (!s.r16(uint64_t{s.offset_} + 1, &port) || !s.r8(uint64_t{s.offset_} + 3, &m) || !s.r8(uint64_t{s.offset_} + 4, &data)) return false;
    s.offset_ += 5;
    if (s.chip_.card_type >= 0x50 && port == 0x03c3 && data == 0x01) {
      // nouveau: "should really figure out wtf this is ... somehow makes things work"
      s.mask(0x614100, 0xf0800000, 0x00800000);
      s.mask(0x00e18c, 0x00020000, 0x00020000);
      s.mask(0x614900, 0xf0800000, 0x00800000);
      s.mask(0x000200, 0x40000000, 0x00000000);
      if (s.exec() && !s.delay(10000)) return false;
      s.mask(0x00e18c, 0x00020000, 0x00000000);
      s.mask(0x000200, 0x40000000, 0x40000000);
      s.wr32(0x614100, 0x00800018);
      s.wr32(0x614900, 0x00800018);
      if (s.exec() && !s.delay(10000)) return false;
      s.wr32(0x614100, 0x10000018);
      s.wr32(0x614900, 0x10000018);
    }
    const uint8_t value = s.rdport(port) & m;
    s.wrport(port, data | value);
    return true;
  }

  static bool sub(S& s) {
    uint8_t index = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &index)) return false;
    const uint32_t addr = s.script_offset(index);
    if (addr && s.exec()) {
      const uint32_t save = s.offset_;
      s.offset_ = addr;
      if (!s.exec_loop()) return false;
      s.offset_ = save;
    }
    s.offset_ += 2;
    return true;
  }

  static bool ram_condition(S& s) {
    uint8_t m = 0, value = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &m) || !s.r8(uint64_t{s.offset_} + 2, &value)) return false;
    s.offset_ += 3;
    if ((s.rd32(0x100000) & m) != value) s.exec_set(false);
    return true;
  }

  static bool nv_reg(S& s) {
    uint32_t reg = 0, m = 0, data = 0;
    const uint64_t o = s.offset_;
    if (!s.r32(o + 1, &reg) || !s.r32(o + 5, &m) || !s.r32(o + 9, &data)) return false;
    s.offset_ += 13;
    s.mask(reg, ~m, data);
    return true;
  }

  static bool macro(S& s) {
    uint8_t index = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &index)) return false;
    const uint32_t table = s.tables_.macro_table;
    if (table) {
      uint32_t addr = 0, data = 0;
      const uint64_t at = uint64_t{table} + index * 8u;
      if (!s.r32(at, &addr) || !s.r32(at + 4, &data)) return false;
      s.wr32(addr, data);
    } else {
      s.warn();
    }
    s.offset_ += 2;
    return true;
  }

  static bool resume(S& s) {
    s.offset_ += 1;
    s.exec_set(true);
    return true;
  }

  static bool strap_condition(S& s) {
    uint32_t m = 0, value = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &m) || !s.r32(uint64_t{s.offset_} + 5, &value)) return false;
    s.offset_ += 9;
    if ((s.rd32(0x101000) & m) != value) s.exec_set(false);
    return true;
  }

  static bool time(S& s) {
    uint16_t usec = 0;
    if (!s.r16(uint64_t{s.offset_} + 1, &usec)) return false;
    s.offset_ += 3;
    if (!s.exec()) return true;
    if (usec < 1000) return s.delay(usec);
    return s.delay(uint64_t{(usec + 900u) / 1000u} * 1000);
  }

  static bool condition(S& s) {
    uint8_t cond = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &cond)) return false;
    s.offset_ += 2;
    bool met = false;
    if (!s.condition_met(cond, &met)) return false;
    if (!met) s.exec_set(false);
    return true;
  }

  static bool io_condition(S& s) {
    uint8_t cond = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &cond)) return false;
    s.offset_ += 2;
    bool met = false;
    if (!s.io_condition_met(cond, &met)) return false;
    if (!met) s.exec_set(false);
    return true;
  }

  static bool zm_reg16(S& s) {
    uint32_t addr = 0;
    uint16_t data = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &addr) || !s.r16(uint64_t{s.offset_} + 5, &data)) return false;
    s.offset_ += 7;
    s.wr32(addr, data);
    return true;
  }

  static bool index_io(S& s) {
    uint16_t port = 0;
    uint8_t index = 0, m = 0, data = 0;
    const uint64_t o = s.offset_;
    if (!s.r16(o + 1, &port) || !s.r8(o + 3, &index) || !s.r8(o + 4, &m) || !s.r8(o + 5, &data)) return false;
    s.offset_ += 6;
    const uint8_t value = s.rdvgai(port, index) & m;
    s.wrvgai(port, index, data | value);
    return true;
  }

  static bool pll(S& s) {
    uint32_t reg = 0;
    uint16_t f = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &reg) || !s.r16(uint64_t{s.offset_} + 5, &f)) return false;
    s.offset_ += 7;
    s.prog_pll(reg, uint32_t{f} * 10);
    return true;
  }

  static bool zm_reg(S& s) {
    uint32_t addr = 0, data = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &addr) || !s.r32(uint64_t{s.offset_} + 5, &data)) return false;
    s.offset_ += 9;
    if (addr == 0x000200) data |= 0x00000001;   // never take PMC_ENABLE's bit 0 away
    s.wr32(addr, data);
    return true;
  }

  static bool ram_restrict_pll(S& s) {
    uint8_t type = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &type)) return false;
    const uint8_t count = s.ram_restrict_group_count();
    const uint8_t strap = s.ram_restrict();
    s.offset_ += 2;
    for (uint32_t c = 0; c < count; ++c) {
      uint32_t freq = 0;
      if (!s.r32(s.offset_, &freq)) return false;
      if (c == strap) s.prog_pll(type, freq);
      s.offset_ += 4;
    }
    return true;
  }

  static bool one_byte(S& s) {   // INIT_RESET_BEGUN 0x8c, INIT_RESET_END 0x8d
    s.offset_ += 1;
    return true;
  }

  static bool gpio(S& s) {
    s.offset_ += 1;
    if (s.exec()) {
      s.trace(NvInitAccess::Gpio, kInitGpioAll, 0, 0);
      if (!s.options_.dry_run && !s.hooks_->gpio_reset(kInitGpioAll)) ++s.result_.gpio_unsupported;
    }
    return true;
  }

  static bool ram_restrict_zm_reg_group(S& s) {
    uint32_t addr = 0;
    uint8_t incr = 0, num = 0;
    const uint64_t o = s.offset_;
    if (!s.r32(o + 1, &addr) || !s.r8(o + 5, &incr) || !s.r8(o + 6, &num)) return false;
    const uint8_t count = s.ram_restrict_group_count();
    const uint8_t index = s.ram_restrict();
    s.offset_ += 7;
    for (uint32_t i = 0; i < num; ++i) {
      for (uint32_t j = 0; j < count; ++j) {
        uint32_t data = 0;
        if (!s.r32(s.offset_, &data)) return false;
        if (j == index) s.wr32(addr, data);
        s.offset_ += 4;
      }
      addr += incr;
    }
    return true;
  }

  static bool copy_zm_reg(S& s) {
    uint32_t sreg = 0, dreg = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &sreg) || !s.r32(uint64_t{s.offset_} + 5, &dreg)) return false;
    s.offset_ += 9;
    s.wr32(dreg, s.rd32(sreg));
    return true;
  }

  static bool zm_reg_group(S& s) {
    uint32_t addr = 0;
    uint8_t count = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &addr) || !s.r8(uint64_t{s.offset_} + 5, &count)) return false;
    s.offset_ += 6;
    while (count--) {
      uint32_t data = 0;
      if (!s.r32(s.offset_, &data)) return false;
      s.wr32(addr, data);
      s.offset_ += 4;
    }
    return true;
  }

  static bool xlat(S& s) {
    uint32_t saddr = 0, daddr = 0, dmask = 0;
    uint8_t sshift = 0, smask = 0, index = 0, shift = 0;
    const uint64_t o = s.offset_;
    if (!s.r32(o + 1, &saddr) || !s.r8(o + 5, &sshift) || !s.r8(o + 6, &smask) || !s.r8(o + 7, &index) || !s.r32(o + 8, &daddr) ||
        !s.r32(o + 12, &dmask) || !s.r8(o + 16, &shift))
      return false;
    s.offset_ += 17;
    const uint32_t data = shift_value(s.rd32(saddr), sshift) & smask;
    uint8_t x = 0;
    if (!s.xlat(index, static_cast<uint8_t>(data), &x)) return false;
    s.mask(daddr, ~dmask, shift < 32 ? uint32_t{x} << shift : 0);
    return true;
  }

  static bool zm_mask_add(S& s) {
    uint32_t addr = 0, m = 0, add = 0;
    const uint64_t o = s.offset_;
    if (!s.r32(o + 1, &addr) || !s.r32(o + 5, &m) || !s.r32(o + 9, &add)) return false;
    s.offset_ += 13;
    uint32_t data = s.rd32(addr);
    data = (data & m) | ((data + add) & ~m);
    s.wr32(addr, data);
    return true;
  }

  static bool auxch(S& s) {
    uint32_t addr = 0;
    uint8_t count = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &addr) || !s.r8(uint64_t{s.offset_} + 5, &count)) return false;
    s.offset_ += 6;
    while (count--) {
      uint8_t m = 0, data = 0;
      if (!s.r8(s.offset_, &m) || !s.r8(uint64_t{s.offset_} + 1, &data)) return false;
      m = s.rdauxr(addr) & m;
      s.wrauxr(addr, m | data);
      s.offset_ += 2;
    }
    return true;
  }

  static bool zm_auxch(S& s) {
    uint32_t addr = 0;
    uint8_t count = 0;
    if (!s.r32(uint64_t{s.offset_} + 1, &addr) || !s.r8(uint64_t{s.offset_} + 5, &count)) return false;
    s.offset_ += 6;
    while (count--) {
      uint8_t data = 0;
      if (!s.r8(s.offset_, &data)) return false;
      s.wrauxr(addr, data);
      s.offset_ += 1;
    }
    return true;
  }

  static bool i2c_long_if(S& s) {
    uint8_t index = 0, addr = 0, reglo = 0, reghi = 0, m = 0, data = 0;
    const uint64_t o = s.offset_;
    if (!s.r8(o + 1, &index) || !s.r8(o + 2, &addr) || !s.r8(o + 3, &reglo) || !s.r8(o + 4, &reghi) || !s.r8(o + 5, &m) || !s.r8(o + 6, &data))
      return false;
    addr >>= 1;
    s.offset_ += 7;
    II2cBus* bus = s.i2c(index);
    // (nouveau does this transfer whether or not the script is executing; a dry run does not)
    if (bus && !s.options_.dry_run) {
      uint8_t out[2] = {reghi, reglo};
      uint8_t in[1] = {0};
      II2cBus::Message msgs[2];
      msgs[0].address = addr;
      msgs[0].data = ByteView(out, 2);
      msgs[1].address = addr;
      msgs[1].read = true;
      msgs[1].data = ByteView(in, 1);
      if (bus->transfer(ArrayView<II2cBus::Message>(msgs, 2))) {
        if ((in[0] & m) == data) return true;
      } else {
        ++s.result_.i2c_failures;
      }
    }
    s.exec_set(false);
    return true;
  }

  __attribute__((noinline)) static bool gpio_ne(S& s) {
    uint8_t count = 0;
    if (!s.r8(uint64_t{s.offset_} + 1, &count)) return false;
    s.offset_ += 2;
    uint8_t list[256];
    for (uint32_t i = 0; i < count; ++i)
      if (!s.r8(uint64_t{s.offset_} + i, &list[i])) return false;
    if (const BoardInfo* board = s.options_.board) {
      for (uint32_t g = 0; g < board->gpio_count && g < BoardInfo::kMaxGpio; ++g) {
        const uint8_t func = board->gpio[g].function;
        if (func == 0xff) continue;
        bool listed = false;
        for (uint32_t i = 0; i < count; ++i) listed = listed || list[i] == func;
        if (!listed && s.exec()) {
          s.trace(NvInitAccess::Gpio, func, 0, 0);
          if (!s.options_.dry_run && !s.hooks_->gpio_reset(func)) ++s.result_.gpio_unsupported;
        }
      }
    }
    s.offset_ += count;
    return true;
  }

  static bool dispatch(S& s, uint8_t op) {
    switch (op) {
      case 0x32: return io_restrict_prog(s);
      case 0x33: return repeat(s);
      case 0x34: return io_restrict_pll(s);
      case 0x36: return end_repeat(s);
      case 0x37: return copy(s);
      case 0x38: return not_(s);
      case 0x39: return io_flag_condition(s);
      case 0x3a: return generic_condition(s);
      case 0x3b: return io_mask_or(s);
      case 0x3c: return io_or(s);
      case 0x47: return andn_reg(s);
      case 0x48: return or_reg(s);
      case 0x49: return idx_addr_latched(s);
      case 0x4a: return io_restrict_pll2(s);
      case 0x4b: return pll2(s);
      case 0x4c: return i2c_byte(s);
      case 0x4d: return zm_i2c_byte(s);
      case 0x4e: return zm_i2c(s);
      case 0x4f: return tmds(s);
      case 0x50: return zm_tmds_group(s);
      case 0x51: return cr_idx_adr_latch(s);
      case 0x52: return cr(s);
      case 0x53: return zm_cr(s);
      case 0x54: return zm_cr_group(s);
      case 0x56: return condition_time(s);
      case 0x57: return ltime(s);
      case 0x58: return zm_reg_sequence(s);
      case 0x59: return pll_indirect(s);
      case 0x5a: return zm_reg_indirect(s);
      case 0x5b: return sub_direct(s);
      case 0x5c: return jump(s);
      case 0x5e: return i2c_if(s);
      case 0x5f: return copy_nv_reg(s);
      case 0x62: return zm_index_io(s);
      case 0x63: return compute_mem(s);
      case 0x65: return reset(s);
      case 0x66: return configure_mem(s);
      case 0x67: return configure_clk(s);
      case 0x68: return configure_preinit(s);
      case 0x69: return io(s);
      case 0x6b: return sub(s);
      case 0x6d: return ram_condition(s);
      case 0x6e: return nv_reg(s);
      case 0x6f: return macro(s);
      case 0x71: return done(s);
      case 0x72: return resume(s);
      case 0x73: return strap_condition(s);
      case 0x74: return time(s);
      case 0x75: return condition(s);
      case 0x76: return io_condition(s);
      case 0x77: return zm_reg16(s);
      case 0x78: return index_io(s);
      case 0x79: return pll(s);
      case 0x7a: return zm_reg(s);
      case 0x87: return ram_restrict_pll(s);
      case 0x8c: return one_byte(s);
      case 0x8d: return one_byte(s);
      case 0x8e: return gpio(s);
      case 0x8f: return ram_restrict_zm_reg_group(s);
      case 0x90: return copy_zm_reg(s);
      case 0x91: return zm_reg_group(s);
      case 0x92: return reserved(s);
      case 0x96: return xlat(s);
      case 0x97: return zm_mask_add(s);
      case 0x98: return auxch(s);
      case 0x99: return zm_auxch(s);
      case 0x9a: return i2c_long_if(s);
      case 0xa9: return gpio_ne(s);
      case 0xaa: return reserved(s);
      default: break;
    }
    return s.fail(NvInitError::UnknownOpcode, s.offset_, "unknown opcode");
  }
};

// --- Running -----------------------------------------------------------------------------------------------------------------------------

bool NvInitScript::exec_loop() {
  if (nested_ >= options_.max_depth) return fail(NvInitError::TooDeep, offset_, "scripts nested too deep (recursion?)");
  ++nested_;
  while (offset_) {
    if (result_.steps >= options_.step_budget) return fail(NvInitError::StepBudget, offset_, "step budget exhausted (a loop?)");
    ++result_.steps;
    uint8_t op = 0;
    if (!r8(offset_, &op)) return false;
    if (!NvInitOps::dispatch(*this, op)) return false;
  }
  --nested_;
  return true;
}

bool NvInitScript::run(uint32_t offset) {
  if (!result_.ok()) return false;
  offset_ = offset;
  execute_ = options_.execute ? 1 : 0;
  nested_ = 0;
  repeat_ = repend_ = 0;
  ramcfg_ = 0;
  head_ = options_.head;
  if (offset == 0) return fail(NvInitError::NoTable, 0, "no script");
  return exec_loop();
}

bool NvInitScript::post() {
  const uint32_t first = script_offset(0);
  if (!first) return fail(NvInitError::NoTable, tables_.table, "the image has no init script table");
  for (uint32_t i = 0; i < kMaxScripts; ++i) {
    const uint32_t data = script_offset(i);
    if (!data) break;
    ++result_.scripts;
    if (!run(data)) return false;
  }
  // nouveau: "the vbios parser will run this right after the normal init tables, whereas the binary driver appears to run it later"
  if (tables_.length >= 16 && tables_.unknown_script) {
    if (!run(tables_.unknown_script)) return false;
  }
  return true;
}

}  // namespace nv
}  // namespace hardware
