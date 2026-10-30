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

// The LLVM bitcode a DXIL shader (Shader Model 6) is made of: a bitstream reader and the parts of an LLVM 3.7 module the translator (DxilTranslate.cpp) needs - types, constants, the
// declarations of the dx.op functions, the function bodies as instructions with their operands resolved to value numbers, and the metadata that describes the entry point and its resources.
// Everything is checked: this is read from a shader a process hands the kernel.
#pragma once
#include "Kernel/Dxgkrnl/DxgTypes.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"

namespace dxgk {
namespace dxil {

// --- The bitstream --------------------------------------------------------------------------------------------------------------------------------------------------

// Receives the blocks and records of a bitstream, in order. A false return stops the parse (and fails it).
class BitstreamVisitor {
 public:
  virtual bool enter_block(uint32_t block_id) = 0;
  virtual bool record(uint32_t block_id, uint32_t code, ArrayView<const uint64_t> operands, ConstByteView blob) = 0;
  virtual bool exit_block(uint32_t block_id) = 0;

 protected:
  BitstreamVisitor() = default;
  ~BitstreamVisitor() = default;
};

// Parses `bytes` (starting at the 'BC' 0xC0DE magic) into the visitor. kInvalidParameter for anything that is not well formed bitcode.
NtStatus parse_bitstream(allocators::IAllocator& allocator, ConstByteView bytes, BitstreamVisitor& visitor);

// --- The module -----------------------------------------------------------------------------------------------------------------------------------------------------

enum class TypeKind : uint8_t { Void, Int, Half, Float, Double, Pointer, Struct, Array, Vector, Function, Label, Metadata, Other };

struct Type {
  TypeKind kind = TypeKind::Other;
  uint32_t width = 0;       // Int: bits
  uint32_t count = 0;       // Struct: elements; Array / Vector: length; Function: parameters
  uint32_t first = 0;       // Struct / Function: the index of the first element or parameter type in Module::type_refs; Array / Vector / Pointer: the element type
  uint32_t result = 0;      // Function: the return type
};

// A constant: an integer (Int, up to 64 bits), a float (the bits of a float), undef/null, or an aggregate (elements in value numbers).
enum class ConstantKind : uint8_t { None, Int, Float, Null, Undef, Aggregate, Gep, Other };   // Gep: a constant expression getelementptr of a global: bits = the global's value number | the last index's value number << 32
struct Constant {
  ConstantKind kind = ConstantKind::None;
  uint64_t bits = 0;
  uint32_t type = 0;
};

enum class Op : uint8_t {
  Binop, Cast, Cmp, Select, Call, Br, CondBr, Ret, Phi, ExtractValue, Load, Store, Alloca, Gep, Unreachable, Switch, Other
};

constexpr uint32_t kMaxCallArguments = 20;
struct Instruction {
  Op op = Op::Other;
  uint32_t code = 0;            // Binop: the LLVM binary operator (0 add, 1 sub, 2 mul, 3 udiv, 4 sdiv, 5 urem, 6 srem, 7 shl, 8 lshr, 9 ashr, 10 and, 11 or, 12 xor); Cast: the cast opcode; Cmp: the predicate
  uint32_t type = 0;            // the type of the result (a Call to a void function: the void type)
  uint32_t result = 0;          // the value number of the result; ~0 for none
  uint32_t operands[3] = {0, 0, 0};   // value numbers (Binop / Cmp: lhs, rhs; Cast: value; Select: true, false, condition; ExtractValue: aggregate; Ret: value; CondBr: condition; Call: the callee)
  uint32_t blocks[2] = {0, 0};  // Br: the target; CondBr: true, false
  uint32_t index = 0;           // ExtractValue: the element
  uint32_t arg_count = 0;       // Call
  uint32_t args[kMaxCallArguments] = {};
  uint32_t phi_first = 0, phi_count = 0;   // Phi: the range in Function::phi_values / phi_blocks
  bool has_value = false;
};

// A global variable: its value number, its type (the value type) and address space (3 is group-shared memory).
struct Global {
  uint32_t value = 0, type = 0, address_space = 0;
};

struct Function {
  explicit Function(allocators::IAllocator& allocator) : name(allocator) {}
  uint32_t local_first = 0, local_count = 0;   // its values above Module::module_values (arguments, constants, results): their range in Module::local_types / local_constants, by value number - module_values
  uint32_t value = 0;           // the function's own value number
  uint32_t type = 0;            // its function type
  bool declaration = true;
  uint32_t first_instruction = 0, instruction_count = 0;   // in Module::instructions
  uint32_t block_count = 0;
  uint32_t first_value = 0;     // the value number of its first argument; its arguments, constants and instruction results follow
  uint32_t first_block_start = 0;
  String name;
};

// A block of a function body: the range of its instructions in Module::instructions.
struct Block {
  uint32_t first = 0, count = 0;
};

// A metadata node: strings, values (constants), and tuples of other nodes.
enum class MetaKind : uint8_t { None, String, Value, Node };
struct Meta {
  MetaKind kind = MetaKind::None;
  uint32_t value = 0;           // Value: the value number; String: the index in Module::strings
  uint32_t first = 0, count = 0;   // Node: the range of operand ids (+1; 0 is null) in Module::meta_refs
};

struct NamedMeta {
  explicit NamedMeta(allocators::IAllocator& allocator) : name(allocator) {}
  String name;
  uint32_t first = 0, count = 0;   // the nodes it names: a range of Module::meta_refs (ids, without the +1)
};

class Module {
 public:
  explicit Module(allocators::IAllocator& allocator)
      : types(allocator), type_refs(allocator), constants(allocator), value_types(allocator), functions(allocator), instructions(allocator), blocks(allocator), phi_values(allocator), phi_blocks(allocator),
        meta(allocator), meta_refs(allocator), strings(allocator), named(allocator), globals(allocator), local_types(allocator), local_constants(allocator), allocator_(allocator) {}
  allocators::IAllocator& allocator() const { return allocator_; }

  List<Type> types;
  List<uint32_t> type_refs;
  List<Constant> constants;         // by value number (kind None for a value that is not a constant)
  List<uint32_t> value_types;       // the type of every module-level value (globals, functions, constants), then those of the function being read
  List<Function> functions;
  List<Instruction> instructions;
  List<Block> blocks;               // all functions' blocks, in order; Function::first_block_start indexes this
  List<uint32_t> phi_values;        // phi incoming value numbers
  List<uint32_t> phi_blocks;        // ... and their blocks
  List<Meta> meta;
  List<uint32_t> meta_refs;
  List<String> strings;
  List<NamedMeta> named;
  List<Global> globals;
  List<uint32_t> local_types;       // the types and constants of the function-local values of every function (see Function::local_first)
  List<Constant> local_constants;
  uint32_t module_values = 0;       // the value numbers below this belong to the module (globals, functions, module constants)

  // The function bodies' results are in numbers above module_values per function: Function::first_value is where a function's start (its arguments).
  const Type* type_at(uint32_t id) const { return id < types.count() ? &types[id] : nullptr; }
  bool is_float(uint32_t type) const { return type < types.count() && types[type].kind == TypeKind::Float; }
  bool is_int(uint32_t type, uint32_t width = 0) const { return type < types.count() && types[type].kind == TypeKind::Int && (width == 0 || types[type].width == width); }
  const NamedMeta* find_named(StringView name) const {
    for (const NamedMeta& n : named.view())
      if (n.name.view() == name) return &n;
    return nullptr;
  }

 private:
  allocators::IAllocator& allocator_;
};

// Reads a module out of the bitcode (kInvalidParameter for a bad one, kNotSupported for a good one that uses what is not read here).
NtStatus read_module(allocators::IAllocator& allocator, ConstByteView bitcode, Module* out);

}  // namespace dxil
}  // namespace dxgk
