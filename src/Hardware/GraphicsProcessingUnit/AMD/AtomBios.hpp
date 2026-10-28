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
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

// What the interpreter needs of the card and the machine: the Linux driver's struct card_info (register, memory controller and PLL access, which the driver fills in) plus the time. The
// register space is the card's MMIO register BAR by dword. A discrete card has no memory controller or PLL access through the BIOS (amdgpu's cail_mc_read / cail_pll_read complain and
// return 0), and the host may do the same.
class IAtomHost {
 public:
  virtual uint32_t read_register(uint32_t reg) = 0;
  virtual void write_register(uint32_t reg, uint32_t value) = 0;
  virtual uint32_t read_mc(uint32_t reg) = 0;
  virtual void write_mc(uint32_t reg, uint32_t value) = 0;
  virtual uint32_t read_pll(uint32_t reg) = 0;
  virtual void write_pll(uint32_t reg, uint32_t value) = 0;
  virtual void delay_us(uint32_t microseconds) = 0;
  virtual void delay_ms(uint32_t milliseconds) = 0;
  virtual uint64_t now_us() = 0;

 protected:
  IAtomHost() = default;
  ~IAtomHost() = default;
};

enum class AtomOp : uint8_t {
  None, Move, And, Or, ShiftLeft, ShiftRight, Mul, Div, Add, Sub, SetPort, SetRegBlock, SetFbBase, Compare, Switch, Jump, Test, Delay, CallTable, Repeat, Clear, Nop, Eot, Mask,
  PostCard, Beep, SaveReg, RestoreReg, SetDataBlock, Xor, Shl, Shr, Debug, ProcessDs, Mul32, Div32,
};

// The AtomBIOS interpreter: the byte code in the video BIOS of an AMD card (Radeon, up to the Polaris generation; Vega and later use ATOM firmware tables with the same idea),
// by which the card is set up - POSTed: the memory controller trained, the clocks and PLLs programmed, the display engine's defaults written - and mode setting is done. A
// port of the Linux amdgpu driver's atom.c (Copyright 2008 Advanced Micro Devices, Inc.; MIT license), operation by operation.
//
// The BIOS image holds a ROM header, a table of command tables (small programs, each with a parameter space and a workspace) and a table of data tables. A command table is a
// list of operations whose operands are registers (of the card, through memory-mapped I/O or an indirect I/O program of the BIOS's own), the table's parameters, its workspace,
// the "framebuffer" scratch memory, the BIOS's data tables, immediates, the PLL registers and the memory controller registers, each as a whole dword or a byte or word of it.
//
// What differs from the driver, because the image is untrusted bytes: every read of the image is checked (a read beyond it stops the run with an error rather than reading
// memory), the scratch memory is checked against its size exactly (the driver's test lets a read one dword beyond through), a table that calls tables is limited in depth, the
// run is limited in the number of operations as well as by the driver's rule that a loop which jumps back to the same place for 20 seconds is stuck, and the workspace is on the
// stack rather than allocated. Where the driver prints a complaint and carries on (a parameter or workspace index out of range, PCI or SYSIO register access, an unknown
// indirect I/O method) this carries on the same way.
//
// Not ported: the BIOS name, date, part number and version strings (cosmetic), and the data table parsing the rest of the driver does for modes and clocks; the unimplemented
// operations (repeat, save and restore registers) do nothing, as in the driver.
class AtomBios {
 public:
  static constexpr uint32_t kCmdInit = 0;          // ATOM_CMD_INIT: ASIC_Init, the POST
  static constexpr uint32_t kDefaultScratchBytes = 20 * 1024;
  static constexpr uint32_t kMaxDepth = 16;        // tables calling tables
  static constexpr uint64_t kLoopTimeoutUs = 20ull * 1000 * 1000;   // ATOM_CMD_TIMEOUT_SEC
  static constexpr uint64_t kDefaultMaxOperations = 50ull * 1000 * 1000;

  enum class Status : uint8_t {
    Ok,
    BadImage,        // not an AtomBIOS image (magic numbers, the table pointers, the indirect I/O programs)
    NoTable,         // the command table list has no table with that index
    BadRead,         // the table reads beyond the image
    Stuck,           // a loop that jumps back to the same place for kLoopTimeoutUs
    TooDeep,         // tables calling tables more than kMaxDepth deep
    TooLong,         // more operations than the limit
    CalledTableFailed,
    NoClocks,        // asic_init: the firmware info has no default engine or memory clock
  };
  static const char* status_name(Status status);

  // The BIOS image is not copied: it must stay as long as this object. Check valid() before use.
  AtomBios(IAtomHost& host, ConstByteView image, allocators::IAllocator& allocator);
  bool valid() const { return valid_; }

  // amdgpu_atom_execute_table: runs command table `index` with the given parameters (the table's parameter space; reads and writes of them go to this array). The registers
  // the operations use as state (data block, register block, framebuffer base, I/O mode, the divide and multiply registers) start from zero, as in the driver.
  Status execute_table(uint32_t index, ArrayView<uint32_t> params);
  // amdgpu_atom_asic_init: the POST, command table 0 with the firmware info's default engine and memory clocks as its first two parameters.
  Status asic_init();

  // For callers and tests
  uint32_t cmd_table() const { return cmd_table_; }
  uint32_t data_table() const { return data_table_; }
  uint32_t firmware_revision() const { return version_; }
  uint32_t scratch_bytes() const { return static_cast<uint32_t>(scratch_.size()); }
  uint32_t scratch_word(uint32_t index) const;
  void set_scratch_word(uint32_t index, uint32_t value);
  uint64_t operations() const { return operations_; }          // executed by the last execute_table
  void set_max_operations(uint64_t limit) { max_operations_ = limit; }
  // amdgpu_atom_parse_data_header / _parse_cmd_header: whether the table exists, and where its data starts / its revisions.
  bool data_table_header(uint32_t index, uint16_t* size, uint8_t* frev, uint8_t* crev, uint16_t* data_start);
  bool cmd_table_header(uint32_t index, uint8_t* frev, uint8_t* crev);
  // How many warnings the driver would have printed and carried on from (indexes out of range, unimplemented register spaces, ...).
  uint32_t warnings() const { return warnings_; }

 private:
  struct Exec;
  uint8_t cu8(uint32_t index);
  uint16_t cu16(uint32_t index);
  uint32_t cu32(uint32_t index);
  uint32_t iio_execute(uint32_t base, uint32_t index, uint32_t data);
  void index_iio(uint32_t base);
  Status execute_locked(uint32_t index, ArrayView<uint32_t> params, uint32_t depth);
  uint32_t get_src_int(Exec& e, uint8_t attr, uint32_t* ptr, uint32_t* saved);
  void skip_src_int(uint8_t attr, uint32_t* ptr);
  uint32_t get_src(Exec& e, uint8_t attr, uint32_t* ptr) { return get_src_int(e, attr, ptr, nullptr); }
  bool failed() const { return fail_ != Status::Ok; }
  bool in_image(uint32_t index, uint32_t bytes);
  uint32_t scratch_read(uint32_t word);
  bool scratch_write(uint32_t word, uint32_t value);
  uint32_t get_src_direct(uint8_t align, uint32_t* ptr);
  uint32_t get_dst(Exec& e, int arg, uint8_t attr, uint32_t* ptr, uint32_t* saved);
  void skip_dst(int arg, uint8_t attr, uint32_t* ptr);
  void put_dst(Exec& e, int arg, uint8_t attr, uint32_t* ptr, uint32_t val, uint32_t saved);
  void run_op(Exec& e, AtomOp op, int arg, uint32_t* ptr);

  IAtomHost& host_;
  ConstByteView bios_;
  bool valid_ = false;
  Status fail_ = Status::Ok;   // the first thing that went wrong in the current run: a read beyond the image, the operation limit
  uint32_t cmd_table_ = 0, data_table_ = 0, version_ = 0;
  uint16_t iio_[256] = {};
  ByteBuffer scratch_;
  // the interpreter's registers (struct atom_context)
  uint16_t data_block_ = 0;
  uint32_t fb_base_ = 0;
  uint32_t divmul_[2] = {};
  uint16_t io_attr_ = 0;
  uint16_t reg_block_ = 0;
  uint8_t shift_ = 0;
  bool cs_equal_ = false, cs_above_ = false;
  int io_mode_ = 0;
  uint64_t operations_ = 0, max_operations_ = kDefaultMaxOperations;
  uint32_t warnings_ = 0;
};

}  // namespace hardware
