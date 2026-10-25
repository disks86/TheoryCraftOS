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

// Instantiation, linking and the interpreter of the WebAssembly runtime. The interpreter runs the code Module::parse made over a stack of 16 byte slots: a frame's
// locals are the bottom of its part of the stack (the arguments of the call are already there), the operands are above them. Calls between WebAssembly functions
// do not recurse in C++ (frames are kept in a table), only calls out to the host and back in do.
#include "Wasm/WasmInternal.hpp"
#include "Wasm/WasmNumeric.hpp"

namespace wasm {

// --- Memory, tables, globals ---------------------------------------------------------------------------------------------------------

Memory::~Memory() {
  if (!resize && data && store) store->allocator().deallocate(data);
}

int64_t Memory::grow(uint32_t delta_pages) {
  const uint64_t old_pages = size / kPageSize;
  const uint64_t limit = has_max ? max_pages : kMaxPages;
  const uint64_t wanted = old_pages + delta_pages;
  if (wanted > limit || wanted > kMaxPages) return -1;
  if (delta_pages == 0) return static_cast<int64_t>(old_pages);
  if (store && store->memory_pages_in_use() + delta_pages > store->page_budget()) return -1;
  const uint64_t new_size = wanted * kPageSize;
  if (resize) {
    if (!resize(*this, new_size)) return -1;
  } else {
    uint8_t* fresh = static_cast<uint8_t*>(store->allocator().allocate(new_size));
    if (!fresh) return -1;
    ArrayView<uint8_t> view(fresh, new_size);
    if (size) view.first(size).copy_from(ConstByteView(data, size));
    view.subview(size).fill(0);
    if (data) store->allocator().deallocate(data);
    data = fresh;
    size = new_size;
  }
  if (store) store->add_pages(delta_pages);
  return static_cast<int64_t>(old_pages);
}

int64_t Table::grow(uint32_t delta, uint64_t init) {
  const uint64_t old_size = items.count();
  const uint64_t limit = has_max && max < kMaxTableElements ? max : kMaxTableElements;
  if (old_size + delta > limit) return -1;
  if (!items.reserve(old_size + delta)) return -1;
  for (uint32_t i = 0; i < delta; ++i) items.push_back(init);
  return static_cast<int64_t>(old_size);
}

// --- The store ----------------------------------------------------------------------------------------------------------------------

class Machine {
 public:
  static constexpr uint32_t kStackSlots = 262144, kMaxFrames = 30000, kMaxHostDepth = 120;
  explicit Machine(Store& store) : store_(store) {}
  ~Machine() {
    store_.allocator().deallocate(stack_);
    store_.allocator().deallocate(frames_);
  }
  Status invoke(FuncInst& f, const Slot* args, Slot* results, const char** trap);

 private:
  struct Frame {
    const FuncCode* code;
    Instance* inst;
    const Instr* pc;   // where the function resumes after a call it made
    Slot* fp;
  };
  bool prepare() {
    if (stack_) return true;
    stack_ = static_cast<Slot*>(store_.allocator().allocate(sizeof(Slot) * uint64_t{kStackSlots}));
    frames_ = static_cast<Frame*>(store_.allocator().allocate(sizeof(Frame) * uint64_t{kMaxFrames}));
    if (!stack_ || !frames_) {
      store_.allocator().deallocate(stack_);
      store_.allocator().deallocate(frames_);
      stack_ = nullptr;
      frames_ = nullptr;
      return false;
    }
    sp_ = stack_;
    return true;
  }
  Status execute(uint32_t entry_depth, const char** trap);

  Store& store_;
  Slot* stack_ = nullptr;
  Frame* frames_ = nullptr;
  Slot* sp_ = nullptr;    // the top of the stack of the running code, as far as a nested call needs to know
  uint32_t depth_ = 0;
  uint32_t host_depth_ = 0;
};

Store::Store(allocators::IAllocator& allocator)
    : allocator_(allocator), sigs_(allocator), memories_(allocator), tables_(allocator), globals_(allocator), funcs_(allocator), tags_(allocator), exceptions_(allocator), free_exceptions_(allocator),
      instances_(allocator) {}

Store::~Store() = default;

const FuncSig* Store::intern_sig(const FuncSig& sig) {
  for (uint64_t i = 0; i < sigs_.count(); ++i)
    if (sigs_[i]->equals(sig)) return sigs_[i].get();
  UniquePointer<FuncSig> copy = UniquePointer<FuncSig>::make(allocator_, allocator_);
  if (!copy || !copy->assign(sig)) return nullptr;
  const FuncSig* result = copy.get();
  if (!sigs_.push_back(util::move(copy))) return nullptr;
  return result;
}

Memory* Store::new_memory(const MemType& type, bool (*resize)(Memory&, uint64_t), void* host) {
  const uint64_t pages = type.limits.min;
  if (pages_in_use_ + pages > page_budget_) return nullptr;
  UniquePointer<Memory> m = UniquePointer<Memory>::make(allocator_);
  if (!m) return nullptr;
  m->store = this;
  m->has_max = type.limits.has_max;
  m->max_pages = type.limits.has_max ? type.limits.max : kMaxPages;
  m->shared = type.limits.shared;
  m->host = host;
  m->resize = resize;
  const uint64_t bytes = pages * kPageSize;
  if (resize) {
    if (!resize(*m, bytes)) {
      m->resize = nullptr;   // (nothing was allocated that the destructor would free)
      return nullptr;
    }
  } else if (bytes) {
    m->data = static_cast<uint8_t*>(allocator_.allocate(bytes));
    if (!m->data) return nullptr;
    m->size = bytes;
    ArrayView<uint8_t>(m->data, bytes).fill(0);
  }
  pages_in_use_ += pages;
  Memory* result = m.get();
  if (!memories_.push_back(util::move(m))) return nullptr;
  return result;
}

Table* Store::new_table(const TableType& type, uint64_t init) {
  if (type.limits.min > kMaxTableElements) return nullptr;
  UniquePointer<Table> t = UniquePointer<Table>::make(allocator_, allocator_);
  if (!t) return nullptr;
  t->elem = type.elem;
  t->has_max = type.limits.has_max;
  t->max = type.limits.has_max ? type.limits.max : kMaxTableSize;
  if (!t->items.reserve(type.limits.min)) return nullptr;
  for (uint32_t i = 0; i < type.limits.min; ++i) t->items.push_back(init);
  Table* result = t.get();
  if (!tables_.push_back(util::move(t))) return nullptr;
  return result;
}

Global* Store::new_global(const GlobalType& type, const Slot& value) {
  UniquePointer<Global> g = UniquePointer<Global>::make(allocator_);
  if (!g) return nullptr;
  g->type = type;
  g->value = value;
  Global* result = g.get();
  if (!globals_.push_back(util::move(g))) return nullptr;
  return result;
}

FuncInst* Store::new_host_function(const FuncSig* sig, uint32_t host_id, void* host) {
  UniquePointer<FuncInst> f = UniquePointer<FuncInst>::make(allocator_);
  if (!f) return nullptr;
  f->sig = sig;
  f->host_id = host_id;
  f->host = host;
  FuncInst* result = f.get();
  if (!funcs_.push_back(util::move(f))) return nullptr;
  return result;
}

Tag* Store::new_tag(const FuncSig* sig) {
  UniquePointer<Tag> t = UniquePointer<Tag>::make(allocator_);
  if (!t) return nullptr;
  t->sig = sig;
  Tag* result = t.get();
  if (!tags_.push_back(util::move(t))) return nullptr;
  return result;
}

namespace {
constexpr uint64_t kMaxExceptions = 1u << 22;   // (exceptions that something refers to are kept for good)
}

ExnObj* Store::new_exception(Tag* tag, const Slot* values, uint32_t count) {
  ExnObj* e;
  if (!free_exceptions_.count()) {
    if (exceptions_.count() >= kMaxExceptions) return nullptr;
    UniquePointer<ExnObj> fresh = UniquePointer<ExnObj>::make(allocator_, allocator_);
    if (!fresh) return nullptr;
    e = fresh.get();
    if (!exceptions_.push_back(util::move(fresh))) return nullptr;
  } else {
    e = free_exceptions_[free_exceptions_.count() - 1];
    free_exceptions_.remove_at(free_exceptions_.count() - 1);
  }
  e->tag = tag;
  e->foreign = 0;
  e->referenced = false;
  e->values.clear();
  for (uint32_t i = 0; i < count; ++i)
    if (!e->values.push_back(values[i])) return nullptr;
  return e;
}

ExnObj* Store::new_foreign_exception(uint64_t foreign) {
  ExnObj* e = new_exception(nullptr, nullptr, 0);
  if (e) e->foreign = foreign;
  return e;
}

void Store::release_exception(ExnObj* exception) {
  if (!exception->referenced) free_exceptions_.push_back(exception);
}

ExternVal Store::export_value(const Instance& instance, const Export& e) {
  ExternVal v;
  v.kind = e.kind;
  switch (e.kind) {
    case ExternKind::Func: v.ptr = instance.funcs[e.index]; break;
    case ExternKind::Table: v.ptr = instance.tables[e.index]; break;
    case ExternKind::Memory: v.ptr = instance.memories[e.index]; break;
    case ExternKind::Global: v.ptr = instance.globals[e.index]; break;
    case ExternKind::Tag: v.ptr = instance.tags[e.index]; break;
  }
  return v;
}

bool Store::find_export(const Instance& instance, StringView name, ExternVal* out) {
  const Module& m = *instance.module;
  for (uint64_t i = 0; i < m.exports.count(); ++i) {
    if (m.exports[i]->name.view().equals(name)) {
      *out = export_value(instance, *m.exports[i]);
      return true;
    }
  }
  return false;
}

bool Store::eval_const(const ConstExpr& expr, const Instance& instance, Slot* out) {
  Slot stack[16];
  uint32_t n = 0;
  for (uint64_t i = 0; i < expr.ops.count(); ++i) {
    const ConstOp& op = expr.ops[i];
    if (n >= 16) return false;
    Slot s;
    switch (op.op) {
      case ConstOpKind::I32Const:
      case ConstOpKind::F32Const: s.lo = static_cast<uint32_t>(op.a); break;
      case ConstOpKind::I64Const:
      case ConstOpKind::F64Const: s.lo = op.a; break;
      case ConstOpKind::V128Const:
        s.lo = op.a;
        s.hi = op.b;
        break;
      case ConstOpKind::GlobalGet:
        if (op.a >= instance.globals.count()) return false;
        s = instance.globals[op.a]->value;
        break;
      case ConstOpKind::RefNull: s.lo = 0; break;
      case ConstOpKind::RefFunc:
        if (op.a >= instance.funcs.count()) return false;
        s.lo = func_ref(instance.funcs[op.a]);
        break;
      default: {   // the arithmetic: two operands
        if (n < 2) return false;
        const uint64_t b = stack[n - 1].lo, a = stack[n - 2].lo;
        n -= 2;
        switch (op.op) {
          case ConstOpKind::I32Add: s.lo = static_cast<uint32_t>(a + b); break;
          case ConstOpKind::I32Sub: s.lo = static_cast<uint32_t>(a - b); break;
          case ConstOpKind::I32Mul: s.lo = static_cast<uint32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)); break;
          case ConstOpKind::I64Add: s.lo = a + b; break;
          case ConstOpKind::I64Sub: s.lo = a - b; break;
          default: s.lo = a * b; break;
        }
      }
    }
    stack[n++] = s;
  }
  if (n != 1) return false;
  *out = stack[0];
  return true;
}

namespace {

bool table_limits_match(const Table& t, const TableType& want) {
  if (t.elem != want.elem) return false;
  if (t.items.count() < want.limits.min) return false;
  if (want.limits.has_max && (!t.has_max || t.max > want.limits.max)) return false;
  return true;
}

bool memory_limits_match(const Memory& m, const MemType& want) {
  if (m.pages() < want.limits.min) return false;
  if (want.limits.has_max && (!m.has_max || m.max_pages > want.limits.max)) return false;
  if (m.shared != want.limits.shared) return false;
  return true;
}

void set_error(String* error, uint64_t index, const Import& imp, const char* why) {
  error->assign("Import #");
  error->append_uint(index);
  error->append(" \"");
  error->append(imp.module.view());
  error->append("\" \"");
  error->append(imp.name.view());
  error->append("\": ");
  error->append(why);
}

}  // namespace

bool Store::instantiate(const SharedPointer<Module>& module, ArrayView<const ExternVal> imports, Instance** out, String* error, bool* link, Status* status) {
  *link = false;
  *status = Status::Ok;
  *out = nullptr;
  const Module& m = *module;
  if (imports.size() != m.imports.count()) {
    *link = true;
    error->assign("wrong number of imports");
    return false;
  }
  UniquePointer<Instance> owned = UniquePointer<Instance>::make(allocator_, allocator_);
  if (!owned) {
    error->assign(kTrapOutOfMemory);
    return false;
  }
  Instance& inst = *owned;
  inst.module = module;
  for (uint64_t i = 0; i < m.types.count(); ++i) {
    const FuncSig* sig = intern_sig(*m.types[i]);
    if (!sig || !inst.type_sigs.push_back(sig)) {
      error->assign(kTrapOutOfMemory);
      return false;
    }
  }
  // imports
  for (uint64_t i = 0; i < m.imports.count(); ++i) {
    const Import& imp = *m.imports[i];
    const ExternVal v = imports[i];
    if (v.kind != imp.kind || !v.ptr) {
      *link = true;
      set_error(error, i, imp, imp.kind == ExternKind::Func ? "function import requires a callable" : (imp.kind == ExternKind::Table ? "table import requires a WebAssembly.Table" : (imp.kind == ExternKind::Memory ? "memory import must be a WebAssembly.Memory object" : imp.kind == ExternKind::Tag ? "tag import requires a WebAssembly.Tag" : "global import must be a number, valid Wasm reference, or WebAssembly.Global object")));
      return false;
    }
    switch (imp.kind) {
      case ExternKind::Func: {
        FuncInst* f = static_cast<FuncInst*>(v.ptr);
        if (f->sig != inst.type_sigs[imp.type_index]) {
          *link = true;
          set_error(error, i, imp, "imported function does not match the expected type");
          return false;
        }
        inst.funcs.push_back(f);
        break;
      }
      case ExternKind::Table: {
        Table* t = static_cast<Table*>(v.ptr);
        if (!table_limits_match(*t, imp.table)) {
          *link = true;
          set_error(error, i, imp, t->elem != imp.table.elem ? "imported table does not match the expected type" : "table import has a smaller size or a larger maximum than required");
          return false;
        }
        inst.tables.push_back(t);
        break;
      }
      case ExternKind::Memory: {
        Memory* mem = static_cast<Memory*>(v.ptr);
        if (!memory_limits_match(*mem, imp.memory)) {
          *link = true;
          set_error(error, i, imp, "memory import has a smaller size or a larger maximum than required");
          return false;
        }
        inst.memories.push_back(mem);
        break;
      }
      case ExternKind::Global: {
        Global* g = static_cast<Global*>(v.ptr);
        if (g->type.type != imp.global.type || g->type.mut != imp.global.mut) {
          *link = true;
          set_error(error, i, imp, g->type.mut != imp.global.mut ? "imported global does not match the expected mutability" : "imported global does not match the expected type");
          return false;
        }
        inst.globals.push_back(g);
        break;
      }
      case ExternKind::Tag: {
        Tag* t = static_cast<Tag*>(v.ptr);
        if (t->sig != inst.type_sigs[imp.type_index]) {
          *link = true;
          set_error(error, i, imp, "imported tag does not match the expected type");
          return false;
        }
        inst.tags.push_back(t);
        break;
      }
    }
  }
  // functions the module defines
  for (uint64_t i = 0; i < m.codes.count(); ++i) {
    UniquePointer<FuncInst> f = UniquePointer<FuncInst>::make(allocator_);
    if (!f) {
      error->assign(kTrapOutOfMemory);
      return false;
    }
    f->code = m.codes[i].get();
    f->sig = inst.type_sigs[f->code->type_index];
    f->instance = &inst;
    f->index = static_cast<uint32_t>(m.imported_funcs + i);
    FuncInst* raw = f.get();
    if (!funcs_.push_back(util::move(f)) || !inst.funcs.push_back(raw)) {
      error->assign(kTrapOutOfMemory);
      return false;
    }
  }
  // tags, tables, memories, globals
  for (uint64_t i = m.imported_tags; i < m.tags.count(); ++i) {
    Tag* t = new_tag(inst.type_sigs[m.tags[i]]);
    if (!t || !inst.tags.push_back(t)) {
      error->assign(kTrapOutOfMemory);
      return false;
    }
  }
  for (uint64_t i = m.imported_tables; i < m.tables.count(); ++i) {
    Table* t = new_table(m.tables[i], 0);
    if (!t || !inst.tables.push_back(t)) {
      error->assign(kTrapOutOfMemory);
      return false;
    }
  }
  for (uint64_t i = m.imported_memories; i < m.memories.count(); ++i) {
    Memory* mem = new_memory(m.memories[i], memory_resize_, memory_host_);
    if (!mem || !inst.memories.push_back(mem)) {
      error->assign("could not allocate memory");
      return false;
    }
  }
  for (uint64_t i = 0; i < m.global_defs.count(); ++i) {
    Slot value;
    if (!eval_const(m.global_defs[i]->init, inst, &value)) {
      error->assign("invalid global initializer");
      return false;
    }
    Global* g = new_global(m.global_defs[i]->type, value);
    if (!g || !inst.globals.push_back(g)) {
      error->assign(kTrapOutOfMemory);
      return false;
    }
  }
  for (uint64_t i = 0; i < m.elems.count(); ++i) inst.dropped_elems.push_back(0);
  for (uint64_t i = 0; i < m.datas.count(); ++i) inst.dropped_datas.push_back(0);
  Instance* result = owned.get();
  if (!instances_.push_back(util::move(owned))) {
    error->assign(kTrapOutOfMemory);
    return false;
  }
  // active segments, in order
  for (uint64_t i = 0; i < m.elems.count(); ++i) {
    const ElemSegment& seg = *m.elems[i];
    if (seg.mode == SegmentMode::Active) {
      Slot off;
      if (!eval_const(seg.offset, *result, &off)) {
        error->assign("invalid element offset");
        return false;
      }
      Table& t = *result->tables[seg.table];
      const uint64_t start = static_cast<uint32_t>(off.lo), n = seg.items.count();
      if (start + n > t.items.count()) {
        error->assign(kTrapElemSegment);
        *status = Status::Trap;
        return false;
      }
      for (uint64_t k = 0; k < n; ++k) {
        Slot ref;
        if (!eval_const(*seg.items[k], *result, &ref)) {
          error->assign("invalid element");
          return false;
        }
        t.items[start + k] = ref.lo;
      }
    }
    if (seg.mode != SegmentMode::Passive) result->dropped_elems[i] = 1;
  }
  for (uint64_t i = 0; i < m.datas.count(); ++i) {
    const DataSegment& seg = *m.datas[i];
    if (seg.mode != SegmentMode::Active) continue;
    Slot off;
    if (!eval_const(seg.offset, *result, &off)) {
      error->assign("invalid data offset");
      return false;
    }
    Memory& mem = *result->memories[seg.memory];
    const uint64_t start = static_cast<uint32_t>(off.lo), n = seg.bytes.size();
    if (start + n > mem.size) {
      error->assign(kTrapDataSegment);
      *status = Status::Trap;
      return false;
    }
    if (n) ArrayView<uint8_t>(mem.data + start, n).copy_from(seg.bytes.view());
    result->dropped_datas[i] = 1;
  }
  *out = result;
  if (m.has_start) {
    const char* trap = nullptr;
    const Status st = invoke(*result->funcs[m.start], nullptr, nullptr, &trap);
    if (st != Status::Ok) {
      *status = st;
      if (trap) error->assign(trap);
      return false;
    }
  }
  return true;
}

Status Store::invoke(FuncInst& function, const Slot* args, Slot* results, const char** trap) {
  if (!machine_) {
    machine_ = UniquePointer<Machine>::make(allocator_, *this);
    if (!machine_) {
      *trap = kTrapOutOfMemory;
      return Status::Trap;
    }
  }
  return machine_->invoke(function, args, results, trap);
}

// --- The interpreter ----------------------------------------------------------------------------------------------------------------

Status Machine::invoke(FuncInst& f, const Slot* args, Slot* results, const char** trap) {
  *trap = nullptr;
  if (!prepare()) {
    *trap = kTrapOutOfMemory;
    return Status::Trap;
  }
  if (host_depth_ >= kMaxHostDepth) {
    *trap = kTrapStack;
    return Status::Trap;
  }
  const uint32_t np = f.sig->nparams, nr = f.sig->nresults;
  Slot* base = sp_;
  if (base + np + 1 > stack_ + kStackSlots) {
    *trap = kTrapStack;
    return Status::Trap;
  }
  for (uint32_t i = 0; i < np; ++i) base[i] = args[i];
  if (!f.code) {   // a host function called straight away
    SmallArray<Slot, 8> tmp(store_.allocator(), nr);
    if (!tmp.ok()) {
      *trap = kTrapOutOfMemory;
      return Status::Trap;
    }
    if (!store_.host() || !store_.host()->call(store_, f, base, tmp.data())) return store_.has_pending_exception() ? Status::Exception : Status::HostError;
    for (uint32_t i = 0; i < nr; ++i) results[i] = tmp[i];
    return Status::Ok;
  }
  const uint32_t entry = depth_;
  const FuncCode* c = f.code;
  if (depth_ >= kMaxFrames || base + c->nlocals + c->max_stack > stack_ + kStackSlots) {
    *trap = kTrapStack;
    return Status::Trap;
  }
  for (uint32_t i = c->nparams; i < c->nlocals; ++i) base[i] = Slot();
  Frame& fr = frames_[depth_++];
  fr.code = c;
  fr.inst = f.instance;
  fr.pc = nullptr;
  fr.fp = base;
  sp_ = base + c->nlocals;
  ++host_depth_;
  const Status st = execute(entry, trap);
  --host_depth_;
  if (st == Status::Ok) {
    for (uint32_t i = 0; i < nr; ++i) results[i] = base[i];
  }
  depth_ = entry;
  sp_ = base;
  return st;
}

#define TRAP(msg)     \
  do {                \
    *trap = (msg);    \
    goto trapped;     \
  } while (0)

// The memory a load or store names.
#define MEMORY(aux) ((aux) == 0 ? mem0 : inst->memories[aux])

Status Machine::execute(uint32_t entry_depth, const char** trap) {
  Frame* frame = &frames_[depth_ - 1];
  const FuncCode* code = frame->code;
  Instance* inst = frame->inst;
  Slot* fp = frame->fp;
  Slot* sp = sp_;
  const Instr* base_pc = code->code.view().data();
  const Instr* pc = base_pc;
  Memory* mem0 = inst->memories.count() ? inst->memories[0] : nullptr;
  Slot* const stack_end = stack_ + kStackSlots;
  uint32_t fuel = 4096;
  FuncInst* callee = nullptr;
  ExnObj* ex = nullptr;   // the exception being thrown
  bool tail = false;
  Status result = Status::Trap;

  for (;;) {
    const Instr& in = *pc++;
    switch (in.op) {
      case 0x00: TRAP(kTrapUnreachable);
      case 0x01: break;
      case kOpJmp: pc = base_pc + in.a; break;
      case kOpJmpIfZero: {
        const uint32_t c = static_cast<uint32_t>(sp[-1].lo);
        --sp;
        if (c == 0) pc = base_pc + in.a;
        break;
      }
      case kOpBrIf: {
        const uint32_t c = static_cast<uint32_t>(sp[-1].lo);
        --sp;
        if (c == 0) break;
      }
        [[fallthrough]];
      case kOpBr: {
        const uint32_t arity = static_cast<uint32_t>(in.b >> 32);
        Slot* dest = fp + static_cast<uint32_t>(in.b);
        if (sp - arity != dest) {
          for (uint32_t i = 0; i < arity; ++i) dest[i] = sp[static_cast<int64_t>(i) - arity];
          sp = dest + arity;
        }
        if (in.a < static_cast<uint32_t>(pc - base_pc) && --fuel == 0) {
          fuel = 4096;
          if (store_.host() && !store_.host()->tick(4096)) {
            sp_ = sp;
            result = Status::HostError;
            goto unwind;
          }
        }
        pc = base_pc + in.a;
        break;
      }
      case kOpBrTable: {
        uint32_t index = static_cast<uint32_t>(sp[-1].lo);
        --sp;
        if (index > in.b) index = static_cast<uint32_t>(in.b);
        const BrTarget& t = code->targets[in.a + index];
        Slot* dest = fp + t.height;
        if (sp - t.arity != dest) {
          for (uint32_t i = 0; i < t.arity; ++i) dest[i] = sp[static_cast<int64_t>(i) - t.arity];
          sp = dest + t.arity;
        }
        if (t.pc < static_cast<uint32_t>(pc - base_pc) && --fuel == 0) {
          fuel = 4096;
          if (store_.host() && !store_.host()->tick(4096)) {
            sp_ = sp;
            result = Status::HostError;
            goto unwind;
          }
        }
        pc = base_pc + t.pc;
        break;
      }
      case kOpReturn: {
      do_return:
        const uint32_t nres = code->nresults;
        for (uint32_t i = 0; i < nres; ++i) fp[i] = sp[static_cast<int64_t>(i) - nres];
        sp = fp + nres;
        if (--depth_ == entry_depth) {
          sp_ = sp;
          return Status::Ok;
        }
        frame = &frames_[depth_ - 1];
        code = frame->code;
        inst = frame->inst;
        fp = frame->fp;
        base_pc = code->code.view().data();
        pc = frame->pc;
        mem0 = inst->memories.count() ? inst->memories[0] : nullptr;
        break;
      }
      case 0x10:
        callee = inst->funcs[in.a];
        tail = false;
        goto call_function;
      case 0x12:
        callee = inst->funcs[in.a];
        tail = true;
        goto call_function;
      case 0x11:
      case 0x13: {
        const uint32_t index = static_cast<uint32_t>(sp[-1].lo);
        --sp;
        Table* table = inst->tables[in.b];
        if (index >= table->items.count()) TRAP(kTrapTable);
        callee = func_from_ref(table->items[index]);   // (what a funcref table holds)
        if (!callee) TRAP(kTrapNullFunction);
        if (callee->sig != inst->type_sigs[in.a]) TRAP(kTrapSignature);
        tail = in.op == 0x13;
        goto call_function;
      }
      case 0x08: {   // throw
        Tag* tag = inst->tags[in.a];
        const uint32_t n = tag->sig->nparams;
        ex = store_.new_exception(tag, sp - n, n);
        if (!ex) TRAP(kTrapOutOfMemory);
        sp -= n;
        goto throw_exception;
      }
      case 0x09: {   // rethrow: the exception a legacy catch block kept in its slot
        const uint64_t r = fp[in.a].lo;
        if (!r) TRAP(kTrapNullRef);
        ex = exn_from_ref(r);
        goto throw_exception;
      }
      case 0x0A: {   // throw_ref
        const uint64_t r = sp[-1].lo;
        --sp;
        if (!r) TRAP(kTrapNullRef);
        ex = exn_from_ref(r);
        goto throw_exception;
      }
      case 0x1A: --sp; break;
      case 0x1B: {
        const uint32_t c = static_cast<uint32_t>(sp[-1].lo);
        sp -= 2;
        if (c == 0) sp[-1] = sp[0];
        break;
      }
      case 0x20: *sp++ = fp[in.a]; break;
      case 0x21: fp[in.a] = *--sp; break;
      case 0x22: fp[in.a] = sp[-1]; break;
      case 0x23: *sp++ = inst->globals[in.a]->value; break;
      case 0x24: inst->globals[in.a]->value = *--sp; break;
      case 0x25: {   // table.get
        Table* t = inst->tables[in.a];
        const uint32_t i = static_cast<uint32_t>(sp[-1].lo);
        if (i >= t->items.count()) TRAP(kTrapTable);
        sp[-1] = Slot();
        sp[-1].lo = t->items[i];
        break;
      }
      case 0x26: {   // table.set
        Table* t = inst->tables[in.a];
        const uint32_t i = static_cast<uint32_t>(sp[-2].lo);
        if (i >= t->items.count()) TRAP(kTrapTable);
        t->items[i] = sp[-1].lo;
        sp -= 2;
        break;
      }

#define LOAD(op, BYTES, EXPR)                                                          \
  case op: {                                                                           \
    Memory* m = MEMORY(in.aux);                                                        \
    const uint64_t ea = static_cast<uint64_t>(static_cast<uint32_t>(sp[-1].lo)) + in.a; \
    if (ea + BYTES > m->size) TRAP(kTrapMemory);                                       \
    const uint64_t raw = num::load_le(m->data + ea, BYTES);                            \
    (void)raw;                                                                         \
    sp[-1].lo = (EXPR);                                                                \
    sp[-1].hi = 0;                                                                     \
    break;                                                                             \
  }
      LOAD(0x28, 4, raw)
      LOAD(0x29, 8, raw)
      LOAD(0x2A, 4, raw)
      LOAD(0x2B, 8, raw)
      LOAD(0x2C, 1, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw))))
      LOAD(0x2D, 1, raw)
      LOAD(0x2E, 2, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw))))
      LOAD(0x2F, 2, raw)
      LOAD(0x30, 1, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(raw))))
      LOAD(0x31, 1, raw)
      LOAD(0x32, 2, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(raw))))
      LOAD(0x33, 2, raw)
      LOAD(0x34, 4, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(raw))))
      LOAD(0x35, 4, raw)
#undef LOAD
#define STORE(op, BYTES)                                                                \
  case op: {                                                                            \
    Memory* m = MEMORY(in.aux);                                                         \
    const uint64_t ea = static_cast<uint64_t>(static_cast<uint32_t>(sp[-2].lo)) + in.a; \
    if (ea + BYTES > m->size) TRAP(kTrapMemory);                                        \
    num::store_le(m->data + ea, BYTES, sp[-1].lo);                                      \
    sp -= 2;                                                                            \
    break;                                                                              \
  }
      STORE(0x36, 4)
      STORE(0x37, 8)
      STORE(0x38, 4)
      STORE(0x39, 8)
      STORE(0x3A, 1)
      STORE(0x3B, 2)
      STORE(0x3C, 1)
      STORE(0x3D, 2)
      STORE(0x3E, 4)
#undef STORE
      case 0x3F: {
        sp->hi = 0;
        sp->lo = MEMORY(in.aux)->pages();
        ++sp;
        break;
      }
      case 0x40: {
        Memory* m = MEMORY(in.aux);
        const int64_t old = m->grow(static_cast<uint32_t>(sp[-1].lo));
        sp[-1].lo = static_cast<uint32_t>(old);
        mem0 = inst->memories.count() ? inst->memories[0] : nullptr;
        break;
      }
      case 0x41:
      case 0x42:
      case 0x43:
      case 0x44:
        sp->lo = in.b;
        sp->hi = 0;
        ++sp;
        break;

      // --- i32 comparison
      case 0x45: sp[-1].lo = static_cast<uint32_t>(sp[-1].lo) == 0; break;
#define CMP32(op, EXPR) case op: { const uint32_t b = static_cast<uint32_t>(sp[-1].lo), a = static_cast<uint32_t>(sp[-2].lo); --sp; sp[-1].lo = (EXPR) ? 1 : 0; break; }
#define CMP32S(op, EXPR) case op: { const int32_t b = static_cast<int32_t>(sp[-1].lo), a = static_cast<int32_t>(sp[-2].lo); --sp; sp[-1].lo = (EXPR) ? 1 : 0; break; }
      CMP32(0x46, a == b)
      CMP32(0x47, a != b)
      CMP32S(0x48, a < b)
      CMP32(0x49, a < b)
      CMP32S(0x4A, a > b)
      CMP32(0x4B, a > b)
      CMP32S(0x4C, a <= b)
      CMP32(0x4D, a <= b)
      CMP32S(0x4E, a >= b)
      CMP32(0x4F, a >= b)
#undef CMP32
#undef CMP32S
      case 0x50: sp[-1].lo = sp[-1].lo == 0; break;
#define CMP64(op, EXPR) case op: { const uint64_t b = sp[-1].lo, a = sp[-2].lo; --sp; sp[-1].lo = (EXPR) ? 1 : 0; break; }
#define CMP64S(op, EXPR) case op: { const int64_t b = static_cast<int64_t>(sp[-1].lo), a = static_cast<int64_t>(sp[-2].lo); --sp; sp[-1].lo = (EXPR) ? 1 : 0; break; }
      CMP64(0x51, a == b)
      CMP64(0x52, a != b)
      CMP64S(0x53, a < b)
      CMP64(0x54, a < b)
      CMP64S(0x55, a > b)
      CMP64(0x56, a > b)
      CMP64S(0x57, a <= b)
      CMP64(0x58, a <= b)
      CMP64S(0x59, a >= b)
      CMP64(0x5A, a >= b)
#undef CMP64
#undef CMP64S
#define CMPF32(op, EXPR) case op: { const float b = num::f32_from(static_cast<uint32_t>(sp[-1].lo)), a = num::f32_from(static_cast<uint32_t>(sp[-2].lo)); --sp; sp[-1].lo = (EXPR) ? 1 : 0; break; }
#define CMPF64(op, EXPR) case op: { const double b = num::f64_from(sp[-1].lo), a = num::f64_from(sp[-2].lo); --sp; sp[-1].lo = (EXPR) ? 1 : 0; break; }
      CMPF32(0x5B, a == b)
      CMPF32(0x5C, a != b)
      CMPF32(0x5D, a < b)
      CMPF32(0x5E, a > b)
      CMPF32(0x5F, a <= b)
      CMPF32(0x60, a >= b)
      CMPF64(0x61, a == b)
      CMPF64(0x62, a != b)
      CMPF64(0x63, a < b)
      CMPF64(0x64, a > b)
      CMPF64(0x65, a <= b)
      CMPF64(0x66, a >= b)
#undef CMPF32
#undef CMPF64

      // --- i32 arithmetic
      case 0x67: sp[-1].lo = num::clz32(static_cast<uint32_t>(sp[-1].lo)); break;
      case 0x68: sp[-1].lo = num::ctz32(static_cast<uint32_t>(sp[-1].lo)); break;
      case 0x69: sp[-1].lo = num::popcnt32(static_cast<uint32_t>(sp[-1].lo)); break;
#define BIN32(op, EXPR) case op: { const uint32_t b = static_cast<uint32_t>(sp[-1].lo), a = static_cast<uint32_t>(sp[-2].lo); --sp; sp[-1].lo = static_cast<uint32_t>(EXPR); break; }
      BIN32(0x6A, a + b)
      BIN32(0x6B, a - b)
      BIN32(0x6C, a * b)
      case 0x6D: {
        const int32_t b = static_cast<int32_t>(sp[-1].lo), a = static_cast<int32_t>(sp[-2].lo);
        if (b == 0) TRAP(kTrapDivZero);
        if (a == INT32_MIN && b == -1) TRAP(kTrapDivOverflow);
        --sp;
        sp[-1].lo = static_cast<uint32_t>(a / b);
        break;
      }
      case 0x6E: {
        const uint32_t b = static_cast<uint32_t>(sp[-1].lo), a = static_cast<uint32_t>(sp[-2].lo);
        if (b == 0) TRAP(kTrapDivZero);
        --sp;
        sp[-1].lo = a / b;
        break;
      }
      case 0x6F: {
        const int32_t b = static_cast<int32_t>(sp[-1].lo), a = static_cast<int32_t>(sp[-2].lo);
        if (b == 0) TRAP(kTrapRemZero);
        --sp;
        sp[-1].lo = (b == -1) ? 0 : static_cast<uint32_t>(a % b);
        break;
      }
      case 0x70: {
        const uint32_t b = static_cast<uint32_t>(sp[-1].lo), a = static_cast<uint32_t>(sp[-2].lo);
        if (b == 0) TRAP(kTrapRemZero);
        --sp;
        sp[-1].lo = a % b;
        break;
      }
      BIN32(0x71, a & b)
      BIN32(0x72, a | b)
      BIN32(0x73, a ^ b)
      BIN32(0x74, a << (b & 31))
      BIN32(0x75, static_cast<int32_t>(a) >> (b & 31))
      BIN32(0x76, a >> (b & 31))
      BIN32(0x77, num::rotl32(a, b))
      BIN32(0x78, num::rotr32(a, b))
#undef BIN32
      // --- i64 arithmetic
      case 0x79: sp[-1].lo = num::clz64(sp[-1].lo); break;
      case 0x7A: sp[-1].lo = num::ctz64(sp[-1].lo); break;
      case 0x7B: sp[-1].lo = num::popcnt64(sp[-1].lo); break;
#define BIN64(op, EXPR) case op: { const uint64_t b = sp[-1].lo, a = sp[-2].lo; --sp; sp[-1].lo = static_cast<uint64_t>(EXPR); break; }
      BIN64(0x7C, a + b)
      BIN64(0x7D, a - b)
      BIN64(0x7E, a * b)
      case 0x7F: {
        const int64_t b = static_cast<int64_t>(sp[-1].lo), a = static_cast<int64_t>(sp[-2].lo);
        if (b == 0) TRAP(kTrapDivZero);
        if (a == INT64_MIN && b == -1) TRAP(kTrapDivOverflow);
        --sp;
        sp[-1].lo = static_cast<uint64_t>(a / b);
        break;
      }
      case 0x80: {
        const uint64_t b = sp[-1].lo, a = sp[-2].lo;
        if (b == 0) TRAP(kTrapDivZero);
        --sp;
        sp[-1].lo = a / b;
        break;
      }
      case 0x81: {
        const int64_t b = static_cast<int64_t>(sp[-1].lo), a = static_cast<int64_t>(sp[-2].lo);
        if (b == 0) TRAP(kTrapRemZero);
        --sp;
        sp[-1].lo = (b == -1) ? 0 : static_cast<uint64_t>(a % b);
        break;
      }
      case 0x82: {
        const uint64_t b = sp[-1].lo, a = sp[-2].lo;
        if (b == 0) TRAP(kTrapRemZero);
        --sp;
        sp[-1].lo = a % b;
        break;
      }
      BIN64(0x83, a & b)
      BIN64(0x84, a | b)
      BIN64(0x85, a ^ b)
      BIN64(0x86, a << (b & 63))
      BIN64(0x87, static_cast<int64_t>(a) >> (b & 63))
      BIN64(0x88, a >> (b & 63))
      BIN64(0x89, num::rotl64(a, static_cast<uint32_t>(b)))
      BIN64(0x8A, num::rotr64(a, static_cast<uint32_t>(b)))
#undef BIN64

      // --- f32
#define UN32F(op, EXPR) case op: { const float a = num::f32_from(static_cast<uint32_t>(sp[-1].lo)); sp[-1].lo = num::bits(static_cast<float>(EXPR)); break; }
#define BIN32F(op, EXPR) case op: { const float b = num::f32_from(static_cast<uint32_t>(sp[-1].lo)), a = num::f32_from(static_cast<uint32_t>(sp[-2].lo)); --sp; sp[-1].lo = num::bits(static_cast<float>(EXPR)); break; }
      UN32F(0x8B, num::f_abs(a))
      UN32F(0x8C, num::f_neg(a))
      UN32F(0x8D, num::f_ceil(a))
      UN32F(0x8E, num::f_floor(a))
      UN32F(0x8F, num::f_trunc(a))
      UN32F(0x90, num::f_nearest(a))
      UN32F(0x91, num::f_sqrt(a))
      BIN32F(0x92, a + b)
      BIN32F(0x93, a - b)
      BIN32F(0x94, a * b)
      BIN32F(0x95, a / b)
      BIN32F(0x96, num::f_min(a, b))
      BIN32F(0x97, num::f_max(a, b))
      BIN32F(0x98, num::f_copysign(a, b))
#undef UN32F
#undef BIN32F
#define UN64F(op, EXPR) case op: { const double a = num::f64_from(sp[-1].lo); sp[-1].lo = num::bits(static_cast<double>(EXPR)); break; }
#define BIN64F(op, EXPR) case op: { const double b = num::f64_from(sp[-1].lo), a = num::f64_from(sp[-2].lo); --sp; sp[-1].lo = num::bits(static_cast<double>(EXPR)); break; }
      UN64F(0x99, num::f_abs(a))
      UN64F(0x9A, num::f_neg(a))
      UN64F(0x9B, num::f_ceil(a))
      UN64F(0x9C, num::f_floor(a))
      UN64F(0x9D, num::f_trunc(a))
      UN64F(0x9E, num::f_nearest(a))
      UN64F(0x9F, num::f_sqrt(a))
      BIN64F(0xA0, a + b)
      BIN64F(0xA1, a - b)
      BIN64F(0xA2, a * b)
      BIN64F(0xA3, a / b)
      BIN64F(0xA4, num::f_min(a, b))
      BIN64F(0xA5, num::f_max(a, b))
      BIN64F(0xA6, num::f_copysign(a, b))
#undef UN64F
#undef BIN64F

      // --- conversions
      case 0xA7: sp[-1].lo = static_cast<uint32_t>(sp[-1].lo); break;
#define TRUNC(op, SRC, OKFN, EXPR)                                \
  case op: {                                                      \
    const SRC x = SRC##_from(sp[-1].lo);                          \
    if (num::is_nan(x)) TRAP(kTrapConversion);                    \
    if (!num::OKFN(x)) TRAP(kTrapConversion);                     \
    sp[-1].lo = (EXPR);                                           \
    break;                                                        \
  }
#define float_from(v) num::f32_from(static_cast<uint32_t>(v))
#define double_from(v) num::f64_from(v)
      TRUNC(0xA8, float, trunc_ok_i32, static_cast<uint32_t>(static_cast<int32_t>(x)))
      TRUNC(0xA9, float, trunc_ok_u32, static_cast<uint32_t>(static_cast<int64_t>(x)))
      TRUNC(0xAA, double, trunc_ok_i32, static_cast<uint32_t>(static_cast<int32_t>(x)))
      TRUNC(0xAB, double, trunc_ok_u32, static_cast<uint32_t>(static_cast<int64_t>(x)))
      case 0xAC: sp[-1].lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(sp[-1].lo))); break;
      case 0xAD: sp[-1].lo = static_cast<uint32_t>(sp[-1].lo); break;
      TRUNC(0xAE, float, trunc_ok_i64, static_cast<uint64_t>(static_cast<int64_t>(x)))
      TRUNC(0xAF, float, trunc_ok_u64, x < 0 ? uint64_t{0} : static_cast<uint64_t>(x))
      TRUNC(0xB0, double, trunc_ok_i64, static_cast<uint64_t>(static_cast<int64_t>(x)))
      TRUNC(0xB1, double, trunc_ok_u64, x < 0 ? uint64_t{0} : static_cast<uint64_t>(x))
#undef TRUNC
#undef float_from
#undef double_from
      case 0xB2: sp[-1].lo = num::bits(static_cast<float>(static_cast<int32_t>(sp[-1].lo))); break;
      case 0xB3: sp[-1].lo = num::bits(static_cast<float>(static_cast<uint32_t>(sp[-1].lo))); break;
      case 0xB4: sp[-1].lo = num::bits(static_cast<float>(static_cast<int64_t>(sp[-1].lo))); break;
      case 0xB5: sp[-1].lo = num::bits(static_cast<float>(sp[-1].lo)); break;
      case 0xB6: sp[-1].lo = num::bits(static_cast<float>(num::f64_from(sp[-1].lo))); break;
      case 0xB7: sp[-1].lo = num::bits(static_cast<double>(static_cast<int32_t>(sp[-1].lo))); break;
      case 0xB8: sp[-1].lo = num::bits(static_cast<double>(static_cast<uint32_t>(sp[-1].lo))); break;
      case 0xB9: sp[-1].lo = num::bits(static_cast<double>(static_cast<int64_t>(sp[-1].lo))); break;
      case 0xBA: sp[-1].lo = num::bits(static_cast<double>(sp[-1].lo)); break;
      case 0xBB: sp[-1].lo = num::bits(static_cast<double>(num::f32_from(static_cast<uint32_t>(sp[-1].lo)))); break;
      case 0xBC: sp[-1].lo = static_cast<uint32_t>(sp[-1].lo); break;
      case 0xBD: break;
      case 0xBE: sp[-1].lo = static_cast<uint32_t>(sp[-1].lo); break;
      case 0xBF: break;
      case 0xC0: sp[-1].lo = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(sp[-1].lo))); break;
      case 0xC1: sp[-1].lo = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(sp[-1].lo))); break;
      case 0xC2: sp[-1].lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(sp[-1].lo))); break;
      case 0xC3: sp[-1].lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int16_t>(sp[-1].lo))); break;
      case 0xC4: sp[-1].lo = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(sp[-1].lo))); break;

      // --- references
      case 0xD0: sp->lo = 0; sp->hi = 0; ++sp; break;
      case 0xD1: sp[-1].lo = sp[-1].lo == 0; break;
      case 0xD2:
        sp->lo = func_ref(inst->funcs[in.a]);
        sp->hi = 0;
        ++sp;
        break;

      // --- saturating truncation
      case 0x100: sp[-1].lo = static_cast<uint32_t>(num::sat_i32(num::f32_from(static_cast<uint32_t>(sp[-1].lo)))); break;
      case 0x101: sp[-1].lo = num::sat_u32(num::f32_from(static_cast<uint32_t>(sp[-1].lo))); break;
      case 0x102: sp[-1].lo = static_cast<uint32_t>(num::sat_i32(num::f64_from(sp[-1].lo))); break;
      case 0x103: sp[-1].lo = num::sat_u32(num::f64_from(sp[-1].lo)); break;
      case 0x104: sp[-1].lo = static_cast<uint64_t>(num::sat_i64(num::f32_from(static_cast<uint32_t>(sp[-1].lo)))); break;
      case 0x105: sp[-1].lo = num::sat_u64(num::f32_from(static_cast<uint32_t>(sp[-1].lo))); break;
      case 0x106: sp[-1].lo = static_cast<uint64_t>(num::sat_i64(num::f64_from(sp[-1].lo))); break;
      case 0x107: sp[-1].lo = num::sat_u64(num::f64_from(sp[-1].lo)); break;

      // --- bulk memory and tables
      case 0x108: {   // memory.init
        Memory* m = MEMORY(in.aux);
        const uint64_t d = static_cast<uint32_t>(sp[-3].lo), s = static_cast<uint32_t>(sp[-2].lo), n = static_cast<uint32_t>(sp[-1].lo);
        sp -= 3;
        const DataSegment& seg = *inst->module->datas[in.a];
        const uint64_t seg_size = inst->dropped_datas[in.a] ? 0 : seg.bytes.size();
        if (s + n > seg_size || d + n > m->size) TRAP(kTrapMemory);
        if (n) ArrayView<uint8_t>(m->data + d, n).copy_from(seg.bytes.view().subview(s, n));
        break;
      }
      case 0x109: inst->dropped_datas[in.a] = 1; break;
      case 0x10A: {   // memory.copy: dst memory in.a, src memory in.b
        Memory* dm = MEMORY(in.a);
        Memory* sm = MEMORY(static_cast<uint16_t>(in.b));
        const uint64_t d = static_cast<uint32_t>(sp[-3].lo), s = static_cast<uint32_t>(sp[-2].lo), n = static_cast<uint32_t>(sp[-1].lo);
        sp -= 3;
        if (s + n > sm->size || d + n > dm->size) TRAP(kTrapMemory);
        if (d <= s || dm != sm) {
          for (uint64_t i = 0; i < n; ++i) dm->data[d + i] = sm->data[s + i];
        } else {
          for (uint64_t i = n; i > 0; --i) dm->data[d + i - 1] = sm->data[s + i - 1];
        }
        break;
      }
      case 0x10B: {   // memory.fill
        Memory* m = MEMORY(in.aux);
        const uint64_t d = static_cast<uint32_t>(sp[-3].lo), n = static_cast<uint32_t>(sp[-1].lo);
        const uint8_t v = static_cast<uint8_t>(sp[-2].lo);
        sp -= 3;
        if (d + n > m->size) TRAP(kTrapMemory);
        if (n) ArrayView<uint8_t>(m->data + d, n).fill(v);
        break;
      }
      case 0x10C: {   // table.init: element segment in.a, table in.b
        Table* t = inst->tables[in.b];
        const uint64_t d = static_cast<uint32_t>(sp[-3].lo), s = static_cast<uint32_t>(sp[-2].lo), n = static_cast<uint32_t>(sp[-1].lo);
        sp -= 3;
        const ElemSegment& seg = *inst->module->elems[in.a];
        const uint64_t seg_size = inst->dropped_elems[in.a] ? 0 : seg.items.count();
        if (s + n > seg_size || d + n > t->items.count()) TRAP(kTrapTable);
        for (uint64_t i = 0; i < n; ++i) {
          Slot ref;
          if (!store_.eval_const(*seg.items[s + i], *inst, &ref)) TRAP(kTrapTable);
          t->items[d + i] = ref.lo;
        }
        break;
      }
      case 0x10D: inst->dropped_elems[in.a] = 1; break;
      case 0x10E: {   // table.copy: dst in.a, src in.b
        Table* dt = inst->tables[in.a];
        Table* st = inst->tables[in.b];
        const uint64_t d = static_cast<uint32_t>(sp[-3].lo), s = static_cast<uint32_t>(sp[-2].lo), n = static_cast<uint32_t>(sp[-1].lo);
        sp -= 3;
        if (s + n > st->items.count() || d + n > dt->items.count()) TRAP(kTrapTable);
        if (d <= s || dt != st) {
          for (uint64_t i = 0; i < n; ++i) dt->items[d + i] = st->items[s + i];
        } else {
          for (uint64_t i = n; i > 0; --i) dt->items[d + i - 1] = st->items[s + i - 1];
        }
        break;
      }
      case 0x10F: {   // table.grow
        Table* t = inst->tables[in.a];
        const uint32_t n = static_cast<uint32_t>(sp[-1].lo);
        const uint64_t init = sp[-2].lo;
        --sp;
        sp[-1].lo = static_cast<uint32_t>(t->grow(n, init));
        sp[-1].hi = 0;
        break;
      }
      case 0x110: {   // table.size
        sp->lo = inst->tables[in.a]->items.count();
        sp->hi = 0;
        ++sp;
        break;
      }
      case 0x111: {   // table.fill
        Table* t = inst->tables[in.a];
        const uint64_t d = static_cast<uint32_t>(sp[-3].lo), n = static_cast<uint32_t>(sp[-1].lo);
        const uint64_t v = sp[-2].lo;
        sp -= 3;
        if (d + n > t->items.count()) TRAP(kTrapTable);
        for (uint64_t i = 0; i < n; ++i) t->items[d + i] = v;
        break;
      }
      default: {
        if (in.op >= 0x200 && in.op < 0x400) {
          uint32_t extra = 0;
          const char* msg = simd_exec(in.op - 0x200, &in, sp, *inst, &extra);
          if (msg) TRAP(msg);
          pc += extra;
          break;
        }
        TRAP("invalid instruction");
      }
    }
    continue;

  call_function: {
    const uint32_t np = callee->sig->nparams, nr = callee->sig->nresults;
    if (--fuel == 0) {
      fuel = 4096;
      if (store_.host() && !store_.host()->tick(4096)) {
        sp_ = sp;
        result = Status::HostError;
        goto unwind;
      }
    }
    if (!callee->code) {   // a function the host provides
      SmallArray<Slot, 8> tmp(store_.allocator(), nr);
      if (!tmp.ok()) TRAP(kTrapOutOfMemory);
      sp_ = sp;
      frame->pc = pc;
      if (!store_.host() || !store_.host()->call(store_, *callee, sp - np, tmp.data())) {
        if (store_.has_pending_exception()) {   // the host threw an exception that code in this call may catch
          ex = store_.take_pending_exception();
          goto throw_exception;
        }
        result = Status::HostError;
        goto unwind;
      }
      sp -= np;
      for (uint32_t i = 0; i < nr; ++i) *sp++ = tmp[i];
      mem0 = inst->memories.count() ? inst->memories[0] : nullptr;
      if (tail) goto do_return;
      continue;
    }
    const FuncCode* c = callee->code;
    Slot* nfp = tail ? fp : sp - np;
    if (tail) {   // the arguments move down over this function's frame
      Slot* src = sp - np;
      for (uint32_t i = 0; i < np; ++i) fp[i] = src[i];
    } else {
      if (depth_ >= kMaxFrames) TRAP(kTrapStack);
      frame->pc = pc;
    }
    if (nfp + c->nlocals + c->max_stack > stack_end) TRAP(kTrapStack);
    for (uint32_t i = c->nparams; i < c->nlocals; ++i) nfp[i] = Slot();
    sp = nfp + c->nlocals;
    if (!tail) {
      frame = &frames_[depth_++];
    }
    frame->code = c;
    frame->inst = callee->instance;
    frame->fp = nfp;
    code = c;
    inst = callee->instance;
    fp = nfp;
    base_pc = code->code.view().data();
    pc = base_pc;
    mem0 = inst->memories.count() ? inst->memories[0] : nullptr;
    continue;
  }

  throw_exception: {
    // Unwind to the innermost try_table or try that has a clause for the exception: this function's, then its callers'. `pc` is just past the instruction that threw
    // or called (a call's return address is saved in its frame). A delegate clause narrows the search to the older handlers and starts it again.
    uint64_t search = code->handlers.count();
    for (;;) {
      const uint64_t at = static_cast<uint64_t>(pc - base_pc) - 1;
      bool caught = false, restart = false;
      for (uint64_t h = search; h > 0 && !caught && !restart; --h) {
        const Handler& handler = code->handlers[h - 1];
        if (at < handler.start || at >= handler.end) continue;
        for (uint32_t k = handler.first; k != 0xFFFFFFFFu; k = code->catches[k].next) {
          const CatchClause& clause = code->catches[k];
          if (clause.kind == CatchKind::Delegate) {
            search = clause.tag;
            restart = true;
            break;
          }
          const bool any = clause.kind == CatchKind::CatchAll || clause.kind == CatchKind::CatchAllRef || clause.kind == CatchKind::LegacyCatchAll;
          if (!any && (ex->tag == nullptr || ex->tag != inst->tags[clause.tag])) continue;
          const BrTarget& target = code->targets[clause.target];
          Slot* dest = fp + target.height;
          Slot* put = dest;
          const bool legacy = clause.kind == CatchKind::LegacyCatch || clause.kind == CatchKind::LegacyCatchAll;
          if (legacy) {   // the exception itself first (if the block rethrows it), then its values
            put->lo = clause.keep ? exn_ref(ex) : 0;
            put->hi = 0;
            ++put;
          }
          if (clause.kind == CatchKind::Catch || clause.kind == CatchKind::CatchRef || clause.kind == CatchKind::LegacyCatch)
            for (const Slot& v : ex->values.view()) *put++ = v;
          if (clause.kind == CatchKind::CatchRef || clause.kind == CatchKind::CatchAllRef) {
            ex->referenced = true;
            put->lo = exn_ref(ex);
            put->hi = 0;
            ++put;
          } else if (legacy && clause.keep) {
            ex->referenced = true;
          } else {
            store_.release_exception(ex);
          }
          sp = put;
          pc = base_pc + target.pc;
          caught = true;
          break;
        }
      }
      if (restart) continue;
      if (caught) break;
      if (--depth_ == entry_depth) {   // nobody in this call catches it
        store_.set_pending_exception(ex);
        return Status::Exception;
      }
      frame = &frames_[depth_ - 1];
      code = frame->code;
      inst = frame->inst;
      fp = frame->fp;
      base_pc = code->code.view().data();
      pc = frame->pc;
      mem0 = inst->memories.count() ? inst->memories[0] : nullptr;
      search = code->handlers.count();
    }
    continue;
  }
  }

trapped:
  result = Status::Trap;
unwind:
  return result;
}

}  // namespace wasm
