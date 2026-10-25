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

// A WebAssembly runtime that does not know about the script engine: the binary decoder and validator (Module::parse), instantiation with linking (Store::instantiate)
// and an interpreter (Store::invoke). Supported: the 1.0 core, sign extension, saturating float conversions, multi-value, reference types, bulk memory and tables,
// fixed-width and relaxed SIMD, tail calls, extended constants and multiple memories. Not supported: threads and atomics, exception handling, garbage collection
// types, 64-bit memories (a module that uses them fails to validate).
//
// Function bodies are checked and translated in one pass into a flat instruction array with resolved branch targets (see Instr), which the interpreter runs over a
// stack of 16 byte slots. Everything a Store makes (modules, memories, tables, globals, functions, instances) lives until the Store does.
#pragma once
#include <stdint.h>
#include <new>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/SharedPointer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace wasm {

enum class ValType : uint8_t { I32 = 0x7F, I64 = 0x7E, F32 = 0x7D, F64 = 0x7C, V128 = 0x7B, FuncRef = 0x70, ExternRef = 0x6F };

inline bool is_ref_type(ValType t) { return t == ValType::FuncRef || t == ValType::ExternRef; }

// One value on the operand stack. i32 / f32 are in the low 32 bits of `lo`, i64 / f64 in `lo`, a v128 in both halves (lane 0 is in the low byte of `lo`), a reference
// in `lo` (0 is null; a funcref is a FuncInst*, an externref is whatever number the host made).
struct Slot {
  uint64_t lo = 0, hi = 0;
};

constexpr uint32_t kMaxParams = 1000, kMaxResults = 1000;
constexpr uint32_t kPageSize = 65536, kMaxPages = 65536, kMaxTableSize = 10000000;   // (what a module may declare)
constexpr uint32_t kMaxTableElements = 1u << 22;                                       // (what this runtime makes room for: 32 MiB in a table)

// A function type. The types of the parameters and results are kept in one allocation that `params` and `results` look into.
struct FuncSig {
  explicit FuncSig(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~FuncSig() { allocator_.deallocate(storage_); }
  FuncSig(const FuncSig&) = delete;
  FuncSig& operator=(const FuncSig&) = delete;
  // Makes room for `np` parameters and `nr` results (all i32 for now). False if there is no memory or the counts are above the limits.
  bool init(uint32_t np, uint32_t nr);
  bool assign(const FuncSig& other);
  bool equals(const FuncSig& o) const;
  uint16_t nparams = 0, nresults = 0;
  ArrayView<ValType> params, results;

 private:
  allocators::IAllocator& allocator_;
  ValType* storage_ = nullptr;
};

// An array of n values that lives on the stack when n is small and in memory from the allocator when it is not (a function type may have a thousand parameters).
template <class T, uint32_t kInline>
class SmallArray {
 public:
  SmallArray(allocators::IAllocator& allocator, uint32_t n) : allocator_(allocator) {
    if (n <= kInline) {
      data_ = inline_;
    } else {
      heap_ = static_cast<T*>(allocator.allocate(sizeof(T) * uint64_t{n}));
      if (heap_)
        for (uint32_t i = 0; i < n; ++i) new (&heap_[i]) T();
      data_ = heap_;
    }
  }
  ~SmallArray() { allocator_.deallocate(heap_); }
  SmallArray(const SmallArray&) = delete;
  SmallArray& operator=(const SmallArray&) = delete;
  bool ok() const { return data_ != nullptr; }
  T* data() const { return data_; }
  T& operator[](uint32_t i) const { return data_[i]; }   // (the caller made it n big)

 private:
  allocators::IAllocator& allocator_;
  T inline_[kInline];
  T* heap_ = nullptr;
  T* data_ = nullptr;
};

struct Limits {
  uint32_t min = 0, max = 0;
  bool has_max = false;
  bool shared = false;
};
struct TableType {
  ValType elem = ValType::FuncRef;
  Limits limits;
};
struct MemType {
  Limits limits;   // in pages
};
struct GlobalType {
  ValType type = ValType::I32;
  bool mut = false;
};

enum class ExternKind : uint8_t { Func = 0, Table = 1, Memory = 2, Global = 3 };

// A constant expression (the value of a global, the offset of a segment, an element): a few operations over a tiny stack.
enum class ConstOpKind : uint8_t { I32Const, I64Const, F32Const, F64Const, V128Const, GlobalGet, RefNull, RefFunc, I32Add, I32Sub, I32Mul, I64Add, I64Sub, I64Mul };
struct ConstOp {
  ConstOpKind op = ConstOpKind::I32Const;
  uint64_t a = 0, b = 0;
};
struct ConstExpr {
  explicit ConstExpr(allocators::IAllocator& allocator) : ops(allocator) {}
  List<ConstOp> ops;
  ValType type = ValType::I32;
};

// An instruction of the translated code. `op` is the WebAssembly opcode (0xFC xx is 0x100 + xx, 0xFD xx is 0x200 + xx) or one of the internal operations below.
struct Instr {
  uint16_t op = 0;
  uint16_t aux = 0;   // a memory index
  uint32_t a = 0;
  uint64_t b = 0;
};

// Internal operations (no WebAssembly opcode is above 0x2FF).
enum InternalOp : uint16_t { kOpJmp = 0x400, kOpJmpIfZero, kOpBr, kOpBrIf, kOpBrTable, kOpReturn, kOpMulti };

struct BrTarget {
  uint32_t pc = 0;
  uint32_t height = 0;   // the slot (counted from the frame's first local) the values are moved to
  uint32_t arity = 0;
};

struct FuncCode {
  explicit FuncCode(allocators::IAllocator& allocator) : code(allocator), targets(allocator) {}
  uint32_t type_index = 0;
  uint32_t nparams = 0, nresults = 0;
  uint32_t nlocals = 0;     // the parameters and the declared locals
  uint32_t max_stack = 0;   // operands above the locals
  List<Instr> code;
  List<BrTarget> targets;
};

struct Import {
  explicit Import(allocators::IAllocator& allocator) : module(allocator), name(allocator) {}
  String module, name;
  ExternKind kind = ExternKind::Func;
  uint32_t type_index = 0;
  TableType table;
  MemType memory;
  GlobalType global;
};

struct Export {
  explicit Export(allocators::IAllocator& allocator) : name(allocator) {}
  String name;
  ExternKind kind = ExternKind::Func;
  uint32_t index = 0;
};

struct GlobalDef {
  explicit GlobalDef(allocators::IAllocator& allocator) : init(allocator) {}
  GlobalType type;
  ConstExpr init;
};

enum class SegmentMode : uint8_t { Active, Passive, Declarative };

struct ElemSegment {
  explicit ElemSegment(allocators::IAllocator& allocator) : offset(allocator), items(allocator) {}
  SegmentMode mode = SegmentMode::Active;
  ValType type = ValType::FuncRef;
  uint32_t table = 0;
  ConstExpr offset;
  List<UniquePointer<ConstExpr>> items;
};

struct DataSegment {
  explicit DataSegment(allocators::IAllocator& allocator) : offset(allocator), bytes(allocator) {}
  SegmentMode mode = SegmentMode::Active;
  uint32_t memory = 0;
  ConstExpr offset;
  ByteBuffer bytes;
};

struct CustomSection {
  explicit CustomSection(allocators::IAllocator& allocator) : name(allocator), bytes(allocator) {}
  String name;
  ByteBuffer bytes;
};

class Module {
 public:
  explicit Module(allocators::IAllocator& allocator);
  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;

  // Decodes and validates a binary module. On failure `*error` says why (and where, as a byte offset).
  bool parse(ConstByteView bytes, String* error);

  allocators::IAllocator& allocator() { return allocator_; }

  List<UniquePointer<FuncSig>> types;
  List<UniquePointer<Import>> imports;
  List<uint32_t> func_types;          // the type of every function, the imported ones first
  List<TableType> tables;             // every table, the imported ones first
  List<MemType> memories;
  List<GlobalType> globals;
  uint32_t imported_funcs = 0, imported_tables = 0, imported_memories = 0, imported_globals = 0;
  List<UniquePointer<GlobalDef>> global_defs;   // the globals the module defines
  List<UniquePointer<Export>> exports;
  List<UniquePointer<ElemSegment>> elems;
  List<UniquePointer<DataSegment>> datas;
  List<UniquePointer<FuncCode>> codes;          // the functions the module defines
  List<UniquePointer<CustomSection>> customs;
  bool has_start = false;
  uint32_t start = 0;

 private:
  allocators::IAllocator& allocator_;
};

// --- Runtime objects

class Store;
struct Instance;

struct Memory {
  uint8_t* data = nullptr;
  uint64_t size = 0;                 // bytes
  uint32_t max_pages = kMaxPages;
  bool has_max = false;
  bool shared = false;
  // Where the bytes live. The default (null) is memory from the Store's allocator; a host that keeps them elsewhere (script engine buffers) installs `resize`, which
  // makes the memory `new_size` bytes big (the old contents kept, the rest zero) by changing data / size, or returns false.
  bool (*resize)(Memory& memory, uint64_t new_size) = nullptr;
  void* host = nullptr;
  uint32_t host_id = 0;
  Store* store = nullptr;
  ~Memory();
  uint32_t pages() const { return static_cast<uint32_t>(size / kPageSize); }
  // memory.grow: the old size in pages, or -1.
  int64_t grow(uint32_t delta_pages);
};

struct Table {
  explicit Table(allocators::IAllocator& allocator) : items(allocator) {}
  ValType elem = ValType::FuncRef;
  uint32_t max = kMaxTableSize;
  bool has_max = false;
  List<uint64_t> items;
  void* host = nullptr;
  uint32_t host_id = 0;
  int64_t grow(uint32_t delta, uint64_t init);
};

struct Global {
  GlobalType type;
  Slot value;
  void* host = nullptr;
  uint32_t host_id = 0;
};

// A function: one the module defines (code is set) or one the host provides (code is null; the host knows it by host_id).
struct FuncInst {
  const FuncSig* sig = nullptr;
  Instance* instance = nullptr;
  const FuncCode* code = nullptr;
  uint32_t index = 0;
  uint32_t host_id = 0;   // for a function of the host: which one
  void* host = nullptr;
  uint32_t handle = 0;    // the number the host's registry gave this function when it first handed it out (0: none yet)
};

struct Instance {
  explicit Instance(allocators::IAllocator& allocator)
      : funcs(allocator), tables(allocator), memories(allocator), globals(allocator), type_sigs(allocator), dropped_elems(allocator), dropped_datas(allocator) {}
  SharedPointer<Module> module;
  List<FuncInst*> funcs;
  List<Table*> tables;
  List<Memory*> memories;
  List<Global*> globals;
  List<const FuncSig*> type_sigs;
  List<uint8_t> dropped_elems, dropped_datas;
};

struct ExternVal {
  ExternKind kind = ExternKind::Func;
  void* ptr = nullptr;   // a FuncInst*, Table*, Memory* or Global*
};

class IHost {
 public:
  // Calls a function the host provides. `args` holds sig->nparams values; the results go to `results` (sig->nresults slots). False if the host failed: whatever it
  // reports (a script exception) is pending in the host, and the interpreter unwinds.
  virtual bool call(Store& store, FuncInst& function, const Slot* args, Slot* results) = 0;
  // Called now and then while code runs, with the number of instructions run since the last call. False stops the run (a limit; the host reports it).
  virtual bool tick(uint64_t instructions) = 0;

 protected:
  IHost() = default;
  ~IHost() = default;
};

enum class Status : uint8_t { Ok, Trap, HostError };

class Machine;

class Store {
 public:
  explicit Store(allocators::IAllocator& allocator);
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  allocators::IAllocator& allocator() { return allocator_; }
  void set_host(IHost* host) { host_ = host; }
  IHost* host() { return host_; }

  // The one FuncSig with these contents (what call_indirect compares).
  const FuncSig* intern_sig(const FuncSig& sig);

  // Objects of the store.
  // A zero filled memory of the type's minimum size; null if there is none to be had. `resize` and `host` are given to the memory (see Memory::resize); with a resize
  // function the first allocation is made by calling it too.
  Memory* new_memory(const MemType& type, bool (*resize)(Memory&, uint64_t) = nullptr, void* host = nullptr);
  // The resize function and host pointer the memories of instantiated modules get.
  void set_memory_hook(bool (*resize)(Memory&, uint64_t), void* host) {
    memory_resize_ = resize;
    memory_host_ = host;
  }
  Table* new_table(const TableType& type, uint64_t init);
  Global* new_global(const GlobalType& type, const Slot& value);
  FuncInst* new_host_function(const FuncSig* sig, uint32_t host_id, void* host);
  uint64_t memory_pages_in_use() const { return pages_in_use_; }
  uint64_t page_budget() const { return page_budget_; }
  void set_page_budget(uint64_t pages) { page_budget_ = pages; }
  void add_pages(uint64_t pages) { pages_in_use_ += pages; }

  // Instantiates a module: checks the imports (kind, type and limits) against what the module declares, builds the instance, runs the active segments and the start
  // function. On failure `*error` says what (`*link` tells a link error from a trap or a host failure; `*status` is HostError if the start function failed in the host).
  bool instantiate(const SharedPointer<Module>& module, ArrayView<const ExternVal> imports, Instance** out, String* error, bool* link, Status* status);

  // The export `name` of kind `kind`.
  static bool find_export(const Instance& instance, StringView name, ExternVal* out);
  // The value of export number `index` of the module.
  static ExternVal export_value(const Instance& instance, const Export& e);

  // Calls a function. `args` has sig->nparams values; `results` gets sig->nresults. On a trap `*trap` is a static message.
  Status invoke(FuncInst& function, const Slot* args, Slot* results, const char** trap);

  // Applies a constant expression (in the context of an instance whose globals and functions are set up so far).
  bool eval_const(const ConstExpr& expr, const Instance& instance, Slot* out);

 private:
  friend class Machine;
  allocators::IAllocator& allocator_;
  IHost* host_ = nullptr;
  List<UniquePointer<FuncSig>> sigs_;
  List<UniquePointer<Memory>> memories_;
  List<UniquePointer<Table>> tables_;
  List<UniquePointer<Global>> globals_;
  List<UniquePointer<FuncInst>> funcs_;
  List<UniquePointer<Instance>> instances_;
  UniquePointer<Machine> machine_;
  uint64_t pages_in_use_ = 0, page_budget_ = 4096;
  bool (*memory_resize_)(Memory&, uint64_t) = nullptr;
  void* memory_host_ = nullptr;
};

const char* val_type_name(ValType t);

// A funcref is the address of its FuncInst, kept in a 64 bit slot (null is 0). These two are the only places that turn one into the other: a FuncInst lives as long as its
// Store, and a slot holds either 0 or a value func_ref() made.
inline uint64_t func_ref(const FuncInst* f) { return reinterpret_cast<uint64_t>(f); }
inline FuncInst* func_from_ref(uint64_t r) { return reinterpret_cast<FuncInst*>(r); }

}  // namespace wasm
