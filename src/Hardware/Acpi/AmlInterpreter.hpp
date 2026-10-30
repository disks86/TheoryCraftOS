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

// An interpreter for ACPI Machine Language (AML), the bytecode of the DSDT and SSDT tables, enough to evaluate the methods the firmware offers for
// reading devices (a battery's _STA, _BST and _BIF, the embedded controller's _CRS, ...). It is freestanding: no exceptions, no RTTI, and every read of
// the code is bounds-checked, because the tables are the firmware's. Loading builds the namespace of the tables (devices, names, methods, operation
// regions and fields); evaluating runs a method or reads a named value. Reading and writing the operation regions (the embedded controller, I/O ports,
// memory, PCI configuration space) is left to an IAmlHost.
//
// Not supported (a method that uses it fails, nothing else is affected): Load/LoadTable, BankField, Match, Wait/Signal events, ConcatenateResTemplate,
// and the string conversions. Evaluation is bounded: a method call is cut off after a fixed number of operations and nesting depth.
#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"

namespace acpi {

enum class RegionSpace : uint8_t { SystemMemory = 0, SystemIo = 1, PciConfig = 2, EmbeddedControl = 3, SmBus = 4, SystemCmos = 5, PciBarTarget = 6, Ipmi = 7 };

// One access to an operation region, as the host sees it.
struct RegionAccess {
  RegionSpace space;
  uint64_t address;       // region base + offset of the access, in bytes
  uint32_t width_bits;    // 8, 16, 32 or 64
  uint8_t pci_bus, pci_device, pci_function;   // PciConfig only
};

struct Node;

class IAmlHost {
 public:
  virtual bool region_read(const RegionAccess& access, uint64_t* value) = 0;
  virtual bool region_write(const RegionAccess& access, uint64_t value) = 0;
  virtual void stall_us(uint32_t us) { (void)us; }
  virtual void sleep_ms(uint32_t ms) { (void)ms; }
  virtual uint64_t timer_100ns() { return 0; }
  // AML executed Notify(device, value): the firmware tells the OS something happened to the device (2: the device woke the system, 0 and 1: bus check, 0x80 and up: the device's own events).
  virtual void notify(Node* device, uint32_t value) { (void)device; (void)value; }

 protected:
  constexpr IAmlHost() = default;
  ~IAmlHost() = default;
};

enum class ValueKind : uint8_t {
  Uninitialized,
  Integer,
  String,       // bytes, length (not counting the terminating zero)
  Buffer,       // bytes, length
  Package,      // elements, length = element count
  Reference,    // slot: another value (a package element, a local variable)
  BufferByte,   // bytes: one byte of a buffer or string (from Index)
  Object        // node: a device, mutex, region or other named object
};

// A value of the interpreter. A String, Buffer or Package value shares its contents with the value it was copied from; the interpreter copies when AML
// stores. Valid until the next evaluate().
struct Value {
  ValueKind kind = ValueKind::Uninitialized;
  uint32_t length = 0;
  uint64_t integer = 0;
  union {
    uint8_t* bytes;
    Value* elements;
    Value* slot;
    Node* node;
  };
  Value() : bytes(nullptr) {}
};

constexpr uint32_t kMaxArguments = 7;

class AmlInterpreter {
 public:
  AmlInterpreter(allocators::IAllocator& allocator, IAmlHost& host);
  ~AmlInterpreter();
  AmlInterpreter(const AmlInterpreter&) = delete;
  AmlInterpreter& operator=(const AmlInterpreter&) = delete;

  // Adds the definition block of a table (the whole table, header included; DSDT or SSDT) to the namespace. `table` must stay valid for as long as the
  // interpreter is used. False if the block was malformed (what was loaded before the fault stays).
  bool load_table(ConstByteView table);
  // The namespace node at an absolute path such as "\\_SB.PCI0.BAT0" (or a name relative to `scope`, searched upwards like AML does), or nullptr.
  Node* find(const char* path, Node* scope = nullptr) const;
  Node* root() const;

  // Reads the named value or runs the method. `result` (may be nullptr) is valid until the next evaluate().
  bool evaluate(Node* node, const Value* arguments, uint32_t argument_count, Value* result);
  bool evaluate(const char* path, Value* result) { Node* n = find(path); return n && evaluate(n, nullptr, 0, result); }
  // Evaluate and take an integer (a buffer of up to 8 bytes counts).
  bool evaluate_integer(Node* node, uint64_t* out);
  bool evaluate_integer(const char* path, uint64_t* out) { Node* n = find(path); return n && evaluate_integer(n, out); }

  // --- Walking the namespace.
  Node* first_child(const Node* node) const;
  Node* next_sibling(const Node* node) const;
  Node* parent(const Node* node) const;
  bool is_device(const Node* node) const;
  bool is_method(const Node* node) const;
  bool is_thermal_zone(const Node* node) const;
  bool is_region(const Node* node) const;
  // The four-character name of a node (not zero-terminated).
  const char* name_of(const Node* node) const;
  RegionSpace region_space(const Node* node) const;
  // The region the field `node` is in, or nullptr.
  Node* region_of_field(const Node* node) const;
  // Does a device's _HID (or one of its _CID) match this ID, "PNP0C0A"?
  bool device_has_id(Node* device, const char* id);

  // True if evaluate() had to give up on the last call (unsupported operation, bad code, budget used up).
  bool failed() const { return failed_; }
  const char* last_error() const { return error_; }
  uint32_t node_count() const { return node_count_; }

 private:
  struct Impl;
  Impl* impl_ = nullptr;
  bool failed_ = false;
  const char* error_ = "";
  uint32_t node_count_ = 0;
};

// An EISA ID (the integer form of a _HID) as text such as "PNP0C0A"; `out` holds 8 bytes.
void eisa_id_to_string(uint32_t id, char out[8]);

}  // namespace acpi
