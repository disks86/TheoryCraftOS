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

// The video BIOS's init scripts ("devinit"): the byte code the x86 VBIOS (or the driver, on a GPU nobody POSTed) runs to bring up the
// memory controller, the clocks and the board's straps. An interpreter of nouveau's nvkm/subdev/bios/init.c (nvbios_exec, nvbios_post):
// the same opcode set, the same "execute" flag rules (a condition that fails turns execution off until INIT_RESUME, INIT_NOT inverts it,
// INIT_I2C_IF / INIT_COMPUTE_MEM / INIT_RESET force it on for their own accesses), the same register address mangling (head / OR / link
// bits on NV50 and later) and the same VGA port mapping (nvkm_rdport/wrport: PRMCIO at 0x601000, PRMVIO at 0x0c0000, + 0x2000 per head).
//
// The image is untrusted input:
//   - every byte of the script and of the tables it points at is read through NvBios::rd08/16/32; a read outside the image stops the run
//     with NvInitError::Truncated (nouveau reads such bytes as 0 and carries on; stopping is the safer choice for a hostile image);
//   - a step budget (opcodes executed, a backwards INIT_JUMP included) and a nesting limit (INIT_SUB, INIT_SUB_DIRECT, INIT_REPEAT nest
//     nvbios_exec) end a looping or recursive script with an error;
//   - an unknown opcode stops the run with an error naming it and its offset (as nouveau does);
//   - every delay goes through INvRegisters::delay_us, each one capped and all of them together bounded;
//   - registers outside BAR0's 16 MiB are neither read nor written.
//
// PLLs, I2C, DisplayPort AUX, GPIO resets and the NV04-era memory init are not done here: they go through INvInitHooks, whose defaults do
// nothing and report "unsupported" (nouveau only warns when a PLL cannot be set, and carries on; so does this).
//
// Dry run: the script is walked with the same execute rules, but nothing is written: every write it would make is reported to the trace
// (INvInitTrace) and remembered, so that a later read of the same register in the run sees the value it would have had. Registers are
// read from the GPU (reads of BAR0 have no side effects on the registers devinit touches); VGA ports, I2C and AUX are not read at all in a
// dry run (an indexed VGA read is a write of the index port), they read back what the run wrote to them, or 0.
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/AMD/Dpcd.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/I2cEdid.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBios.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBiosTables.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"
#include "Hardware/Mmio.hpp"

namespace hardware {
namespace nv {

// --- Byte access to the VGA register mirrors -------------------------------------------------------------------------------------------

// 8-bit access to BAR0 (nvkm_rd08/wr08), which the VGA port mirrors need: PRMCIO (CRTC, attribute, input status) at 0x601000 + port,
// PRMVIO (misc, sequencer, graphics) at 0x0c0000 + port, each + 0x2000 for head 1.
class INvVgaIo {
 public:
  virtual uint8_t read8(uint32_t offset) = 0;
  virtual void write8(uint32_t offset, uint8_t value) = 0;
  // An index/data pair (0x3c4/0x3c5, 0x3ce/0x3cf, 0x3d4/0x3d5): `value` to register `index` behind the index port at `offset`. Two byte
  // writes by default; an implementation without byte writes does both in one access.
  virtual void write_indexed(uint32_t offset, uint8_t index, uint8_t value) {
    write8(offset, index);
    write8(offset + 1, value);
  }

 protected:
  INvVgaIo() = default;
  ~INvVgaIo() = default;
};

// The real thing: byte accesses of the mapped BAR0 (the window is checked first; outside it reads 0xff and writes nothing).
class MmioNvVgaIo final : public INvVgaIo {
 public:
  explicit MmioNvVgaIo(const MmioRegion& bar0) : bar0_(bar0) {}
  uint8_t read8(uint32_t offset) override { return offset < bar0_.length() ? bar0_.read8(offset) : 0xff; }
  void write8(uint32_t offset, uint8_t value) override { if (offset < bar0_.length()) bar0_.write8(offset, value); }

 private:
  MmioRegion bar0_;
};

// Over the 32-bit INvRegisters (the register models of the tests, or a BAR0 that is only reachable with 32-bit accesses): a byte read is
// the byte of the aligned word, a byte write a read-modify-write of it, and an index/data pair one 32-bit write carrying both bytes (so that
// the data byte goes with the new index, not the old one). The neighbouring bytes are written back as read: right for a register model,
// unverified on hardware (where reading a VGA data port has no side effects but writing it back does rewrite the register).
class RegistersNvVgaIo final : public INvVgaIo {
 public:
  explicit RegistersNvVgaIo(INvRegisters& registers) : registers_(registers) {}
  uint8_t read8(uint32_t offset) override;
  void write8(uint32_t offset, uint8_t value) override;
  void write_indexed(uint32_t offset, uint8_t index, uint8_t value) override;

 private:
  INvRegisters& registers_;
};

// --- Hooks: what the interpreter cannot do itself --------------------------------------------------------------------------------------

// The special I2C bus indices (init_i2c): the board's primary and secondary default ports (the DCB I2C table's defaults). A script's 0xff
// means the output's default (primary, secondary when NvInitOutput::i2c_upper_default), 0x80 primary, 0x81 secondary; anything else is
// an index into the DCB I2C table.
constexpr uint8_t kInitI2cPrimary = 0x80, kInitI2cSecondary = 0x81;
// INIT_GPIO resets every GPIO function of the board (nvkm_gpio_reset with DCB_GPIO_UNUSED).
constexpr uint8_t kInitGpioAll = 0xff;

class INvInitHooks {
 public:
  // nvkm_devinit_pll_set: program PLL `type` (a register address, or a BIOS PLL type below 0x100) to `khz`. False: not done (the run goes
  // on, as nouveau's does, and the result counts it).
  [[nodiscard]] virtual bool set_pll(uint32_t type, uint32_t khz) { (void)type; (void)khz; return false; }
  // The I2C bus for `index` (a DCB I2C table index, kInitI2cPrimary or kInitI2cSecondary): NvI2cBus or NvAuxChannel (I2C over AUX) fit.
  // nullptr: no such bus (the opcode's accesses fail as nouveau's do with -ENODEV).
  virtual II2cBus* i2c_bus(uint8_t index) { (void)index; return nullptr; }
  // The DisplayPort AUX channel of the output's I2C index (init_aux: nvkm_i2c_aux_find(outp->i2c_index)); NvAuxChannel fits.
  virtual IDpcdAux* aux_channel(uint8_t i2c_index) { (void)i2c_index; return nullptr; }
  // nvkm_gpio_reset: put the GPIO lines of DCB function `function` (kInitGpioAll: every function) in their default state.
  [[nodiscard]] virtual bool gpio_reset(uint8_t function) { (void)function; return false; }
  // nvkm_devinit_meminit (INIT_COMPUTE_MEM): only NV04-era boards have one; NV50 and later do nothing.
  [[nodiscard]] virtual bool meminit() { return false; }

 protected:
  INvInitHooks() = default;
  ~INvInitHooks() = default;
};

// The hooks with nothing behind them.
class NullNvInitHooks final : public INvInitHooks {};

// --- Tracing -----------------------------------------------------------------------------------------------------------------------------

enum class NvInitAccess : uint8_t {
  Mmio,        // address = BAR0 offset, value = 32 bits
  VgaPort,     // address = BAR0 offset of the port (after the head mapping), value = 8 bits
  VgaIndexed,  // address = BAR0 offset of the index port, index, value = 8 bits
  Pll,         // address = PLL type, value = kHz
  I2c,         // address = bus index, index = (7-bit address << 8 | register), value = byte (ZM_I2C: the first byte, index = byte count)
  Aux,         // address = DPCD address, value = byte
  Gpio,        // address = function
  MemInit,
  Delay,       // value = microseconds (after capping)
};
const char* init_access_name(NvInitAccess access);

struct NvInitWrite {
  NvInitAccess kind = NvInitAccess::Mmio;
  uint32_t address = 0;
  uint32_t index = 0;
  uint32_t value = 0;
  uint32_t script_offset = 0;   // the opcode that made it
};

class INvInitTrace {
 public:
  // Every write (or, in a dry run, every write that would be made), every hook call and every delay, in order.
  virtual void on_write(const NvInitWrite& write) = 0;

 protected:
  INvInitTrace() = default;
  ~INvInitTrace() = default;
};

// --- The run -----------------------------------------------------------------------------------------------------------------------------

// The display output a script is run for (nouveau's init->outp, init->or, init->link, init->head). Post scripts run without one.
struct NvInitOutput {
  bool present = false;
  uint8_t or_mask = 0;               // dcb_output.or (INIT_TMDS indexes by the mask itself)
  uint8_t i2c_index = 0;             // dcb_output.i2c_index (the AUX channel of INIT_AUXCH)
  bool i2c_upper_default = false;
  uint8_t connector_type = 0xff;     // the connector table entry's type (DCB_CONNECTOR_*; 0x47 eDP), 0xff unknown
  bool has_dp_info = false;          // the DP output table matched this output (INIT_GENERIC_CONDITION 1/2)
  uint8_t dp_info_flags = 0;         // nvbios_dpout.flags
};

struct NvInitOptions {
  static constexpr uint32_t kDefaultStepBudget = 1000000;
  static constexpr uint32_t kDefaultMaxDepth = 16;
  static constexpr uint32_t kDefaultMaxDelayUs = 100000;          // one delay (INIT_TIME, INIT_LTIME, a poll of INIT_CONDITION_TIME)
  static constexpr uint64_t kDefaultTotalDelayUs = 10000000;      // all of them in one run

  bool dry_run = false;
  bool execute = true;               // nvbios_post's `execute`: false walks the scripts without executing anything (nouveau's parse mode)
  uint32_t step_budget = kDefaultStepBudget;
  uint32_t max_depth = kDefaultMaxDepth;
  uint32_t max_delay_us = kDefaultMaxDelayUs;
  uint64_t total_delay_us = kDefaultTotalDelayUs;
  int head = -1, or_index = -1;      // init->head, init->or (-1: none)
  uint8_t link = 0;                  // init->link: 0 none, 1 link A, 2 link B
  NvInitOutput output;
  // gt215_devinit_mmio: on GT215/GT216/GT218 skip the memory partition registers of partitions the board does not have (0x001540).
  bool gt215_partition_filter = false;
  // The board's tables (parse_board): INIT_GPIO_NE walks its GPIO entries. nullptr: no GPIO entries.
  const BoardInfo* board = nullptr;
};

enum class NvInitError : uint8_t {
  None,
  NoTable,          // no BIT 'I' (and no BMP 5.10+) init table, or the requested pointer is missing / too short
  Truncated,        // a read of the script or a table outside the image
  UnknownOpcode,
  StepBudget,       // more opcodes than the budget: a loop
  TooDeep,          // nesting beyond max_depth: recursion
  DelayBudget,      // the delays of the run add up past total_delay_us
  Unsupported,      // an opcode whose data this does not have (INIT_CONFIGURE_MEM/CLK on BIOS major <= 2: the BMP memory tables)
};
const char* init_error_name(NvInitError error);

struct NvInitResult {
  NvInitError error = NvInitError::None;
  uint32_t error_offset = 0;         // where it stopped (the opcode, or the byte that could not be read)
  uint8_t opcode = 0;                // the opcode at error_offset (UnknownOpcode, StepBudget ...)
  const char* message = "";
  uint32_t scripts = 0;              // scripts started from the script table (post)
  uint32_t steps = 0;                // opcodes executed
  uint32_t mmio_writes = 0, vga_writes = 0;
  uint32_t plls_unsupported = 0, i2c_failures = 0, aux_failures = 0, gpio_unsupported = 0, meminit_unsupported = 0;
  uint32_t warnings = 0;             // nouveau's warn/error lines that do not stop it (missing head/OR, unknown register bits ...)
  uint64_t delay_us = 0;             // the delays made (capped)
  bool shadow_full = false;          // (dry run) more distinct registers written than remembered: later reads may see the GPU's values
  bool ok() const { return error == NvInitError::None; }
};

// The pointers of the init table (BIT 'I' version 1; BMP 5.10+ at bmp + 75). 0 = absent.
struct NvInitTables {
  uint32_t table = 0, length = 0;    // the BIT 'I' data
  uint16_t script_table = 0;         // +0x00
  uint16_t macro_index_table = 0;    // +0x02
  uint16_t macro_table = 0;          // +0x04
  uint16_t condition_table = 0;      // +0x06
  uint16_t io_condition_table = 0;   // +0x08
  uint16_t io_flag_condition_table = 0;  // +0x0a
  uint16_t function_table = 0;       // +0x0c
  uint16_t unknown_script = 0;       // +0x0e (run after the script table; nouveau's init_unknown_script)
  uint16_t xlat_table = 0;           // +0x10
};
[[nodiscard]] NvInitTables find_init_tables(const NvBios& bios);

class NvInitScript {
 public:
  // `vga`, `hooks` and `trace` may be nullptr (no VGA access: ports read 0; NullNvInitHooks; no trace). All must outlive the object.
  NvInitScript(const NvBios& bios, const Chip& chip, INvRegisters& registers, INvVgaIo* vga, INvInitHooks* hooks, INvInitTrace* trace,
               const NvInitOptions& options);

  // nvbios_init + nvbios_exec: the script at `offset` with a fresh execute state. False on an error (result() says which).
  [[nodiscard]] bool run(uint32_t offset);
  // nvbios_post: the script table's scripts in order (until a 0 pointer or the table's end), then the "unknown" script at +0x0e.
  [[nodiscard]] bool post();
  // The script table's entry `index` (init_script; BMP < 5.10 boards have two at bmp + 14 / 18). 0 if there is none.
  [[nodiscard]] uint32_t script_offset(uint32_t index) const;

  const NvInitResult& result() const { return result_; }
  const NvInitTables& tables() const { return tables_; }
  // The image's BIOS major version (BIT 'i' byte 3, or the BMP's): some opcodes depend on it.
  uint8_t bios_major() const { return bios_major_; }

  static constexpr uint32_t kMaxScripts = 256;
  static constexpr uint32_t kShadowEntries = 256;
  static constexpr uint32_t kBar0Bytes = 16u << 20;

 private:
  friend struct NvInitOps;

  // the execute flag (init_exec*)
  bool exec() const { return execute_ == 1 || (execute_ & 5) == 5; }
  void exec_set(bool on) { if (on) execute_ &= 0xfd; else execute_ |= 0x02; }
  void exec_inv() { execute_ ^= 0x02; }
  void exec_force(bool on) { if (on) execute_ |= 0x04; else execute_ &= 0xfb; }

  [[nodiscard]] bool exec_loop();
  [[nodiscard]] bool fail(NvInitError error, uint32_t at, const char* message);
  [[nodiscard]] bool r8(uint64_t at, uint8_t* out);
  [[nodiscard]] bool r16(uint64_t at, uint16_t* out);
  [[nodiscard]] bool r32(uint64_t at, uint32_t* out);
  [[nodiscard]] bool delay(uint64_t us);

  // registers (init_nvreg, init_rd32, init_wr32, init_mask): false = skip
  bool nvreg(uint32_t* reg);
  uint32_t rd32(uint32_t reg);
  void wr32(uint32_t reg, uint32_t value);
  uint32_t mask(uint32_t reg, uint32_t clear, uint32_t set);
  uint32_t raw_read32(uint32_t reg);   // the GPU (or the dry run's shadow)
  void raw_write32(uint32_t reg, uint32_t value);
  // VGA (init_rdport, init_wrport, init_rdvgai, init_wrvgai)
  uint8_t rdport(uint16_t port);
  void wrport(uint16_t port, uint8_t value);
  uint8_t rdvgai(uint16_t port, uint8_t index);
  void wrvgai(uint16_t port, uint8_t index, uint8_t value);
  uint32_t port_offset(int head, uint16_t port) const;   // 0: not a mapped port
  // I2C / AUX / PLL
  II2cBus* i2c(uint8_t index);
  int rdi2cr(uint8_t index, uint8_t address, uint8_t reg);
  int wri2cr(uint8_t index, uint8_t address, uint8_t reg, uint8_t value);
  uint8_t rdauxr(uint32_t address);
  void wrauxr(uint32_t address, uint8_t value);
  void prog_pll(uint32_t type, uint32_t khz);
  // context
  int init_or();
  int init_link();
  int init_head();
  uint8_t init_conn();
  // tables
  [[nodiscard]] bool condition_met(uint8_t cond, bool* met);
  [[nodiscard]] bool io_condition_met(uint8_t cond, bool* met);
  [[nodiscard]] bool io_flag_condition_met(uint8_t cond, bool* met);
  uint8_t ram_restrict_group_count();
  uint8_t ram_restrict();
  uint8_t ramcfg_index();
  [[nodiscard]] bool xlat(uint8_t index, uint8_t offset, uint8_t* out);
  uint32_t tmds_reg(uint8_t tmds);
  void trace(NvInitAccess kind, uint32_t address, uint32_t index, uint32_t value);
  void warn() { ++result_.warnings; }

  struct Shadow {
    uint8_t kind = 0;   // 0 free, 1 mmio, 2 vga port, 3 vga indexed
    uint32_t key = 0;
    uint32_t value = 0;
  };
  bool shadow_get(uint8_t kind, uint32_t key, uint32_t* value) const;
  void shadow_put(uint8_t kind, uint32_t key, uint32_t value);

  const NvBios& bios_;
  Chip chip_;
  INvRegisters& regs_;
  INvVgaIo* vga_;
  INvInitHooks* hooks_;
  NullNvInitHooks null_hooks_;
  INvInitTrace* trace_;
  NvInitOptions options_;
  NvInitTables tables_;
  uint8_t bios_major_ = 0;
  NvInitResult result_;

  // nvbios_init's state
  uint32_t offset_ = 0;
  uint8_t execute_ = 1;
  uint32_t nested_ = 0;
  uint32_t repeat_ = 0, repend_ = 0;
  uint32_t ramcfg_ = 0;
  int head_ = -1;
  uint32_t r001540_ = 0;
  bool r001540_read_ = false;
  Shadow shadow_[kShadowEntries];
};

}  // namespace nv
}  // namespace hardware
