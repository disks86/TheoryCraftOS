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

#include "Hardware/Acpi/AmlInterpreter.hpp"
#include <new>

namespace acpi {

// The interpreter recurses (a method calls a method, an operand is an expression) on thread stacks of 64 KiB, so the big or rarely used operations stay out of
// line: what the recursive path holds on the stack is then small.
#define AML_NOINLINE __attribute__((noinline))

namespace {
constexpr uint64_t kPermanentLimit = 8ull << 20;    // memory for the namespace and the values AML keeps
constexpr uint64_t kTemporaryLimit = 4ull << 20;    // memory for one evaluation
constexpr uint64_t kOperationBudget = 3000000;      // operations one evaluation may run
constexpr uint32_t kMaxDepth = 8;                   // nested method calls
constexpr uint32_t kMaxNameSegments = 16;
constexpr uint32_t kMaxNesting = 32;                // nested operations in one expression
constexpr uint32_t kMaxLocalNodes = 24;

bool is_lead_char(uint8_t c) { return (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_name_char(uint8_t c) { return is_lead_char(c) || (c >= '0' && c <= '9'); }
bool starts_name(uint8_t c) { return is_lead_char(c) || c == 0x5C || c == 0x5E || c == 0x2E || c == 0x2F; }

uint64_t string_length(const char* s) {
  uint64_t n = 0;
  while (s[n]) ++n;
  return n;
}
}  // namespace

enum class NodeKind : uint8_t { Scope, Device, Processor, ThermalZone, PowerResource, Name, Method, Region, Field, IndexField, BufField, Mutex, Event, Alias };

struct Node {
  char name[4] = {'_', '_', '_', '_'};
  NodeKind kind = NodeKind::Scope;
  Node* parent = nullptr;
  Node* first_child = nullptr;
  Node* last_child = nullptr;
  Node* next = nullptr;
  // Name: the value, once the initializer ran; Method: the body.
  Value value;
  bool ready = false;
  bool evaluating = false;
  const uint8_t* code = nullptr;
  const uint8_t* code_end = nullptr;
  uint8_t argc = 0;
  uint8_t native = 0;   // 1: _OSI
  // Region.
  RegionSpace space = RegionSpace::SystemMemory;
  uint64_t base = 0, length = 0;
  // Field (region, or index and data for IndexField), and BufField (bit_offset into `value`'s buffer).
  Node* region = nullptr;
  Node* index = nullptr;
  Node* data = nullptr;
  uint32_t bit_offset = 0, bit_width = 0;
  uint8_t access = 0, update = 0;
  Node* target = nullptr;   // Alias
};

namespace {
class Arena {
 public:
  Arena(allocators::IAllocator& allocator, uint64_t limit) : allocator_(allocator), limit_(limit) {}
  ~Arena() { release(); }
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Zeroed, 16-byte aligned; nullptr if over the limit or out of memory.
  void* alloc(uint64_t size) {
    size = (size + 15) & ~uint64_t{15};
    if (size == 0) size = 16;
    if (!head_ || head_->size - head_->used < size) {
      if (total_ + size > limit_) return nullptr;
      const uint64_t chunk = size > kChunk ? size : kChunk;
      Chunk* c = static_cast<Chunk*>(allocator_.allocate_aligned(sizeof(Chunk) + chunk, 16));
      if (!c) return nullptr;
      c->next = head_;
      c->size = chunk;
      c->used = 0;
      head_ = c;
      total_ += chunk;
    }
    uint8_t* p = reinterpret_cast<uint8_t*>(head_ + 1) + head_->used;
    head_->used += size;
    __builtin_memset(p, 0, size);
    return p;
  }
  void reset() {
    release();
  }
  bool contains(const void* p) const {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (const Chunk* c = head_; c; c = c->next) {
      const uint8_t* start = reinterpret_cast<const uint8_t*>(c + 1);
      if (b >= start && b < start + c->size) return true;
    }
    return false;
  }

 private:
  struct alignas(16) Chunk {
    Chunk* next;
    uint64_t size, used;
  };
  static constexpr uint64_t kChunk = 32 * 1024;
  void release() {
    while (head_) {
      Chunk* n = head_->next;
      allocator_.deallocate(head_);
      head_ = n;
    }
    total_ = 0;
  }
  allocators::IAllocator& allocator_;
  uint64_t limit_;
  uint64_t total_ = 0;
  Chunk* head_ = nullptr;
};

enum class Flow : uint8_t { Next, Break, Continue, Return };

struct Frame {
  Value args[kMaxArguments];
  Value locals[8];
  Node* scope = nullptr;       // where relative names are looked up from
  Node* method = nullptr;
  const uint8_t* pc = nullptr;
  const uint8_t* end = nullptr;
  Flow flow = Flow::Next;
  Value ret;
  Node* local_nodes[kMaxLocalNodes];
  uint32_t local_node_count = 0;
};

struct NamePath {
  bool root = false;
  uint32_t parents = 0;
  uint32_t count = 0;
  char segs[kMaxNameSegments][4];
};

struct Target {
  enum Kind : uint8_t { None, Debug, Local, Arg, Named, Slot, Byte } kind = None;
  uint32_t index = 0;
  Node* node = nullptr;
  Value* slot = nullptr;
  uint8_t* byte = nullptr;
};
}  // namespace

struct AmlInterpreter::Impl {
  Impl(allocators::IAllocator& allocator, IAmlHost& host_, AmlInterpreter& owner_)
      : mem(allocator), host(host_), owner(owner_), perm(allocator, kPermanentLimit), temp(allocator, kTemporaryLimit) {
    cur = &perm;
  }

  allocators::IAllocator& mem;
  IAmlHost& host;
  AmlInterpreter& owner;
  Arena perm, temp;
  Arena* cur;
  Node* root_node = nullptr;
  uint64_t int_mask = ~0ull;
  bool have_revision = false;
  uint64_t ops = 0;
  uint32_t depth = 0;
  uint32_t nesting = 0;
  bool bad = false;
  const char* error = "";

  // --- Failure.
  Value fail(const char* why) {
    if (!bad) {
      bad = true;
      error = why;
    }
    return Value();
  }
  bool failed_flag(const char* why) {
    fail(why);
    return false;
  }

  // --- Making objects.
  Node* new_node(Arena& arena, Node* parent, const char* seg, NodeKind kind) {
    void* memory = arena.alloc(sizeof(Node));
    if (!memory) {
      fail("out of memory");
      return nullptr;
    }
    Node* n = new (memory) Node();
    __builtin_memcpy(n->name, seg, 4);
    n->kind = kind;
    n->parent = parent;
    if (parent) {
      if (parent->last_child) parent->last_child->next = n;
      else parent->first_child = n;
      parent->last_child = n;
    }
    if (&arena == &perm) ++owner.node_count_;
    return n;
  }
  Value* new_values(Arena& arena, uint32_t count) {
    Value* v = static_cast<Value*>(arena.alloc(sizeof(Value) * (count ? count : 1)));
    if (!v) fail("out of memory");
    return v;
  }
  uint8_t* new_bytes(Arena& arena, uint32_t count) {
    uint8_t* p = static_cast<uint8_t*>(arena.alloc(count + 1ull));
    if (!p) fail("out of memory");
    return p;
  }
  Value make_int(uint64_t v) const {
    Value r;
    r.kind = ValueKind::Integer;
    r.integer = v & int_mask;
    return r;
  }
  Value make_buffer(Arena& arena, uint32_t length) {
    Value r;
    uint8_t* p = new_bytes(arena, length);
    if (!p) return Value();
    r.kind = ValueKind::Buffer;
    r.bytes = p;
    r.length = length;
    r.integer = length;
    return r;
  }
  Value make_string(Arena& arena, const char* text, uint32_t length) {
    Value r;
    uint8_t* p = new_bytes(arena, length);
    if (!p) return Value();
    __builtin_memcpy(p, text, length);
    r.kind = ValueKind::String;
    r.bytes = p;
    r.length = length;
    r.integer = length;
    return r;
  }
  Value make_object(Node* n) {
    Value r;
    r.kind = ValueKind::Object;
    r.node = n;
    return r;
  }
  Value make_ref(Value* slot) {
    Value r;
    r.kind = ValueKind::Reference;
    r.slot = slot;
    return r;
  }

  // A copy of a value that does not share its contents (strings, buffers and packages are copied; the rest are plain).
  AML_NOINLINE Value clone(const Value& v, Arena& arena, uint32_t level = 0) {
    switch (v.kind) {
      case ValueKind::String: return make_string(arena, reinterpret_cast<const char*>(v.bytes), v.length);
      case ValueKind::Buffer: {
        Value r = make_buffer(arena, v.length);
        if (!bad && v.length) __builtin_memcpy(r.bytes, v.bytes, v.length);
        return r;
      }
      case ValueKind::Package: {
        if (level > 8) return fail("packages nested too deep");
        Value r;
        r.kind = ValueKind::Package;
        r.elements = new_values(arena, v.length);
        if (bad) return Value();
        r.length = v.length;
        for (uint32_t i = 0; i < v.length; ++i) r.elements[i] = clone(v.elements[i], arena, level + 1);
        return r;
      }
      default: return v;
    }
  }

  // --- Reading the code.
  static bool have(const Frame& f, uint64_t n) { return static_cast<uint64_t>(f.end - f.pc) >= n; }
  bool need(const Frame& f, uint64_t n) {
    if (have(f, n)) return true;
    return failed_flag("the code ends in the middle of an operation");
  }
  uint8_t byte(Frame& f) {
    if (!need(f, 1)) return 0;
    return *f.pc++;
  }
  uint32_t word(Frame& f) {
    if (!need(f, 2)) return 0;
    const uint32_t v = f.pc[0] | (uint32_t{f.pc[1]} << 8);
    f.pc += 2;
    return v;
  }
  uint32_t dword(Frame& f) {
    if (!need(f, 4)) return 0;
    const uint32_t v = f.pc[0] | (uint32_t{f.pc[1]} << 8) | (uint32_t{f.pc[2]} << 16) | (uint32_t{f.pc[3]} << 24);
    f.pc += 4;
    return v;
  }
  uint64_t qword(Frame& f) {
    const uint64_t low = dword(f);
    return low | (uint64_t{dword(f)} << 32);
  }
  // PkgLength: leaves pc after it and returns where the object ends.
  const uint8_t* pkg_end(Frame& f) {
    const uint8_t* start = f.pc;
    const uint8_t lead = byte(f);
    if (bad) return f.pc;
    const uint32_t extra = lead >> 6;
    uint32_t length;
    if (extra == 0) {
      length = lead & 0x3F;
    } else {
      if (!need(f, extra)) return f.pc;
      length = lead & 0x0F;
      for (uint32_t i = 0; i < extra; ++i) length |= uint32_t{*f.pc++} << (4 + 8 * i);
    }
    if (length < static_cast<uint64_t>(f.pc - start) || length > static_cast<uint64_t>(f.end - start)) {
      fail("bad package length");
      return f.pc;
    }
    return start + length;
  }

  // --- Names.
  bool read_name(Frame& f, NamePath* out) {
    *out = NamePath();
    if (!need(f, 1)) return false;
    if (*f.pc == 0x5C) {
      out->root = true;
      ++f.pc;
    } else {
      while (have(f, 1) && *f.pc == 0x5E) {
        ++out->parents;
        ++f.pc;
      }
    }
    if (!need(f, 1)) return false;
    uint32_t count;
    const uint8_t c = *f.pc;
    if (c == 0x00) {
      ++f.pc;
      count = 0;
    } else if (c == 0x2E) {
      ++f.pc;
      count = 2;
    } else if (c == 0x2F) {
      ++f.pc;
      count = byte(f);
      if (count > kMaxNameSegments) return failed_flag("name path too long");
    } else if (is_lead_char(c)) {
      count = 1;
    } else {
      return failed_flag("bad name");
    }
    if (!need(f, uint64_t{count} * 4)) return false;
    for (uint32_t i = 0; i < count; ++i) {
      for (uint32_t j = 0; j < 4; ++j) {
        const uint8_t ch = f.pc[j];
        if (!(j == 0 ? is_lead_char(ch) : is_name_char(ch))) return failed_flag("bad name segment");
        out->segs[i][j] = static_cast<char>(ch);
      }
      f.pc += 4;
    }
    out->count = count;
    return true;
  }
  static bool same_seg(const char* a, const char* b) { return __builtin_memcmp(a, b, 4) == 0; }
  Node* child_named(Node* parent, const char* seg) const {
    for (Node* n = parent->first_child; n; n = n->next)
      if (same_seg(n->name, seg)) return n;
    return nullptr;
  }
  Node* resolve(Frame* f, Node* scope, const NamePath& p) {
    if (!p.root && p.parents == 0 && p.count == 1) {   // a plain name: here, then in each enclosing scope
      if (f)
        for (uint32_t i = f->local_node_count; i > 0; --i)
          if (same_seg(f->local_nodes[i - 1]->name, p.segs[0])) return f->local_nodes[i - 1];
      for (Node* s = scope; s; s = s->parent)
        if (Node* n = child_named(s, p.segs[0])) return n;
      return nullptr;
    }
    Node* cur_node = p.root ? root_node : scope;
    for (uint32_t i = 0; i < p.parents && cur_node && cur_node != root_node; ++i) cur_node = cur_node->parent;
    for (uint32_t i = 0; i < p.count && cur_node; ++i) cur_node = child_named(cur_node, p.segs[i]);
    return cur_node;
  }
  // The node a new name is made in (everything but the last segment must exist, or is made as a scope), and the last segment.
  Node* parent_for_new(Frame& f, Node* scope, const NamePath& p, const char** last) {
    if (p.count == 0) {
      fail("a definition needs a name");
      return nullptr;
    }
    Node* cur_node = p.root ? root_node : scope;
    for (uint32_t i = 0; i < p.parents && cur_node && cur_node != root_node; ++i) cur_node = cur_node->parent;
    for (uint32_t i = 0; i + 1 < p.count; ++i) {
      Node* n = child_named(cur_node, p.segs[i]);
      if (!n) n = new_node(perm, cur_node, p.segs[i], NodeKind::Scope);
      if (!n) return nullptr;
      cur_node = n;
    }
    *last = p.segs[p.count - 1];
    return cur_node;
  }
  // The node for a definition: an existing one of the same name is taken over (a later table redefines).
  Node* define(Frame& f, Node* scope, const NamePath& p, NodeKind kind) {
    const char* last = nullptr;
    Node* parent_node = parent_for_new(f, scope, p, &last);
    if (!parent_node) return nullptr;
    Node* n = child_named(parent_node, last);
    if (n) {
      n->kind = kind;
      n->ready = false;
      n->value = Value();
      return n;
    }
    return new_node(perm, parent_node, last, kind);
  }

  // --- Conversions.
  Value deref(Value v) {
    for (int i = 0; i < 4; ++i) {
      if (v.kind == ValueKind::Reference && v.slot) {
        v = *v.slot;
      } else if (v.kind == ValueKind::BufferByte && v.bytes) {
        return make_int(*v.bytes);
      } else {
        break;
      }
    }
    return v;
  }
  bool to_int(Value v, uint64_t* out) {
    v = deref(v);
    switch (v.kind) {
      case ValueKind::Integer: *out = v.integer; return true;
      case ValueKind::Uninitialized: *out = 0; return true;
      case ValueKind::Buffer: {
        uint64_t r = 0;
        for (uint32_t i = 0; i < v.length && i < 8; ++i) r |= uint64_t{v.bytes[i]} << (8 * i);
        *out = r & int_mask;
        return true;
      }
      case ValueKind::String: {   // a hex number
        uint64_t r = 0;
        for (uint32_t i = 0; i < v.length && i < 16; ++i) {
          const uint8_t c = v.bytes[i];
          const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
          if (d < 0) break;
          r = (r << 4) | static_cast<uint64_t>(d);
        }
        *out = r & int_mask;
        return true;
      }
      default: return failed_flag("an integer was expected");
    }
  }
  uint64_t eval_int(Frame& f) {
    const Value v = eval(f);
    if (bad) return 0;
    uint64_t r = 0;
    to_int(v, &r);
    return r;
  }

  // --- Names as values.
  AML_NOINLINE Value* name_slot(Node* n) {
    while (n->kind == NodeKind::Alias && n->target) n = n->target;
    if (n->kind != NodeKind::Name) {
      fail("not a named value");
      return nullptr;
    }
    if (!n->ready) {
      if (n->evaluating || !n->code) {
        fail("a value uses itself");
        return nullptr;
      }
      n->evaluating = true;
      Frame init;
      init.scope = n->parent;
      init.pc = n->code;
      init.end = n->code_end;
      Arena* saved = cur;
      cur = &perm;
      const Value v = eval(init);
      cur = saved;
      n->evaluating = false;
      if (bad) return nullptr;
      n->value = v;
      n->ready = true;
    }
    return &n->value;
  }

  Node* pci_scope_device(Node* region) {
    for (Node* n = region->parent; n; n = n->parent) {
      if (n->kind == NodeKind::Device) return n;
    }
    return nullptr;
  }
  AML_NOINLINE bool region_access(Node* region, uint64_t offset, uint32_t width, RegionAccess* out) {
    out->space = region->space;
    out->address = region->base + offset;
    out->width_bits = width;
    out->pci_bus = out->pci_device = out->pci_function = 0;
    if (region->space == RegionSpace::PciConfig) {
      Node* device = pci_scope_device(region);
      uint64_t adr = 0;
      if (device) {
        if (Node* a = child_named(device, "_ADR")) {
          Value* slot = a->kind == NodeKind::Name ? name_slot(a) : nullptr;
          if (slot) to_int(*slot, &adr);
        }
        for (Node* up = device; up; up = up->parent) {   // the bus number of the host bridge above
          if (Node* b = child_named(up, "_BBN")) {
            Value* slot = b->kind == NodeKind::Name ? name_slot(b) : nullptr;
            uint64_t bus = 0;
            if (slot && to_int(*slot, &bus)) out->pci_bus = static_cast<uint8_t>(bus);
            break;
          }
        }
      }
      out->pci_device = static_cast<uint8_t>((adr >> 16) & 0x1F);
      out->pci_function = static_cast<uint8_t>(adr & 7);
    }
    return !bad;
  }
  static uint32_t unit_bits(const Node* field, RegionSpace space) {
    if (space == RegionSpace::EmbeddedControl || space == RegionSpace::SmBus || space == RegionSpace::SystemCmos) return 8;
    switch (field->access) {
      case 2: return 16;
      case 3: return 32;
      case 4: return 64;
      default: return 8;   // Any, Byte, Buffer
    }
  }
  static Node* field_region(Node* field) {
    return field->kind == NodeKind::Field ? field->region : nullptr;
  }
  AML_NOINLINE bool unit_read(Node* field, uint64_t unit, uint32_t bits, uint64_t* out) {
    if (field->kind == NodeKind::IndexField) {   // write the index, read the data register
      if (!field_write_raw(field->index, unit * (bits / 8))) return false;
      Value v;
      if (!field_read(field->data, &v)) return false;
      return to_int(v, out);
    }
    Node* region = field_region(field);
    if (!region) return failed_flag("a field without a region");
    RegionAccess a;
    if (!region_access(region, unit * (bits / 8), bits, &a)) return false;
    if (!host.region_read(a, out)) return failed_flag("a region could not be read");
    return true;
  }
  AML_NOINLINE bool unit_write(Node* field, uint64_t unit, uint32_t bits, uint64_t value) {
    if (field->kind == NodeKind::IndexField) {
      if (!field_write_raw(field->index, unit * (bits / 8))) return false;
      Value v = make_int(value);
      return field_write(field->data, v);
    }
    Node* region = field_region(field);
    if (!region) return failed_flag("a field without a region");
    RegionAccess a;
    if (!region_access(region, unit * (bits / 8), bits, &a)) return false;
    if (!host.region_write(a, value)) return failed_flag("a region could not be written");
    return true;
  }
  bool field_write_raw(Node* field, uint64_t value) {
    Value v;
    v.kind = ValueKind::Integer;
    v.integer = value;
    return field_write(field, v);
  }
  Node* effective_region_for_bits(Node* field) {
    return field->kind == NodeKind::IndexField ? nullptr : field_region(field);
  }
  uint32_t field_unit_bits(Node* field) {
    Node* region = effective_region_for_bits(field);
    return unit_bits(field, region ? region->space : RegionSpace::SystemMemory);
  }

  // Reads the bits of a field into an integer (up to the integer width) or a buffer.
  AML_NOINLINE bool field_read(Node* field, Value* out) {
    if (field->kind == NodeKind::BufField) return buffer_field_read(field, out);
    const uint32_t ub = field_unit_bits(field);
    const uint32_t width = field->bit_width;
    const bool as_buffer = width > (int_mask == 0xFFFFFFFFull ? 32u : 64u);
    uint8_t scratch[8] = {};
    Value buf;
    uint8_t* dst = scratch;
    if (as_buffer) {
      buf = make_buffer(*cur, (width + 7) / 8);
      if (bad) return false;
      dst = buf.bytes;
    }
    if (width) {
      const uint64_t first = field->bit_offset / ub, last = (uint64_t{field->bit_offset} + width - 1) / ub;
      for (uint64_t u = first; u <= last; ++u) {
        uint64_t raw = 0;
        if (!unit_read(field, u, ub, &raw)) return false;
        const uint64_t unit_start = u * ub;
        for (uint32_t b = 0; b < ub; ++b) {
          const uint64_t pos = unit_start + b;
          if (pos < field->bit_offset || pos >= uint64_t{field->bit_offset} + width) continue;
          const uint64_t bit = pos - field->bit_offset;
          if ((raw >> b) & 1) dst[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
        }
      }
    }
    if (as_buffer) {
      *out = buf;
    } else {
      uint64_t v = 0;
      for (uint32_t i = 0; i < 8; ++i) v |= uint64_t{scratch[i]} << (8 * i);
      *out = make_int(v);
    }
    return true;
  }
  AML_NOINLINE bool field_write(Node* field, const Value& value) {
    if (field->kind == NodeKind::BufField) return buffer_field_write(field, value);
    if (field->kind != NodeKind::Field && field->kind != NodeKind::IndexField) return failed_flag("not a field");
    const Value src = deref(value);
    uint8_t scratch[8] = {};
    const uint8_t* bytes = scratch;
    uint32_t byte_count = 8;
    if (src.kind == ValueKind::Buffer || src.kind == ValueKind::String) {
      bytes = src.bytes;
      byte_count = src.length;
    } else {
      uint64_t v = 0;
      if (!to_int(src, &v)) return false;
      for (uint32_t i = 0; i < 8; ++i) scratch[i] = static_cast<uint8_t>(v >> (8 * i));
    }
    const uint32_t ub = field_unit_bits(field);
    const uint32_t width = field->bit_width;
    if (!width) return true;
    const uint64_t first = field->bit_offset / ub, last = (uint64_t{field->bit_offset} + width - 1) / ub;
    for (uint64_t u = first; u <= last; ++u) {
      const uint64_t unit_start = u * ub;
      uint64_t raw = 0;
      const bool whole = field->bit_offset <= unit_start && uint64_t{field->bit_offset} + width >= unit_start + ub;
      if (!whole) {
        if (field->update == 0) {
          if (!unit_read(field, u, ub, &raw)) return false;   // keep the other bits
        } else if (field->update == 1) {
          raw = ub == 64 ? ~0ull : ((1ull << ub) - 1);
        }
      }
      for (uint32_t b = 0; b < ub; ++b) {
        const uint64_t pos = unit_start + b;
        if (pos < field->bit_offset || pos >= uint64_t{field->bit_offset} + width) continue;
        const uint64_t bit = pos - field->bit_offset;
        const bool one = bit / 8 < byte_count && ((bytes[bit / 8] >> (bit % 8)) & 1);
        if (one) raw |= 1ull << b;
        else raw &= ~(1ull << b);
      }
      if (!unit_write(field, u, ub, raw)) return false;
    }
    return true;
  }
  AML_NOINLINE bool buffer_field_read(Node* field, Value* out) {
    const Value buffer = deref(field->value);
    if (buffer.kind != ValueKind::Buffer && buffer.kind != ValueKind::String) return failed_flag("a buffer field outside its buffer");
    const uint64_t end_bit = uint64_t{field->bit_offset} + field->bit_width;
    if (end_bit > uint64_t{buffer.length} * 8) return failed_flag("a buffer field reaches past its buffer");
    const bool as_buffer = field->bit_width > (int_mask == 0xFFFFFFFFull ? 32u : 64u);
    uint8_t scratch[8] = {};
    Value result;
    uint8_t* dst = scratch;
    if (as_buffer) {
      result = make_buffer(*cur, (field->bit_width + 7) / 8);
      if (bad) return false;
      dst = result.bytes;
    }
    for (uint32_t i = 0; i < field->bit_width; ++i) {
      const uint64_t pos = uint64_t{field->bit_offset} + i;
      if ((buffer.bytes[pos / 8] >> (pos % 8)) & 1) dst[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    }
    if (as_buffer) {
      *out = result;
    } else {
      uint64_t v = 0;
      for (uint32_t i = 0; i < 8; ++i) v |= uint64_t{scratch[i]} << (8 * i);
      *out = make_int(v);
    }
    return true;
  }
  AML_NOINLINE bool buffer_field_write(Node* field, const Value& value) {
    const Value buffer = deref(field->value);
    if (buffer.kind != ValueKind::Buffer && buffer.kind != ValueKind::String) return failed_flag("a buffer field outside its buffer");
    const uint64_t end_bit = uint64_t{field->bit_offset} + field->bit_width;
    if (end_bit > uint64_t{buffer.length} * 8) return failed_flag("a buffer field reaches past its buffer");
    const Value src = deref(value);
    uint8_t scratch[8] = {};
    const uint8_t* bytes = scratch;
    uint32_t byte_count = 8;
    if (src.kind == ValueKind::Buffer || src.kind == ValueKind::String) {
      bytes = src.bytes;
      byte_count = src.length;
    } else {
      uint64_t v = 0;
      if (!to_int(src, &v)) return false;
      for (uint32_t i = 0; i < 8; ++i) scratch[i] = static_cast<uint8_t>(v >> (8 * i));
    }
    for (uint32_t i = 0; i < field->bit_width; ++i) {
      const uint64_t pos = uint64_t{field->bit_offset} + i;
      const bool one = i / 8 < byte_count && ((bytes[i / 8] >> (i % 8)) & 1);
      if (one) buffer.bytes[pos / 8] |= static_cast<uint8_t>(1u << (pos % 8));
      else buffer.bytes[pos / 8] &= static_cast<uint8_t>(~(1u << (pos % 8)));
    }
    return true;
  }

  // The value an object has when named in an expression (methods are called by the caller of this).
  AML_NOINLINE Value node_value(Node* n) {
    for (int i = 0; i < 8 && n->kind == NodeKind::Alias && n->target; ++i) n = n->target;
    switch (n->kind) {
      case NodeKind::Name: {
        Value* slot = name_slot(n);
        return slot ? *slot : Value();
      }
      case NodeKind::Field:
      case NodeKind::IndexField:
      case NodeKind::BufField: {
        Value v;
        if (!field_read(n, &v)) return Value();
        return v;
      }
      default: return make_object(n);
    }
  }

  // --- Storing.
  // Stores into a slot that holds a value (a named value, a package element, a local variable).
  AML_NOINLINE void assign(Value* slot, const Value& source) {
    Arena& arena = perm.contains(slot) ? perm : temp;
    Value v = source;
    if (v.kind == ValueKind::Reference && slot->kind != ValueKind::Reference) v = deref(v);
    switch (slot->kind) {
      case ValueKind::Buffer:
        if (v.kind == ValueKind::Buffer || v.kind == ValueKind::String) {
          const uint32_t n = v.length < slot->length ? v.length : slot->length;
          __builtin_memcpy(slot->bytes, v.bytes, n);
          if (n < slot->length) __builtin_memset(slot->bytes + n, 0, slot->length - n);
          return;
        }
        if (v.kind == ValueKind::Integer) {
          for (uint32_t i = 0; i < slot->length; ++i) slot->bytes[i] = i < 8 ? static_cast<uint8_t>(v.integer >> (8 * i)) : 0;
          return;
        }
        break;
      case ValueKind::String:
        if ((v.kind == ValueKind::String || v.kind == ValueKind::Buffer) && v.length <= slot->integer) {
          __builtin_memcpy(slot->bytes, v.bytes, v.length);
          slot->bytes[v.length] = 0;
          slot->length = v.length;
          return;
        }
        break;
      case ValueKind::Integer:
        if (v.kind == ValueKind::Buffer || v.kind == ValueKind::String) {
          uint64_t r = 0;
          to_int(v, &r);
          slot->integer = r;
          return;
        }
        break;
      case ValueKind::Package:
        if (v.kind == ValueKind::Package && v.length <= slot->length) {
          for (uint32_t i = 0; i < v.length; ++i) assign(&slot->elements[i], v.elements[i]);
          return;
        }
        break;
      default: break;
    }
    *slot = clone(v, arena);
  }
  Value load_target(const Target& t, Frame& f) {
    switch (t.kind) {
      case Target::Local: return f.locals[t.index];
      case Target::Arg: return f.args[t.index];
      case Target::Slot: return *t.slot;
      case Target::Byte: return make_int(*t.byte);
      case Target::Named: return node_value(t.node);
      default: return Value();
    }
  }
  AML_NOINLINE void store(Frame& f, const Target& t, const Value& v) {
    switch (t.kind) {
      case Target::None:
      case Target::Debug: return;
      case Target::Local: f.locals[t.index] = clone(v, temp); return;
      case Target::Arg:
        if (f.args[t.index].kind == ValueKind::Reference && f.args[t.index].slot) assign(f.args[t.index].slot, v);
        else f.args[t.index] = clone(v, temp);
        return;
      case Target::Slot: assign(t.slot, v); return;
      case Target::Byte: {
        uint64_t b = 0;
        to_int(v, &b);
        *t.byte = static_cast<uint8_t>(b);
        return;
      }
      case Target::Named: {
        Node* n = t.node;
        for (int i = 0; i < 8 && n->kind == NodeKind::Alias && n->target; ++i) n = n->target;
        switch (n->kind) {
          case NodeKind::Name: {
            Value* slot = name_slot(n);
            if (slot) assign(slot, v);
            return;
          }
          case NodeKind::Field:
          case NodeKind::IndexField:
          case NodeKind::BufField: field_write(n, v); return;
          default: fail("cannot store into this object"); return;
        }
      }
    }
  }

  // A SuperName or Target operand.
  AML_NOINLINE Target parse_target(Frame& f) {
    Target t;
    if (!need(f, 1)) return t;
    const uint8_t op = *f.pc;
    if (op == 0x00) {
      ++f.pc;
      return t;
    }
    if (op >= 0x60 && op <= 0x67) {
      ++f.pc;
      t.kind = Target::Local;
      t.index = op - 0x60u;
      return t;
    }
    if (op >= 0x68 && op <= 0x6E) {
      ++f.pc;
      t.kind = Target::Arg;
      t.index = op - 0x68u;
      return t;
    }
    if (op == 0x5B && have(f, 2) && f.pc[1] == 0x31) {
      f.pc += 2;
      t.kind = Target::Debug;
      return t;
    }
    if (starts_name(op)) {
      NamePath p;
      if (!read_name(f, &p)) return t;
      Node* n = resolve(&f, f.scope, p);
      if (!n) {
        fail("a name that is not defined");
        return t;
      }
      t.kind = Target::Named;
      t.node = n;
      return t;
    }
    // An expression that gives a reference: Index, DerefOf, RefOf.
    const Value v = eval(f);
    if (bad) return t;
    if (v.kind == ValueKind::Reference && v.slot) {
      t.kind = Target::Slot;
      t.slot = v.slot;
    } else if (v.kind == ValueKind::BufferByte && v.bytes) {
      t.kind = Target::Byte;
      t.byte = v.bytes;
    } else if (v.kind == ValueKind::Object) {
      t.kind = Target::Named;
      t.node = v.node;
    } else {
      fail("an operand cannot be stored into");
    }
    return t;
  }

  // --- Executing.
  bool tick() {
    if (++ops > kOperationBudget) {
      fail("the method ran for too long");
      return false;
    }
    return !bad;
  }

  AML_NOINLINE Value call(Node* method, const Value* arguments, uint32_t argument_count) {
    if (method->native == 1) {   // _OSI
      Value a = argument_count ? deref(arguments[0]) : Value();
      static const char* const kYes[] = {"Windows 2001", "Windows 2001 SP1", "Windows 2001 SP2", "Windows 2001.1", "Windows 2006", "Windows 2006 SP1", "Windows 2006 SP2",
                                          "Windows 2006.1", "Windows 2009", "Windows 2012", "Windows 2013", "Windows 2015", "Module Device", "Processor Device",
                                          "3.0 _SCP Extensions", "Processor Aggregator Device"};
      if (a.kind == ValueKind::String) {
        for (const char* s : kYes)
          if (string_length(s) == a.length && __builtin_memcmp(s, a.bytes, a.length) == 0) return make_int(~0ull);
      }
      return make_int(0);
    }
    if (depth >= kMaxDepth) return fail("methods nested too deep");
    if (!method->code) return make_int(0);
    ++depth;
    Frame callee;
    callee.method = method;
    callee.scope = method;
    callee.pc = method->code;
    callee.end = method->code_end;
    for (uint32_t i = 0; i < argument_count && i < kMaxArguments; ++i) callee.args[i] = arguments[i];
    exec_list(callee, callee.end);
    --depth;
    if (bad) return Value();
    if (callee.flow == Flow::Return) return callee.ret;
    return make_int(0);
  }

  AML_NOINLINE Value use_name(Frame& f) {
    NamePath p;
    if (!read_name(f, &p)) return Value();
    Node* n = resolve(&f, f.scope, p);
    if (!n) return fail("a name that is not defined");
    for (int i = 0; i < 8 && n->kind == NodeKind::Alias && n->target; ++i) n = n->target;
    if (n->kind == NodeKind::Method) {
      Value args[kMaxArguments];
      for (uint32_t i = 0; i < n->argc && i < kMaxArguments; ++i) {
        args[i] = eval(f);
        if (bad) return Value();
      }
      return call(n, args, n->argc);
    }
    return node_value(n);
  }

  // Skips the statements of a block and runs them.
  AML_NOINLINE void exec_list(Frame& f, const uint8_t* end) {
    const uint8_t* saved_end = f.end;
    f.end = end <= f.end ? end : f.end;
    while (f.pc < f.end && !bad && f.flow == Flow::Next) {
      if (!tick()) break;
      const uint8_t op = *f.pc;
      switch (op) {
        case 0xA0: {   // If
          ++f.pc;
          const uint8_t* body_end = pkg_end(f);
          if (bad) break;
          const uint64_t cond = eval_int(f);
          if (bad) break;
          const bool has_else = body_end < f.end && *body_end == 0xA1;
          if (cond) {
            exec_list(f, body_end);
            if (bad || f.flow != Flow::Next) break;
            f.pc = body_end;
            if (has_else) {
              f.pc = body_end + 1;
              f.pc = pkg_skip(f);
            }
          } else {
            f.pc = body_end;
            if (has_else) {
              ++f.pc;
              const uint8_t* else_end = pkg_end(f);
              if (bad) break;
              exec_list(f, else_end);
              if (bad || f.flow != Flow::Next) break;
              f.pc = else_end;
            }
          }
          break;
        }
        case 0xA1: fail("an Else without an If"); break;
        case 0xA2: {   // While
          ++f.pc;
          const uint8_t* body_end = pkg_end(f);
          if (bad) break;
          const uint8_t* predicate = f.pc;
          for (;;) {
            if (!tick()) break;
            f.pc = predicate;
            const uint64_t cond = eval_int(f);
            if (bad || !cond) break;
            exec_list(f, body_end);
            if (bad) break;
            if (f.flow == Flow::Break) {
              f.flow = Flow::Next;
              break;
            }
            if (f.flow == Flow::Continue) f.flow = Flow::Next;
            if (f.flow == Flow::Return) break;
          }
          if (!bad && f.flow != Flow::Return) f.pc = body_end;
          break;
        }
        case 0xA4: {   // Return
          ++f.pc;
          const Value v = eval(f);
          if (bad) break;
          f.ret = deref(v);   // (a reference into this frame would not outlive it)
          f.flow = Flow::Return;
          break;
        }
        case 0xA5: ++f.pc; f.flow = Flow::Break; break;
        case 0x9F: ++f.pc; f.flow = Flow::Continue; break;
        case 0xA3: ++f.pc; break;   // Noop
        case 0xCC: ++f.pc; break;   // BreakPoint
        case 0x08: {                // Name: a named value local to the method
          ++f.pc;
          NamePath p;
          if (!read_name(f, &p)) break;
          Node* n = local_node(f, p, NodeKind::Name);
          if (!n) break;
          const Value v = eval(f);
          if (bad) break;
          n->value = clone(v, temp);
          n->ready = true;
          break;
        }
        case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F: create_buffer_field(f, op); break;
        case 0x5B:
          if (have(f, 2) && f.pc[1] == 0x13) {
            f.pc += 2;
            create_field_generic(f);
            break;
          }
          if (have(f, 2) && (f.pc[1] == 0x01 || f.pc[1] == 0x02)) {   // Mutex, Event
            const bool is_mutex = f.pc[1] == 0x01;
            f.pc += 2;
            NamePath p;
            if (!read_name(f, &p)) break;
            if (is_mutex) byte(f);
            local_node(f, p, is_mutex ? NodeKind::Mutex : NodeKind::Event);
            break;
          }
          eval(f);
          break;
        default: eval(f); break;
      }
    }
    f.end = saved_end;
  }
  // After an Else at pc (its opcode already read): the end of its body.
  const uint8_t* pkg_skip(Frame& f) { return pkg_end(f); }

  Node* local_node(Frame& f, const NamePath& p, NodeKind kind) {
    if (p.count != 1 || f.local_node_count >= kMaxLocalNodes) {
      fail("unsupported local name");
      return nullptr;
    }
    Node* n = new_node(temp, f.method, p.segs[0], kind);
    if (!n) return nullptr;
    // (new_node linked it under the method: harmless, the method has no children of its own and the temporary memory goes away; unlink to be clean)
    if (f.method) {
      f.method->first_child = nullptr;
      f.method->last_child = nullptr;
    }
    f.local_nodes[f.local_node_count++] = n;
    return n;
  }

  // CreateDWordField and the like: (source, index, name).
  AML_NOINLINE void create_buffer_field(Frame& f, uint8_t op) {
    ++f.pc;
    uint32_t width, scale;   // bits; bits per index step
    switch (op) {
      case 0x8A: width = 32; scale = 8; break;
      case 0x8B: width = 16; scale = 8; break;
      case 0x8C: width = 8; scale = 8; break;
      case 0x8D: width = 1; scale = 1; break;
      default: width = 64; scale = 8; break;
    }
    const Value source = eval(f);
    if (bad) return;
    const uint64_t index = eval_int(f);
    if (bad) return;
    make_buffer_field(f, source, index * scale, width);
  }
  AML_NOINLINE void create_field_generic(Frame& f) {   // CreateField (source, bit index, bit count, name)
    const Value source = eval(f);
    if (bad) return;
    const uint64_t index = eval_int(f);
    const uint64_t count = eval_int(f);
    if (bad) return;
    make_buffer_field(f, source, index, static_cast<uint32_t>(count));
  }
  AML_NOINLINE void make_buffer_field(Frame& f, const Value& source, uint64_t bit_offset, uint32_t width) {
    NamePath p;
    if (!read_name(f, &p)) return;
    const Value buffer = deref(source);
    if (buffer.kind != ValueKind::Buffer && buffer.kind != ValueKind::String) {
      fail("a buffer field needs a buffer");
      return;
    }
    if (bit_offset + width > uint64_t{buffer.length} * 8) {
      fail("a buffer field reaches past its buffer");
      return;
    }
    Node* n;
    if (f.method) {
      n = local_node(f, p, NodeKind::BufField);
    } else {
      n = define(f, f.scope, p, NodeKind::BufField);
    }
    if (!n) return;
    n->value = buffer;
    n->bit_offset = static_cast<uint32_t>(bit_offset);
    n->bit_width = width;
  }

  // --- Expressions.
  AML_NOINLINE Value eval(Frame& f);
  AML_NOINLINE Value eval_one(Frame& f);
  AML_NOINLINE Value eval_ext(Frame& f);
  AML_NOINLINE Value eval_package(Frame& f, bool variable);
  AML_NOINLINE Value eval_buffer(Frame& f);
  AML_NOINLINE Value compare(Frame& f, uint8_t kind);
  AML_NOINLINE Value binary(Frame& f, uint8_t op);
  AML_NOINLINE Value do_index(Frame& f);
  AML_NOINLINE Value do_mid(Frame& f);
  AML_NOINLINE Value do_concat(Frame& f);
  AML_NOINLINE Value type_of(Frame& f);
  AML_NOINLINE Value size_of(Frame& f);
  AML_NOINLINE Value ref_of(Frame& f);

  // --- Loading.
  AML_NOINLINE bool load_block(Frame& f, Node* scope);
  AML_NOINLINE bool load_fields(Frame& f, const uint8_t* end, Node* scope, Node* region, Node* index, Node* data, uint8_t flags);
  AML_NOINLINE const uint8_t* skip_data(Frame& f);
};

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// Expressions

AmlInterpreter::AmlInterpreter(allocators::IAllocator& allocator, IAmlHost& host) {
  void* memory = allocator.allocate(sizeof(Impl));
  if (!memory) return;
  impl_ = new (memory) Impl(allocator, host, *this);
  Impl& m = *impl_;
  m.root_node = m.new_node(m.perm, nullptr, "\\___", NodeKind::Scope);
  if (!m.root_node) return;
  static const char* const kScopes[] = {"_SB_", "_GPE", "_PR_", "_TZ_", "_SI_"};
  for (const char* name : kScopes) m.new_node(m.perm, m.root_node, name, NodeKind::Scope);
  if (Node* osi = m.new_node(m.perm, m.root_node, "_OSI", NodeKind::Method)) {
    osi->argc = 1;
    osi->native = 1;
  }
  if (Node* os = m.new_node(m.perm, m.root_node, "_OS_", NodeKind::Name)) {
    os->value = m.make_string(m.perm, "Microsoft Windows NT", 20);
    os->ready = true;
  }
  if (Node* rev = m.new_node(m.perm, m.root_node, "_REV", NodeKind::Name)) {
    rev->value = m.make_int(2);
    rev->ready = true;
  }
  m.new_node(m.perm, m.root_node, "_GL_", NodeKind::Mutex);
}

AmlInterpreter::~AmlInterpreter() {
  if (impl_) {
    allocators::IAllocator& allocator = impl_->mem;
    impl_->~Impl();
    allocator.deallocate(impl_);
  }
}

Node* AmlInterpreter::root() const { return impl_ ? impl_->root_node : nullptr; }

Value AmlInterpreter::Impl::eval_buffer(Frame& f) {
  const uint8_t* end = pkg_end(f);
  if (bad) return Value();
  const uint64_t size = eval_int(f);
  if (bad) return Value();
  if (size > (1u << 20)) return fail("buffer too large");
  Value r = make_buffer(*cur, static_cast<uint32_t>(size));
  if (bad) return Value();
  const uint64_t init = static_cast<uint64_t>(end - f.pc);
  if (end >= f.pc) __builtin_memcpy(r.bytes, f.pc, init < size ? init : size);
  f.pc = end;
  return r;
}

Value AmlInterpreter::Impl::eval_package(Frame& f, bool variable) {
  const uint8_t* end = pkg_end(f);
  if (bad) return Value();
  uint64_t count;
  if (variable) count = eval_int(f);
  else count = byte(f);
  if (bad) return Value();
  if (count > 4096) return fail("package too large");
  Value r;
  r.kind = ValueKind::Package;
  r.elements = new_values(*cur, static_cast<uint32_t>(count));
  if (bad) return Value();
  r.length = static_cast<uint32_t>(count);
  for (uint32_t i = 0; f.pc < end && !bad; ++i) {
    if (i >= count) return fail("a package has too many elements");
    if (starts_name(*f.pc)) {   // a name stands for the object, it is not read or called
      NamePath p;
      if (!read_name(f, &p)) return Value();
      Node* n = resolve(&f, f.scope, p);
      r.elements[i] = n ? make_object(n) : Value();
    } else {
      r.elements[i] = eval(f);
    }
  }
  f.pc = end;
  return r;
}

Value AmlInterpreter::Impl::compare(Frame& f, uint8_t kind) {   // 0 equal, 1 greater, 2 less
  const Value a = deref(eval(f));
  const Value b = deref(eval(f));
  if (bad) return Value();
  int order;
  if ((a.kind == ValueKind::String || a.kind == ValueKind::Buffer) && (b.kind == ValueKind::String || b.kind == ValueKind::Buffer)) {
    const uint32_t n = a.length < b.length ? a.length : b.length;
    order = n ? __builtin_memcmp(a.bytes, b.bytes, n) : 0;
    if (order == 0) order = a.length == b.length ? 0 : (a.length > b.length ? 1 : -1);
  } else {
    uint64_t x, y;
    if (!to_int(a, &x) || !to_int(b, &y)) return Value();
    order = x == y ? 0 : (x > y ? 1 : -1);
  }
  bool result = kind == 0 ? order == 0 : kind == 1 ? order > 0 : order < 0;
  return make_int(result ? ~0ull : 0);
}

Value AmlInterpreter::Impl::binary(Frame& f, uint8_t op) {
  const uint64_t a = eval_int(f);
  const uint64_t b = eval_int(f);
  if (bad) return Value();
  uint64_t r = 0;
  switch (op) {
    case 0x72: r = a + b; break;
    case 0x74: r = a - b; break;
    case 0x77: r = a * b; break;
    case 0x79: r = b >= 64 ? 0 : a << b; break;
    case 0x7A: r = b >= 64 ? 0 : a >> b; break;
    case 0x7B: r = a & b; break;
    case 0x7C: r = ~(a & b); break;
    case 0x7D: r = a | b; break;
    case 0x7E: r = ~(a | b); break;
    case 0x7F: r = a ^ b; break;
    case 0x85:
      if (b == 0) return fail("a division by zero");
      r = a % b;
      break;
    default: return fail("unsupported operator");
  }
  Value v = make_int(r);
  const Target t = parse_target(f);
  if (bad) return Value();
  store(f, t, v);
  return v;
}

Value AmlInterpreter::Impl::do_index(Frame& f) {
  const Value src = eval(f);
  const uint64_t index = eval_int(f);
  if (bad) return Value();
  const Value base = deref(src);
  Value r;
  if (base.kind == ValueKind::Package) {
    if (index >= base.length) return fail("an index outside the package");
    r = make_ref(&base.elements[index]);
  } else if (base.kind == ValueKind::Buffer || base.kind == ValueKind::String) {
    if (index >= base.length) return fail("an index outside the buffer");
    r.kind = ValueKind::BufferByte;
    r.bytes = base.bytes + index;
  } else {
    return fail("Index needs a package, buffer or string");
  }
  const Target t = parse_target(f);
  if (bad) return Value();
  store(f, t, r);
  return r;
}

Value AmlInterpreter::Impl::do_mid(Frame& f) {
  const Value src = deref(eval(f));
  const uint64_t index = eval_int(f);
  const uint64_t length = eval_int(f);
  if (bad) return Value();
  if (src.kind != ValueKind::String && src.kind != ValueKind::Buffer) return fail("Mid needs a string or buffer");
  uint64_t n = 0;
  if (index < src.length) n = src.length - index < length ? src.length - index : length;
  Value r = src.kind == ValueKind::String ? make_string(*cur, "", 0) : make_buffer(*cur, 0);
  if (bad) return Value();
  if (src.kind == ValueKind::String) r = make_string(*cur, reinterpret_cast<const char*>(src.bytes) + (n ? index : 0), static_cast<uint32_t>(n));
  else {
    r = make_buffer(*cur, static_cast<uint32_t>(n));
    if (!bad && n) __builtin_memcpy(r.bytes, src.bytes + index, n);
  }
  const Target t = parse_target(f);
  if (bad) return Value();
  store(f, t, r);
  return r;
}

Value AmlInterpreter::Impl::do_concat(Frame& f) {
  const Value a = deref(eval(f));
  const Value b = deref(eval(f));
  if (bad) return Value();
  Value r;
  if (a.kind == ValueKind::String && b.kind == ValueKind::String) {
    r = make_string(*cur, "", 0);
    const uint32_t n = a.length + b.length;
    uint8_t* p = new_bytes(*cur, n);
    if (bad) return Value();
    __builtin_memcpy(p, a.bytes, a.length);
    __builtin_memcpy(p + a.length, b.bytes, b.length);
    r.bytes = p;
    r.length = n;
    r.integer = n;
  } else if (a.kind == ValueKind::Buffer && b.kind == ValueKind::Buffer) {
    r = make_buffer(*cur, a.length + b.length);
    if (bad) return Value();
    __builtin_memcpy(r.bytes, a.bytes, a.length);
    __builtin_memcpy(r.bytes + a.length, b.bytes, b.length);
  } else {
    return fail("unsupported Concatenate operands");
  }
  const Target t = parse_target(f);
  if (bad) return Value();
  store(f, t, r);
  return r;
}

Value AmlInterpreter::Impl::ref_of(Frame& f) {
  if (!need(f, 1)) return Value();
  const uint8_t op = *f.pc;
  Value r;
  if (op >= 0x60 && op <= 0x67) {
    ++f.pc;
    return make_ref(&f.locals[op - 0x60]);
  }
  if (op >= 0x68 && op <= 0x6E) {
    ++f.pc;
    return make_ref(&f.args[op - 0x68]);
  }
  if (starts_name(op)) {
    NamePath p;
    if (!read_name(f, &p)) return Value();
    Node* n = resolve(&f, f.scope, p);
    if (!n) return fail("RefOf a name that is not defined");
    if (n->kind == NodeKind::Name) {
      Value* slot = name_slot(n);
      return slot ? make_ref(slot) : Value();
    }
    return make_object(n);
  }
  return eval(f);
}

Value AmlInterpreter::Impl::size_of(Frame& f) {
  const Value v = deref(ref_of(f));
  if (bad) return Value();
  switch (v.kind) {
    case ValueKind::String:
    case ValueKind::Buffer:
    case ValueKind::Package: return make_int(v.length);
    default: return fail("SizeOf needs a string, buffer or package");
  }
}

Value AmlInterpreter::Impl::type_of(Frame& f) {
  if (!need(f, 1)) return Value();
  const uint8_t op = *f.pc;
  Value v;
  if (starts_name(op)) {
    NamePath p;
    if (!read_name(f, &p)) return Value();
    Node* n = resolve(&f, f.scope, p);
    if (!n) return make_int(0);
    switch (n->kind) {
      case NodeKind::Method: return make_int(8);
      case NodeKind::Device: return make_int(6);
      case NodeKind::Event: return make_int(7);
      case NodeKind::Mutex: return make_int(9);
      case NodeKind::Region: return make_int(10);
      case NodeKind::PowerResource: return make_int(11);
      case NodeKind::Processor: return make_int(12);
      case NodeKind::ThermalZone: return make_int(13);
      case NodeKind::Field:
      case NodeKind::IndexField: return make_int(5);
      case NodeKind::BufField: return make_int(14);
      default: v = node_value(n); break;
    }
  } else {
    v = eval(f);
  }
  if (bad) return Value();
  v = deref(v);
  switch (v.kind) {
    case ValueKind::Integer: return make_int(1);
    case ValueKind::String: return make_int(2);
    case ValueKind::Buffer: return make_int(3);
    case ValueKind::Package: return make_int(4);
    default: return make_int(0);
  }
}

Value AmlInterpreter::Impl::eval_ext(Frame& f) {
  const uint8_t op = byte(f);
  if (bad) return Value();
  switch (op) {
    case 0x12: {   // CondRefOf
      bool found = false;
      Value ref;
      if (need(f, 1) && starts_name(*f.pc)) {
        NamePath p;
        if (!read_name(f, &p)) return Value();
        Node* n = resolve(&f, f.scope, p);
        if (n) {
          found = true;
          ref = n->kind == NodeKind::Name ? Value() : make_object(n);
          if (n->kind == NodeKind::Name) {
            Value* slot = name_slot(n);
            if (slot) ref = make_ref(slot);
          }
        }
      } else {
        parse_target(f);   // (a local or argument: always there)
        found = true;
      }
      const Target t = parse_target(f);
      if (bad) return Value();
      if (found) store(f, t, ref);
      return make_int(found ? ~0ull : 0);
    }
    case 0x21: {   // Stall
      const uint64_t us = eval_int(f);
      if (!bad) host.stall_us(us > 100 ? 100 : static_cast<uint32_t>(us));
      return Value();
    }
    case 0x22: {   // Sleep
      const uint64_t ms = eval_int(f);
      if (!bad) host.sleep_ms(ms > 20 ? 20 : static_cast<uint32_t>(ms));
      return Value();
    }
    case 0x23:     // Acquire (a mutex: there is one thread, so it is always free)
      parse_target(f);
      word(f);
      return make_int(0);
    case 0x27: parse_target(f); return Value();                 // Release
    case 0x24: parse_target(f); return Value();                 // Signal
    case 0x26: parse_target(f); return Value();                 // Reset
    case 0x25: parse_target(f); eval(f); return make_int(0);    // Wait
    case 0x28: {   // FromBCD
      const uint64_t v = eval_int(f);
      const Target t = parse_target(f);
      if (bad) return Value();
      uint64_t r = 0, scale = 1;
      for (uint64_t x = v; x; x >>= 4, scale *= 10) r += (x & 0xF) * scale;
      Value out = make_int(r);
      store(f, t, out);
      return out;
    }
    case 0x29: {   // ToBCD
      uint64_t v = eval_int(f);
      const Target t = parse_target(f);
      if (bad) return Value();
      uint64_t r = 0;
      for (uint32_t shift = 0; v; v /= 10, shift += 4) r |= (v % 10) << shift;
      Value out = make_int(r);
      store(f, t, out);
      return out;
    }
    case 0x30: return make_int(2);   // Revision
    case 0x31: return Value();       // Debug
    case 0x32: return fail("the firmware called Fatal");
    case 0x33: return make_int(host.timer_100ns());
    default: return fail("unsupported extended operation");
  }
}

Value AmlInterpreter::Impl::eval(Frame& f) {
  if (!tick()) return Value();
  if (nesting >= kMaxNesting) return fail("expression nested too deep");
  ++nesting;
  const Value v = eval_one(f);
  --nesting;
  return v;
}

Value AmlInterpreter::Impl::eval_one(Frame& f) {
  if (!need(f, 1)) return Value();
  const uint8_t op = *f.pc;
  if (starts_name(op)) return use_name(f);
  ++f.pc;
  switch (op) {
    case 0x00: return make_int(0);
    case 0x01: return make_int(1);
    case 0xFF: return make_int(~0ull);
    case 0x0A: return make_int(byte(f));
    case 0x0B: return make_int(word(f));
    case 0x0C: return make_int(dword(f));
    case 0x0E: return make_int(qword(f));
    case 0x0D: {
      const uint8_t* start = f.pc;
      while (f.pc < f.end && *f.pc) ++f.pc;
      if (f.pc >= f.end) return fail("an unterminated string");
      const uint32_t n = static_cast<uint32_t>(f.pc - start);
      ++f.pc;
      return make_string(*cur, reinterpret_cast<const char*>(start), n);
    }
    case 0x11: return eval_buffer(f);
    case 0x12: return eval_package(f, false);
    case 0x13: return eval_package(f, true);
    case 0x5B: return eval_ext(f);
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67: return f.locals[op - 0x60];
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: return f.args[op - 0x68];
    case 0x70: {   // Store
      const Value v = eval(f);
      const Target t = parse_target(f);
      if (bad) return Value();
      store(f, t, v);
      return v;
    }
    case 0x9D: {   // CopyObject
      const Value v = eval(f);
      const Target t = parse_target(f);
      if (bad) return Value();
      if (t.kind == Target::Local) f.locals[t.index] = clone(v, temp);
      else store(f, t, v);
      return v;
    }
    case 0x71: return ref_of(f);
    case 0x72: case 0x74: case 0x77: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: case 0x85: return binary(f, op);
    case 0x73: return do_concat(f);
    case 0x75: case 0x76: {   // Increment, Decrement
      const Target t = parse_target(f);
      if (bad) return Value();
      uint64_t v = 0;
      if (!to_int(load_target(t, f), &v)) return Value();
      Value r = make_int(op == 0x75 ? v + 1 : v - 1);
      store(f, t, r);
      return r;
    }
    case 0x78: {   // Divide
      const uint64_t a = eval_int(f);
      const uint64_t b = eval_int(f);
      if (bad) return Value();
      if (b == 0) return fail("a division by zero");
      const Target remainder = parse_target(f);
      const Target quotient = parse_target(f);
      if (bad) return Value();
      store(f, remainder, make_int(a % b));
      Value q = make_int(a / b);
      store(f, quotient, q);
      return q;
    }
    case 0x80: {   // Not
      const uint64_t a = eval_int(f);
      const Target t = parse_target(f);
      if (bad) return Value();
      Value r = make_int(~a);
      store(f, t, r);
      return r;
    }
    case 0x81: case 0x82: {   // FindSetLeftBit, FindSetRightBit
      const uint64_t a = eval_int(f);
      const Target t = parse_target(f);
      if (bad) return Value();
      uint64_t r = 0;
      if (a) {
        if (op == 0x81) {
          r = 64 - static_cast<uint64_t>(__builtin_clzll(a));
        } else {
          r = static_cast<uint64_t>(__builtin_ctzll(a)) + 1;
        }
      }
      Value out = make_int(r);
      store(f, t, out);
      return out;
    }
    case 0x83: {   // DerefOf
      Value v = eval(f);
      if (bad) return Value();
      if (v.kind == ValueKind::Reference && v.slot) return *v.slot;
      if (v.kind == ValueKind::BufferByte && v.bytes) return make_int(*v.bytes);
      if (v.kind == ValueKind::Object && v.node) return node_value(v.node);
      return v;
    }
    case 0x86: {   // Notify
      const Target t = parse_target(f);
      const Value v = eval(f);
      if (!bad && t.kind == Target::Named && t.node) host.notify(t.node, v.kind == ValueKind::Integer ? static_cast<uint32_t>(v.integer) : 0);
      return Value();
    }
    case 0x87: return size_of(f);
    case 0x88: return do_index(f);
    case 0x8E: return type_of(f);
    case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F:
      --f.pc;
      create_buffer_field(f, op);
      return Value();
    case 0x90: {   // Land
      const uint64_t a = eval_int(f);
      const uint64_t b = eval_int(f);
      return bad ? Value() : make_int(a && b ? ~0ull : 0);
    }
    case 0x91: {   // Lor
      const uint64_t a = eval_int(f);
      const uint64_t b = eval_int(f);
      return bad ? Value() : make_int(a || b ? ~0ull : 0);
    }
    case 0x92: {   // Lnot, and LNotEqual, LLessEqual, LGreaterEqual
      if (have(f, 1) && (*f.pc == 0x93 || *f.pc == 0x94 || *f.pc == 0x95)) {
        const uint8_t second = *f.pc++;
        // not (a == b), not (a > b), not (a < b)
        Value v = compare(f, second == 0x93 ? 0 : second == 0x94 ? 1 : 2);
        if (bad) return Value();
        return make_int(v.integer ? 0 : ~0ull);
      }
      const uint64_t a = eval_int(f);
      return bad ? Value() : make_int(a ? 0 : ~0ull);
    }
    case 0x93: return compare(f, 0);
    case 0x94: return compare(f, 1);
    case 0x95: return compare(f, 2);
    case 0x96: {   // ToBuffer
      const Value v = deref(eval(f));
      const Target t = parse_target(f);
      if (bad) return Value();
      Value r;
      if (v.kind == ValueKind::Buffer) {
        r = v;
      } else if (v.kind == ValueKind::Integer) {
        r = make_buffer(*cur, int_mask == 0xFFFFFFFFull ? 4 : 8);
        if (bad) return Value();
        for (uint32_t i = 0; i < r.length; ++i) r.bytes[i] = static_cast<uint8_t>(v.integer >> (8 * i));
      } else if (v.kind == ValueKind::String) {
        r = make_buffer(*cur, v.length + 1);
        if (bad) return Value();
        __builtin_memcpy(r.bytes, v.bytes, v.length);
      } else {
        return fail("ToBuffer needs an integer, string or buffer");
      }
      store(f, t, r);
      return r;
    }
    case 0x99: {   // ToInteger
      uint64_t v = 0;
      const Value src = eval(f);
      const Target t = parse_target(f);
      if (bad || !to_int(src, &v)) return Value();
      Value r = make_int(v);
      store(f, t, r);
      return r;
    }
    case 0x9E: return do_mid(f);
    default: return fail("unsupported operation");
  }
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// Loading a definition block

// The end of a data object at pc (for names whose value is evaluated later), or nullptr.
const uint8_t* AmlInterpreter::Impl::skip_data(Frame& f) {
  if (!need(f, 1)) return nullptr;
  const uint8_t op = *f.pc;
  switch (op) {
    case 0x00: case 0x01: case 0xFF: return f.pc + 1;
    case 0x0A: return have(f, 2) ? f.pc + 2 : nullptr;
    case 0x0B: return have(f, 3) ? f.pc + 3 : nullptr;
    case 0x0C: return have(f, 5) ? f.pc + 5 : nullptr;
    case 0x0E: return have(f, 9) ? f.pc + 9 : nullptr;
    case 0x0D: {
      const uint8_t* p = f.pc + 1;
      while (p < f.end && *p) ++p;
      return p < f.end ? p + 1 : nullptr;
    }
    case 0x11: case 0x12: case 0x13: {
      const uint8_t* start = f.pc;
      ++f.pc;
      const uint8_t* end = pkg_end(f);
      f.pc = start;
      return bad ? nullptr : end;
    }
    default:
      if (starts_name(op)) {
        const uint8_t* start = f.pc;
        NamePath p;
        const bool ok = read_name(f, &p);
        const uint8_t* end = f.pc;
        f.pc = start;
        return ok ? end : nullptr;
      }
      // Anything else (an expression): run it once to find where it ends.
      {
        const uint8_t* start = f.pc;
        Arena* saved = cur;
        cur = &temp;
        eval(f);
        cur = saved;
        const uint8_t* end = f.pc;
        f.pc = start;
        return bad ? nullptr : end;
      }
  }
}

bool AmlInterpreter::Impl::load_fields(Frame& f, const uint8_t* end, Node* scope, Node* region, Node* index, Node* data, uint8_t flags) {
  uint64_t offset = 0;
  uint8_t access = flags & 0x0F;
  const uint8_t update = (flags >> 5) & 3;
  while (f.pc < end && !bad) {
    const uint8_t lead = *f.pc;
    if (lead == 0x00) {   // reserved
      ++f.pc;
      const uint8_t* start = f.pc;
      (void)start;
      uint32_t length = 0;
      const uint8_t first = byte(f);
      const uint32_t extra = first >> 6;
      if (extra == 0) length = first & 0x3F;
      else {
        length = first & 0x0F;
        for (uint32_t i = 0; i < extra; ++i) length |= uint32_t{byte(f)} << (4 + 8 * i);
      }
      offset += length;
    } else if (lead == 0x01) {   // access type
      ++f.pc;
      access = byte(f) & 0x0F;
      byte(f);   // attribute
    } else if (lead == 0x03) {   // extended access
      ++f.pc;
      access = byte(f) & 0x0F;
      byte(f);
      byte(f);
    } else if (lead == 0x02) {   // connection: skip a name or a buffer
      ++f.pc;
      if (!need(f, 1)) return false;
      if (*f.pc == 0x11) {
        ++f.pc;
        const uint8_t* buffer_end = pkg_end(f);
        if (bad) return false;
        f.pc = buffer_end;
      } else {
        NamePath p;
        if (!read_name(f, &p)) return false;
      }
    } else if (is_lead_char(lead)) {
      char seg[4];
      if (!need(f, 4)) return false;
      __builtin_memcpy(seg, f.pc, 4);
      f.pc += 4;
      uint32_t length = 0;
      const uint8_t first = byte(f);
      const uint32_t extra = first >> 6;
      if (extra == 0) length = first & 0x3F;
      else {
        length = first & 0x0F;
        for (uint32_t i = 0; i < extra; ++i) length |= uint32_t{byte(f)} << (4 + 8 * i);
      }
      if (bad) return false;
      Node* n = child_named(scope, seg);
      if (!n) n = new_node(perm, scope, seg, NodeKind::Field);
      if (!n) return false;
      n->kind = index ? NodeKind::IndexField : NodeKind::Field;
      n->region = region;
      n->index = index;
      n->data = data;
      n->bit_offset = static_cast<uint32_t>(offset);
      n->bit_width = length;
      n->access = access;
      n->update = update;
      offset += length;
    } else {
      return failed_flag("bad field list");
    }
  }
  f.pc = end;
  return !bad;
}

bool AmlInterpreter::Impl::load_block(Frame& f, Node* scope) {
  while (f.pc < f.end && !bad) {
    if (!tick()) return false;
    const uint8_t op = *f.pc++;
    switch (op) {
      case 0x10: {   // Scope
        const uint8_t* end = pkg_end(f);
        NamePath p;
        if (bad || !read_name(f, &p)) return false;
        Node* target = resolve(&f, scope, p);
        if (!target) target = define(f, scope, p, NodeKind::Scope);
        if (!target) return false;
        const uint8_t* saved_end = f.end;
        f.end = end;
        const bool ok = load_block(f, target);
        f.end = saved_end;
        f.pc = end;
        if (!ok) return false;
        break;
      }
      case 0x08: {   // Name
        NamePath p;
        if (!read_name(f, &p)) return false;
        Node* n = define(f, scope, p, NodeKind::Name);
        if (!n) return false;
        const uint8_t* end = skip_data(f);
        if (!end) return failed_flag("a Name with a value this loader cannot skip");
        n->code = f.pc;
        n->code_end = f.end;
        f.pc = end;
        break;
      }
      case 0x14: {   // Method
        const uint8_t* end = pkg_end(f);
        NamePath p;
        if (bad || !read_name(f, &p)) return false;
        const uint8_t flags = byte(f);
        Node* n = define(f, scope, p, NodeKind::Method);
        if (!n) return false;
        n->argc = flags & 7;
        n->code = f.pc;
        n->code_end = end;
        f.pc = end;
        break;
      }
      case 0x06: {   // Alias (source, alias)
        NamePath source, alias;
        if (!read_name(f, &source) || !read_name(f, &alias)) return false;
        Node* n = define(f, scope, alias, NodeKind::Alias);
        if (!n) return false;
        n->target = resolve(&f, scope, source);
        break;
      }
      case 0x15: {   // External
        NamePath p;
        if (!read_name(f, &p)) return false;
        byte(f);
        byte(f);
        break;
      }
      case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F: {
        f.scope = scope;
        f.method = nullptr;
        --f.pc;   // (create_buffer_field reads the opcode itself)
        create_buffer_field(f, op);
        break;
      }
      case 0xA0: {   // If at the top level
        f.scope = scope;
        const uint8_t* end = pkg_end(f);
        if (bad) return false;
        const uint64_t cond = eval_int(f);
        if (bad) return false;
        const bool has_else = end < f.end && *end == 0xA1;
        const uint8_t* saved_end = f.end;
        if (cond) {
          f.end = end;
          const bool ok = load_block(f, scope);
          f.end = saved_end;
          if (!ok) return false;
          f.pc = end;
          if (has_else) {
            ++f.pc;
            f.pc = pkg_end(f);
          }
        } else {
          f.pc = end;
          if (has_else) {
            ++f.pc;
            const uint8_t* else_end = pkg_end(f);
            if (bad) return false;
            f.end = else_end;
            const bool ok = load_block(f, scope);
            f.end = saved_end;
            if (!ok) return false;
            f.pc = else_end;
          }
        }
        break;
      }
      case 0x5B: {
        const uint8_t ext = byte(f);
        if (bad) return false;
        switch (ext) {
          case 0x01: case 0x02: {   // Mutex, Event
            NamePath p;
            if (!read_name(f, &p)) return false;
            if (ext == 0x01) byte(f);
            if (!define(f, scope, p, ext == 0x01 ? NodeKind::Mutex : NodeKind::Event)) return false;
            break;
          }
          case 0x13: f.scope = scope; f.method = nullptr; create_field_generic(f); break;
          case 0x80: {   // OperationRegion (name, space, offset, length)
            NamePath p;
            if (!read_name(f, &p)) return false;
            const uint8_t space = byte(f);
            f.scope = scope;
            const uint64_t offset = eval_int(f);
            const uint64_t length = eval_int(f);
            if (bad) return false;
            Node* n = define(f, scope, p, NodeKind::Region);
            if (!n) return false;
            n->space = static_cast<RegionSpace>(space);
            n->base = offset;
            n->length = length;
            break;
          }
          case 0x81: {   // Field (region, flags, list)
            const uint8_t* end = pkg_end(f);
            NamePath p;
            if (bad || !read_name(f, &p)) return false;
            const uint8_t flags = byte(f);
            Node* region = resolve(&f, scope, p);
            if (!load_fields(f, end, scope, region, nullptr, nullptr, flags)) return false;
            break;
          }
          case 0x86: {   // IndexField (index, data, flags, list)
            const uint8_t* end = pkg_end(f);
            NamePath ip, dp;
            if (bad || !read_name(f, &ip) || !read_name(f, &dp)) return false;
            const uint8_t flags = byte(f);
            Node* index_node = resolve(&f, scope, ip);
            Node* data_node = resolve(&f, scope, dp);
            if (!index_node || !data_node) return failed_flag("an IndexField without its registers");
            if (!load_fields(f, end, scope, nullptr, index_node, data_node, flags)) return false;
            break;
          }
          case 0x82: case 0x83: case 0x84: case 0x85: {   // Device, Processor, PowerResource, ThermalZone
            const uint8_t* end = pkg_end(f);
            NamePath p;
            if (bad || !read_name(f, &p)) return false;
            if (ext == 0x83) {
              byte(f);
              dword(f);
              byte(f);
            } else if (ext == 0x84) {
              byte(f);
              word(f);
            }
            const NodeKind kind = ext == 0x82 ? NodeKind::Device : ext == 0x83 ? NodeKind::Processor : ext == 0x84 ? NodeKind::PowerResource : NodeKind::ThermalZone;
            Node* node = define(f, scope, p, kind);
            if (!node) return false;
            const uint8_t* saved_end = f.end;
            f.end = end;
            const bool ok = load_block(f, node);
            f.end = saved_end;
            f.pc = end;
            if (!ok) return false;
            break;
          }
          case 0x87: case 0x88: {   // BankField, DataTableRegion: not used by what this reads
            const uint8_t* end = ext == 0x87 ? pkg_end(f) : nullptr;
            if (!end) return failed_flag("unsupported definition");
            f.pc = end;
            break;
          }
          default: return failed_flag("unsupported definition");
        }
        break;
      }
      case 0x00: break;   // padding
      default: return failed_flag("unexpected operation in a definition block");
    }
  }
  return !bad;
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// The public interface

bool AmlInterpreter::load_table(ConstByteView table) {
  if (!impl_ || !impl_->root_node || table.size() < 36) return false;
  Impl& m = *impl_;
  const uint32_t length = table[4] | (uint32_t{table[5]} << 8) | (uint32_t{table[6]} << 16) | (uint32_t{table[7]} << 24);
  if (length < 36 || length > table.size()) return false;
  if (!m.have_revision) {
    m.have_revision = true;
    m.int_mask = table[8] >= 2 ? ~0ull : 0xFFFFFFFFull;
  }
  m.bad = false;
  m.error = "";
  m.ops = 0;
  m.depth = 0;
  m.nesting = 0;
  m.cur = &m.perm;
  m.temp.reset();
  Frame f;
  f.scope = m.root_node;
  f.pc = table.data() + 36;
  f.end = table.data() + length;
  const bool ok = m.load_block(f, m.root_node);
  failed_ = m.bad;
  error_ = m.error;
  return ok;
}

Node* AmlInterpreter::find(const char* path, Node* scope) const {
  if (!impl_ || !path) return nullptr;
  Impl& m = *impl_;
  NamePath p;
  const char* s = path;
  if (*s == '\\') {
    p.root = true;
    ++s;
  }
  while (*s == '^') {
    ++p.parents;
    ++s;
  }
  while (*s) {
    if (p.count >= kMaxNameSegments) return nullptr;
    char* seg = p.segs[p.count];
    uint32_t n = 0;
    while (*s && *s != '.') {
      if (n < 4) seg[n] = *s;
      ++n;
      ++s;
    }
    if (n == 0 || n > 4) return nullptr;
    for (uint32_t i = n; i < 4; ++i) seg[i] = '_';
    ++p.count;
    if (*s == '.') ++s;
  }
  return m.resolve(nullptr, scope ? scope : m.root_node, p);
}

bool AmlInterpreter::evaluate(Node* node, const Value* arguments, uint32_t argument_count, Value* result) {
  if (!impl_ || !node) return false;
  Impl& m = *impl_;
  m.temp.reset();
  m.bad = false;
  m.error = "";
  m.ops = 0;
  m.depth = 0;
  m.nesting = 0;
  m.cur = &m.temp;
  Value v;
  if (node->kind == NodeKind::Method) {
    v = m.call(node, arguments, argument_count);
  } else {
    v = m.node_value(node);
  }
  failed_ = m.bad;
  error_ = m.error;
  if (m.bad) return false;
  if (result) *result = m.deref(v).kind == ValueKind::Reference ? v : m.deref(v);
  return true;
}

bool AmlInterpreter::evaluate_integer(Node* node, uint64_t* out) {
  Value v;
  if (!evaluate(node, nullptr, 0, &v)) return false;
  if (v.kind == ValueKind::Integer) {
    *out = v.integer;
    return true;
  }
  if (v.kind == ValueKind::Buffer) {
    uint64_t r = 0;
    for (uint32_t i = 0; i < v.length && i < 8; ++i) r |= uint64_t{v.bytes[i]} << (8 * i);
    *out = r;
    return true;
  }
  return false;
}

Node* AmlInterpreter::first_child(const Node* node) const { return node ? node->first_child : nullptr; }
Node* AmlInterpreter::next_sibling(const Node* node) const { return node ? node->next : nullptr; }
Node* AmlInterpreter::parent(const Node* node) const { return node ? node->parent : nullptr; }
bool AmlInterpreter::is_device(const Node* node) const { return node && node->kind == NodeKind::Device; }
bool AmlInterpreter::is_method(const Node* node) const { return node && node->kind == NodeKind::Method; }
bool AmlInterpreter::is_region(const Node* node) const { return node && node->kind == NodeKind::Region; }
const char* AmlInterpreter::name_of(const Node* node) const { return node ? node->name : "____"; }
RegionSpace AmlInterpreter::region_space(const Node* node) const { return node ? node->space : RegionSpace::SystemMemory; }
Node* AmlInterpreter::region_of_field(const Node* node) const { return node && node->kind == NodeKind::Field ? node->region : nullptr; }

void eisa_id_to_string(uint32_t id, char out[8]) {
  static const char kHex[] = "0123456789ABCDEF";
  const uint32_t manufacturer = ((id & 0xFF) << 8) | ((id >> 8) & 0xFF);
  out[0] = static_cast<char>(0x40 + ((manufacturer >> 10) & 0x1F));
  out[1] = static_cast<char>(0x40 + ((manufacturer >> 5) & 0x1F));
  out[2] = static_cast<char>(0x40 + (manufacturer & 0x1F));
  out[3] = kHex[(id >> 20) & 0xF];
  out[4] = kHex[(id >> 16) & 0xF];
  out[5] = kHex[(id >> 28) & 0xF];
  out[6] = kHex[(id >> 24) & 0xF];
  out[7] = 0;
}

namespace {
bool id_matches(const Value& v, const char* id) {
  char text[8];
  if (v.kind == ValueKind::Integer) {
    eisa_id_to_string(static_cast<uint32_t>(v.integer), text);
    return __builtin_memcmp(text, id, 8) == 0;
  }
  if (v.kind == ValueKind::String) return string_length(id) == v.length && __builtin_memcmp(v.bytes, id, v.length) == 0;
  return false;
}
}  // namespace

bool AmlInterpreter::device_has_id(Node* device, const char* id) {
  if (!impl_ || !device) return false;
  Value v;
  if (Node* hid = impl_->child_named(device, "_HID")) {
    if (evaluate(hid, nullptr, 0, &v) && id_matches(v, id)) return true;
  }
  if (Node* cid = impl_->child_named(device, "_CID")) {
    if (evaluate(cid, nullptr, 0, &v)) {
      if (v.kind == ValueKind::Package) {
        for (uint32_t i = 0; i < v.length; ++i)
          if (id_matches(v.elements[i], id)) return true;
      } else if (id_matches(v, id)) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace acpi
