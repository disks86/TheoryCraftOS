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


#include "Script/TypedArray.hpp"
#include "Utility/ByteText.hpp"

namespace script {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

constexpr uint64_t kMaxBytes = 128u << 20;

Value arg_at(const Value* args, uint32_t argc, uint32_t i) { return i < argc ? args[i] : Value(); }
Value num_value(double d) { return Value::make_number(d); }

constexpr uint32_t kSizes[] = {1, 1, 1, 2, 2, 4, 4, 4, 8, 8, 8};
const char* const kNames[] = {"Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array", "Int32Array", "Uint32Array", "Float32Array", "Float64Array", "BigInt64Array", "BigUint64Array"};

uint32_t size_of(ArrayKind k) { return kSizes[static_cast<uint32_t>(k)]; }

// --- Numbers in bytes ------------------------------------------------------------------------------------------------------

bool is_finite_number(double d) { return d == d && d - d == 0; }

// ToInt32 / ToUint32 style: the value modulo 2^bits.
uint64_t wrap_integer(double d, uint32_t bits) {
  if (!is_finite_number(d)) return 0;
  d = d < 0 ? -static_cast<double>(static_cast<uint64_t>(-d < 1.8e19 ? -d : 0.0)) : static_cast<double>(static_cast<uint64_t>(d < 1.8e19 ? d : 0.0));
  if (d >= 1.8e19 || d <= -1.8e19) return 0;
  const int64_t whole = static_cast<int64_t>(d);
  const uint64_t mask = bits >= 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
  return static_cast<uint64_t>(whole) & mask;
}

uint8_t clamp_byte(double d) {
  if (!(d > 0)) return 0;
  if (d >= 255) return 255;
  double f = static_cast<double>(static_cast<int>(d));   // round half to even
  const double diff = d - f;
  if (diff > 0.5 || (diff == 0.5 && (static_cast<int>(f) & 1))) f += 1;
  return static_cast<uint8_t>(f);
}

uint64_t read_bits(const uint8_t* p, uint32_t size, bool little) {
  uint64_t v = 0;
  for (uint32_t i = 0; i < size; ++i) {
    const uint64_t byte = p[little ? i : size - 1 - i];
    v |= byte << (8 * i);
  }
  return v;
}

void write_bits(uint8_t* p, uint32_t size, uint64_t bits, bool little) {
  for (uint32_t i = 0; i < size; ++i) p[little ? i : size - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
}

bool is_big(ArrayKind kind) { return kind == ArrayKind::BigInt64 || kind == ArrayKind::BigUint64; }

// A BigInt as 64 bits (modulo 2^64), and 64 bits as a BigInt. The digits of a BigInt are decimal.
bool bigint_bits(Vm& vm, const Value& v, uint64_t* bits) {
  bool negative;
  StringView digits;
  if (!vm.bigint_parts(v, &negative, &digits)) {
    String message(vm.allocator());
    message.append("Cannot convert a value to a BigInt");
    return vm.throw_error(ErrorKind::TypeError, message.view());
  }
  uint64_t acc = 0;
  for (uint64_t i = 0; i < digits.size(); ++i) acc = acc * 10 + static_cast<uint64_t>(digits[i] - '0');   // (wraps: the value modulo 2^64)
  *bits = negative ? ~acc + 1 : acc;
  return true;
}

Value bigint_value(Vm& vm, uint64_t bits, bool is_signed) {
  bool negative = false;
  if (is_signed && (bits >> 63)) {
    negative = true;
    bits = ~bits + 1;
  }
  char digits[24];
  int n = 0;
  if (bits == 0) digits[n++] = '0';
  while (bits > 0) {
    digits[n++] = static_cast<char>('0' + bits % 10);
    bits /= 10;
  }
  char text[24];
  for (int i = 0; i < n; ++i) text[i] = digits[n - 1 - i];
  return Value::make_object(vm.new_bigint(negative, StringView(text, static_cast<uint64_t>(n))));
}

double load(ArrayKind kind, const uint8_t* p, bool little = true) {
  const uint64_t raw = read_bits(p, size_of(kind), little);
  switch (kind) {
    case ArrayKind::Int8: return static_cast<double>(static_cast<int8_t>(raw));
    case ArrayKind::Uint8:
    case ArrayKind::Uint8Clamped: return static_cast<double>(raw);
    case ArrayKind::Int16: return static_cast<double>(static_cast<int16_t>(raw));
    case ArrayKind::Uint16: return static_cast<double>(raw);
    case ArrayKind::Int32: return static_cast<double>(static_cast<int32_t>(raw));
    case ArrayKind::Uint32: return static_cast<double>(raw);
    case ArrayKind::Float32: return static_cast<double>(__builtin_bit_cast(float, static_cast<uint32_t>(raw)));
    case ArrayKind::Float64: return __builtin_bit_cast(double, raw);
    case ArrayKind::BigInt64:
    case ArrayKind::BigUint64: return 0;   // (read as BigInts: see load_value)
  }
  return 0;
}

void store(ArrayKind kind, uint8_t* p, double d, bool little = true) {
  uint64_t bits;
  switch (kind) {
    case ArrayKind::Uint8Clamped: bits = clamp_byte(d); break;
    case ArrayKind::Float32: bits = __builtin_bit_cast(uint32_t, static_cast<float>(d)); break;
    case ArrayKind::Float64: bits = __builtin_bit_cast(uint64_t, d); break;
    default: bits = wrap_integer(d, size_of(kind) * 8); break;
  }
  write_bits(p, size_of(kind), bits, little);
}

// An element as the script sees it: a number, or a BigInt for the 64-bit integer kinds.
Value load_value(Vm& vm, ArrayKind kind, const uint8_t* p, bool little = true) {
  if (is_big(kind)) return bigint_value(vm, read_bits(p, 8, little), kind == ArrayKind::BigInt64);
  return num_value(load(kind, p, little));
}

// Converts `v` (ToNumber, or ToBigInt for the 64-bit integer kinds) and writes it. False with the exception pending.
bool store_value(Vm& vm, ArrayKind kind, uint8_t* p, const Value& v, bool little = true) {
  if (is_big(kind)) {
    uint64_t bits;
    if (!bigint_bits(vm, v, &bits)) return false;
    write_bits(p, 8, bits, little);
    return true;
  }
  double d;
  if (!vm.to_number(v, &d)) return false;
  store(kind, p, d, little);
  return true;
}

// --- Objects ---------------------------------------------------------------------------------------------------------------

JsString* cell_of(JsObject* o) { return static_cast<JsString*>(o->host_data); }
ArrayKind kind_of(JsObject* o) { return static_cast<ArrayKind>(static_cast<uint32_t>(o->bound_this.u.number)); }

// The length of a view, 0 once the bytes it covered were detached.
// A view that tracks the length of a resizable buffer (made without a length) has `capacity` 1; any other has the length it was made with, and is out of bounds
// (length 0) once the buffer is shrunk below its end.
bool tracks_length(const JsObject* o) { return o->capacity == 1; }

uint32_t live_length(JsObject* o) {
  const uint64_t total = cell_of(o)->length;
  if (tracks_length(o)) return o->position <= total ? static_cast<uint32_t>((total - o->position) / size_of(kind_of(o))) : 0;
  const uint64_t need = uint64_t{o->position} + uint64_t{o->length} * size_of(kind_of(o));
  return total >= need ? o->length : 0;
}

// An ArrayBuffer made resizable keeps its largest size + 1 in `position` (0: fixed length); a detached one has `capacity` 1.
bool buffer_resizable(const JsObject* b) { return b->position != 0; }
uint64_t buffer_max(const JsObject* b) { return b->position - 1; }
bool buffer_detached(const JsObject* b) { return b->capacity == 1; }

bool parse_index(StringView k, uint32_t* out) {
  if (k.empty() || k.size() > 10) return false;
  uint64_t v = 0;
  for (uint64_t i = 0; i < k.size(); ++i) {
    if (k[i] < '0' || k[i] > '9') return false;
    v = v * 10 + static_cast<uint64_t>(k[i] - '0');
  }
  if (k.size() > 1 && k[0] == '0') return false;
  *out = static_cast<uint32_t>(v);
  return v < 0xFFFFFFFFull;
}

uint8_t* data_of(JsString* cell) { return chars_as_bytes(cell->data()); }

bool buffer_get(Vm& vm, JsObject* o, JsString* key, Value* out) {
  (void)vm;
  const StringView k = key->view();
  if (k.equals("byteLength")) {
    *out = num_value(cell_of(o)->length);
    return true;
  }
  if (k.equals("resizable")) {
    *out = Value::make_bool(buffer_resizable(o));
    return true;
  }
  if (k.equals("maxByteLength")) {
    *out = num_value(buffer_detached(o) ? 0 : buffer_resizable(o) ? static_cast<double>(buffer_max(o)) : static_cast<double>(cell_of(o)->length));
    return true;
  }
  if (k.equals("detached")) {
    *out = Value::make_bool(buffer_detached(o));
    return true;
  }
  return false;
}

const HostClass kBufferClass = {"ArrayBuffer", buffer_get, nullptr, nullptr, nullptr};

bool typed_hook_get(Vm& vm, JsObject* o, JsString* key, Value* out) {
  const StringView k = key->view();
  uint32_t i;
  if (parse_index(k, &i)) {
    if (i < live_length(o)) typed_array_get_value(vm, o, i, out);
    else *out = Value();
    return true;
  }
  if (k.equals("length")) {
    *out = num_value(live_length(o));
    return true;
  }
  if (k.equals("byteLength")) {
    *out = num_value(static_cast<double>(live_length(o)) * size_of(kind_of(o)));
    return true;
  }
  if (k.equals("byteOffset")) {
    *out = num_value(live_length(o) || o->length == 0 ? o->position : 0);
    return true;
  }
  return false;
}

HostClass::Set typed_hook_set(Vm& vm, JsObject* o, JsString* key, const Value& value) {
  uint32_t i;
  if (!parse_index(key->view(), &i)) {
    const StringView k = key->view();
    if (k.equals("length") || k.equals("byteLength") || k.equals("byteOffset")) return HostClass::Set::Handled;
    // a number that is not an index of an array ("-1", "1.5") is not a property of a typed array: the write is dropped
    bool numeric = !k.empty();
    for (uint64_t n = 0; n < k.size() && numeric; ++n) numeric = (k[n] >= '0' && k[n] <= '9') || (n == 0 && k[n] == '-') || (n > 0 && k[n] == '.');
    return numeric && k.size() > (k[0] == '-' ? 1u : 0u) ? HostClass::Set::Handled : HostClass::Set::NotHandled;
  }
  if (!typed_array_put(vm, o, i, value)) return HostClass::Set::Threw;
  return HostClass::Set::Handled;
}

void typed_hook_keys(Vm& vm, JsObject* o, JsObject* keys) {
  const uint32_t n = live_length(o) < (1u << 20) ? live_length(o) : (1u << 20);
  for (uint32_t i = 0; i < n; ++i) {
    String s(vm.allocator());
    s.append_uint(i);
    vm.array_push(keys, Value::make_string(vm.intern(s.view())));
  }
}

const HostClass kTypedClass = {"TypedArray", typed_hook_get, typed_hook_set, nullptr, typed_hook_keys};

JsObject* g_protos_unused = nullptr;

JsObject* proto_for(Vm& vm, ArrayKind kind) {
  Value proto;
  vm.get_named(Value::make_object(vm.global()), "__typed_protos", &proto);
  Value p;
  if (proto.is_object() && vm.get_named(proto, kNames[static_cast<uint32_t>(kind)], &p) && p.is_object()) return p.as_object();
  return nullptr;
}

JsObject* new_buffer_object(Vm& vm, JsString* cell) {
  Value proto;
  vm.get_named(Value::make_object(vm.global()), "__array_buffer_proto", &proto);
  JsObject* b = vm.new_host_object(proto.is_object() ? proto.as_object() : nullptr, &kBufferClass, cell);
  vm.define_named(b, "__bytes", Value::make_string(cell), 0);   // (keeps the bytes alive)
  return b;
}

// A typed array of `kind` over (part of) a buffer.
JsObject* new_view(Vm& vm, ArrayKind kind, JsObject* buffer, uint32_t byte_offset, uint32_t length, bool tracking = false) {
  JsObject* o = vm.new_host_object(proto_for(vm, kind), &kTypedClass, cell_of(buffer));
  o->okind = ObjKind::TypedArray;
  o->position = byte_offset;
  o->length = length;
  o->capacity = tracking ? 1 : 0;
  o->bound_this = num_value(static_cast<double>(static_cast<uint32_t>(kind)));
  vm.define_named(o, "buffer", Value::make_object(buffer), 0);
  return o;
}

JsString* new_zeroed_cell(Vm& vm, uint64_t bytes) { return vm.new_bytes(static_cast<uint32_t>(bytes)); }

bool is_buffer(const Value& v) { return v.is_object() && v.as_object()->host == &kBufferClass; }

// The element at `index` of any array-like (a typed array is read directly).
bool element_of(Vm& vm, const Value& source, uint32_t index, double* out) {
  Value item;
  if (!vm.get_member(source, num_value(index), &item)) return false;
  return vm.to_number(item, out);
}

// --- The constructors -------------------------------------------------------------------------------------------------------

bool construct(Vm& vm, ArrayKind kind, const Value* args, uint32_t argc, Value* result) {
  const uint32_t size = size_of(kind);
  const Value first = arg_at(args, argc, 0);
  if (is_buffer(first)) {   // new Int16Array(buffer, byteOffset, length)
    JsObject* buffer = first.as_object();
    const uint64_t total = cell_of(buffer)->length;
    double offset = 0, count = -1;
    if (argc > 1 && !args[1].is_undefined() && !vm.to_number(args[1], &offset)) return false;
    if (argc > 2 && !args[2].is_undefined() && !vm.to_number(args[2], &count)) return false;
    if (!(offset >= 0) || offset > static_cast<double>(total)) return vm.throw_error(ErrorKind::RangeError, "Start offset is outside the bounds of the buffer");
    const uint64_t off = static_cast<uint64_t>(offset);
    if (off % size != 0) {
      String m(vm.allocator());
      m.append("start offset of ");
      m.append(kNames[static_cast<uint32_t>(kind)]);
      m.append(" should be a multiple of ");
      m.append_uint(size);
      return vm.throw_error(ErrorKind::RangeError, m.view());
    }
    uint64_t length;
    if (count < 0 && buffer_resizable(buffer)) {   // a view of a resizable buffer without a length follows the buffer
      *result = Value::make_object(new_view(vm, kind, buffer, static_cast<uint32_t>(off), static_cast<uint32_t>((total - off) / size), true));
      return true;
    }
    if (count < 0) {
      if ((total - off) % size != 0) {
        String m(vm.allocator());
        m.append("byte length of ");
        m.append(kNames[static_cast<uint32_t>(kind)]);
        m.append(" should be a multiple of ");
        m.append_uint(size);
        return vm.throw_error(ErrorKind::RangeError, m.view());
      }
      length = (total - off) / size;
    } else {
      length = static_cast<uint64_t>(count);
      if (off + length * size > total) return vm.throw_error(ErrorKind::RangeError, "Invalid typed array length");
    }
    *result = Value::make_object(new_view(vm, kind, buffer, static_cast<uint32_t>(off), static_cast<uint32_t>(length)));
    return true;
  }
  if (first.is_object()) {   // a typed array, an array, an iterable, an array-like
    JsObject* source = first.as_object();
    double n = 0;
    JsObject* items = nullptr;
    if (source->okind != ObjKind::TypedArray) {
      Value iterator;
      if (source->okind != ObjKind::Array && vm.get_named(first, "@@iterator", &iterator) && Vm::is_callable(iterator)) {
        items = vm.new_array();
        if (!vm.iterate_into(items, first, -1)) return false;
        n = items->length;
      } else if (source->okind == ObjKind::Array) {
        items = source;
        n = items->length;
      } else {
        Value len;
        if (!vm.get(first, vm.length_name(), &len) || !vm.to_number(len, &n)) return false;
        if (!(n >= 0)) n = 0;
      }
    } else {
      n = live_length(source);
    }
    if (n * size > static_cast<double>(kMaxBytes)) return vm.throw_error(ErrorKind::RangeError, "Invalid typed array length");
    JsObject* buffer = new_buffer_object(vm, new_zeroed_cell(vm, static_cast<uint64_t>(n) * size));
    JsObject* view = new_view(vm, kind, buffer, 0, static_cast<uint32_t>(n));
    *result = Value::make_object(view);
    for (uint32_t i = 0; i < static_cast<uint32_t>(n); ++i) {
      Value v;
      if (source->okind == ObjKind::TypedArray) {
        if (!typed_array_get_value(vm, source, i, &v)) return false;
      } else if (!vm.get_member(Value::make_object(items ? items : source), num_value(i), &v)) {
        return false;
      }
      if (!store_value(vm, kind, data_of(cell_of(buffer)) + static_cast<uint64_t>(i) * size, v)) return false;
    }
    return true;
  }
  double n = 0;
  if (!first.is_undefined() && !vm.to_number(first, &n)) return false;
  if (n != n) n = 0;
  if (!(n >= 0) || n * size > static_cast<double>(kMaxBytes)) return vm.throw_error(ErrorKind::RangeError, "Invalid typed array length");
  JsObject* buffer = new_buffer_object(vm, new_zeroed_cell(vm, static_cast<uint64_t>(n) * size));
  *result = Value::make_object(new_view(vm, kind, buffer, 0, static_cast<uint32_t>(n)));
  return true;
}

#define TYPED_CONSTRUCTOR(Name, Kind) \
  NATIVE(Name) {                      \
    (void)self;                       \
    return construct(vm, ArrayKind::Kind, args, argc, result); \
  }
TYPED_CONSTRUCTOR(ctor_int8, Int8)
TYPED_CONSTRUCTOR(ctor_uint8, Uint8)
TYPED_CONSTRUCTOR(ctor_uint8_clamped, Uint8Clamped)
TYPED_CONSTRUCTOR(ctor_int16, Int16)
TYPED_CONSTRUCTOR(ctor_uint16, Uint16)
TYPED_CONSTRUCTOR(ctor_int32, Int32)
TYPED_CONSTRUCTOR(ctor_uint32, Uint32)
TYPED_CONSTRUCTOR(ctor_float32, Float32)
TYPED_CONSTRUCTOR(ctor_float64, Float64)
TYPED_CONSTRUCTOR(ctor_bigint64, BigInt64)
TYPED_CONSTRUCTOR(ctor_biguint64, BigUint64)
#undef TYPED_CONSTRUCTOR

// The bytes of a resizable buffer are allocated for its largest size; the length of the cell is the current one (and resizing changes it in place).
JsObject* new_resizable_buffer(Vm& vm, uint64_t length, uint64_t max) {
  JsString* cell = new_zeroed_cell(vm, max);
  if (max != 0) cell->length = static_cast<uint32_t>(length);
  JsObject* b = new_buffer_object(vm, cell);
  b->position = static_cast<uint32_t>(max + 1);
  return b;
}

NATIVE(typed_array_abstract) {
  (void)self; (void)args; (void)argc; (void)result;
  return vm.throw_error(ErrorKind::TypeError, "Abstract class TypedArray not directly constructable");
}

NATIVE(array_buffer_constructor) {
  (void)self;
  double n = 0;
  if (argc > 0 && !args[0].is_undefined() && !vm.to_number(args[0], &n)) return false;
  if (n != n) n = 0;
  if (!(n >= 0) || n > static_cast<double>(kMaxBytes)) return vm.throw_error(ErrorKind::RangeError, "Array buffer allocation failed");
  bool resizable = false;
  double max = 0;
  if (argc > 1 && args[1].is_object()) {   // new ArrayBuffer(length, { maxByteLength })
    Value m;
    if (!vm.get_named(args[1], "maxByteLength", &m)) return false;
    if (!m.is_undefined()) {
      if (!vm.to_number(m, &max)) return false;
      if (max != max) max = 0;
      if (!(max >= 0) || max > static_cast<double>(kMaxBytes)) return vm.throw_error(ErrorKind::RangeError, "Invalid array buffer max length");
      if (n > max) return vm.throw_error(ErrorKind::RangeError, "Invalid array buffer max length");
      resizable = true;
    }
  }
  if (!resizable) {
    *result = Value::make_object(new_buffer_object(vm, new_zeroed_cell(vm, static_cast<uint64_t>(n))));
    return true;
  }
  *result = Value::make_object(new_resizable_buffer(vm, static_cast<uint64_t>(n), static_cast<uint64_t>(max)));
  return true;
}

NATIVE(array_buffer_resize) {
  if (!is_buffer(self) || !buffer_resizable(self.as_object())) return vm.throw_error(ErrorKind::TypeError, "Method ArrayBuffer.prototype.resize called on incompatible receiver #<ArrayBuffer>");
  JsObject* b = self.as_object();
  if (buffer_detached(b)) return vm.throw_error(ErrorKind::TypeError, "Cannot perform ArrayBuffer.prototype.resize on a detached ArrayBuffer");
  double n = 0;
  if (argc > 0 && !vm.to_number(args[0], &n)) return false;
  if (n != n) n = 0;
  if (!(n >= 0) || n > static_cast<double>(buffer_max(b))) return vm.throw_error(ErrorKind::RangeError, "ArrayBuffer.prototype.resize: Invalid length parameter");
  JsString* cell = cell_of(b);
  const uint32_t from = cell->length, to = static_cast<uint32_t>(n);
  if (to < from) {   // (what is cut off is gone: growing again gives zeros)
    uint8_t* d = data_of(cell);
    for (uint32_t i = to; i < from; ++i) d[i] = 0;
  }
  cell->length = to;
  *result = Value();
  return true;
}

// buffer.transfer(newLength) and transferToFixedLength(newLength): the bytes move to a new buffer and this one is detached.
bool transfer_buffer(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result, bool fixed) {
  if (!is_buffer(self)) return vm.throw_error(ErrorKind::TypeError, "Method ArrayBuffer.prototype.transfer called on incompatible receiver");
  JsObject* b = self.as_object();
  if (b->length == 1) return vm.throw_error(ErrorKind::TypeError, "Cannot perform ArrayBuffer.prototype.transfer on a WebAssembly.Memory buffer");
  if (buffer_detached(b)) return vm.throw_error(ErrorKind::TypeError, "Cannot perform ArrayBuffer.prototype.transfer on a detached ArrayBuffer");
  JsString* cell = cell_of(b);
  double n = cell->length;
  if (argc > 0 && !args[0].is_undefined()) {
    if (!vm.to_number(args[0], &n)) return false;
    if (n != n) n = 0;
    if (!(n >= 0) || n > static_cast<double>(kMaxBytes)) return vm.throw_error(ErrorKind::RangeError, "Invalid array buffer length");
  }
  const bool keep_resizable = buffer_resizable(b) && !fixed;
  if (keep_resizable && n > static_cast<double>(buffer_max(b))) return vm.throw_error(ErrorKind::RangeError, "Invalid array buffer length");
  const uint64_t length = static_cast<uint64_t>(n);
  JsObject* fresh = keep_resizable ? new_resizable_buffer(vm, length, buffer_max(b)) : new_buffer_object(vm, new_zeroed_cell(vm, length));
  const uint64_t copy = length < cell->length ? length : cell->length;
  uint8_t* to = data_of(cell_of(fresh));
  const uint8_t* from = data_of(cell);
  for (uint64_t i = 0; i < copy; ++i) to[i] = from[i];
  cell->length = 0;   // (detached: every view of it is now empty)
  b->capacity = 1;
  *result = Value::make_object(fresh);
  return true;
}
NATIVE(array_buffer_transfer) { return transfer_buffer(vm, self, args, argc, result, false); }
NATIVE(array_buffer_transfer_fixed) { return transfer_buffer(vm, self, args, argc, result, true); }

NATIVE(array_buffer_is_view) {
  (void)vm;
  (void)self;
  const Value v = arg_at(args, argc, 0);
  *result = Value::make_bool(is_typed_array(v) || (v.is_object() && v.as_object()->host_data == nullptr && false));
  if (v.is_object()) {
    Value flag;
    if (vm.get_plain(v.as_object(), vm.intern("__dataview"), &flag)) *result = Value::make_bool(true);
  }
  return true;
}

// buffer.slice(begin, end)
NATIVE(array_buffer_slice) {
  if (!is_buffer(self)) return vm.throw_error(ErrorKind::TypeError, "Method ArrayBuffer.prototype.slice called on incompatible receiver");
  JsString* cell = cell_of(self.as_object());
  double from = 0, to = cell->length;
  if (argc > 0 && !args[0].is_undefined() && !vm.to_number(args[0], &from)) return false;
  if (argc > 1 && !args[1].is_undefined() && !vm.to_number(args[1], &to)) return false;
  if (from != from) from = 0;
  if (to != to) to = 0;
  if (from < 0) from += cell->length;
  if (to < 0) to += cell->length;
  if (from < 0) from = 0;
  if (to > cell->length) to = cell->length;
  if (to < from) to = from;
  JsString* copy = vm.new_string(StringView(cell->data() + static_cast<uint64_t>(from), static_cast<uint64_t>(to - from)));
  *result = Value::make_object(new_buffer_object(vm, copy));
  return true;
}

// --- TypedArray.prototype: what needs the bytes -----------------------------------------------------------------------------

bool typed_self(Vm& vm, const Value& self, JsObject** out) {
  if (!is_typed_array(self)) return vm.throw_error(ErrorKind::TypeError, "this is not a typed array.");
  *out = self.as_object();
  return true;
}

uint8_t* element_ptr(JsObject* o, uint32_t index) { return data_of(cell_of(o)) + o->position + static_cast<uint64_t>(index) * size_of(kind_of(o)); }

// array.set(source, offset)
NATIVE(typed_set) {
  JsObject* o;
  if (!typed_self(vm, self, &o)) return false;
  const Value source = arg_at(args, argc, 0);
  double offset = 0;
  if (argc > 1 && !args[1].is_undefined() && !vm.to_number(args[1], &offset)) return false;
  if (!(offset >= 0)) return vm.throw_error(ErrorKind::RangeError, "offset is out of bounds");
  double n = 0;
  if (is_typed_array(source)) {
    n = live_length(source.as_object());
  } else {
    Value len;
    if (!vm.get(source, vm.length_name(), &len) || !vm.to_number(len, &n)) return false;
    if (!(n >= 0)) n = 0;
  }
  if (offset + n > live_length(o)) return vm.throw_error(ErrorKind::RangeError, "offset is out of bounds");
  const uint32_t start = static_cast<uint32_t>(offset), count = static_cast<uint32_t>(n);
  if (is_typed_array(source)) {   // (the source may overlap: it is read first)
    List<Value> values(vm.allocator());
    for (uint32_t i = 0; i < count; ++i) {
      Value v;
      if (!typed_array_get_value(vm, source.as_object(), i, &v)) return false;
      values.push_back(v);
    }
    for (uint32_t i = 0; i < count; ++i)
      if (!store_value(vm, kind_of(o), element_ptr(o, start + i), values[i])) return false;
  } else {
    for (uint32_t i = 0; i < count; ++i) {
      Value v;
      if (!vm.get_member(source, num_value(i), &v)) return false;
      if (!store_value(vm, kind_of(o), element_ptr(o, start + i), v)) return false;
    }
  }
  *result = Value();
  return true;
}

void clamp_range(const Value* args, uint32_t argc, Vm& vm, uint32_t length, uint32_t* from, uint32_t* to, bool* ok) {
  double f = 0, t = length;
  *ok = true;
  if (argc > 0 && !args[0].is_undefined() && !vm.to_number(args[0], &f)) *ok = false;
  if (argc > 1 && !args[1].is_undefined() && !vm.to_number(args[1], &t)) *ok = false;
  if (f != f) f = 0;
  if (t != t) t = 0;
  if (f < 0) f += length;
  if (t < 0) t += length;
  if (f < 0) f = 0;
  if (f > length) f = length;
  if (t > length) t = length;
  if (t < f) t = f;
  *from = static_cast<uint32_t>(f);
  *to = static_cast<uint32_t>(t);
}

// array.subarray(begin, end): a view of the same buffer
NATIVE(typed_subarray) {
  JsObject* o;
  if (!typed_self(vm, self, &o)) return false;
  uint32_t from, to;
  bool ok;
  clamp_range(args, argc, vm, live_length(o), &from, &to, &ok);
  if (!ok) return false;
  Value buffer;
  vm.get_plain(o, vm.intern("buffer"), &buffer);
  *result = Value::make_object(new_view(vm, kind_of(o), buffer.as_object(), o->position + from * size_of(kind_of(o)), to - from));
  return true;
}

// array.slice(begin, end): a copy
NATIVE(typed_slice) {
  JsObject* o;
  if (!typed_self(vm, self, &o)) return false;
  uint32_t from, to;
  bool ok;
  clamp_range(args, argc, vm, live_length(o), &from, &to, &ok);
  if (!ok) return false;
  const uint32_t size = size_of(kind_of(o));
  JsString* copy = vm.new_string(bytes_as_text(ConstByteView(element_ptr(o, from), static_cast<uint64_t>(to - from) * size)));
  *result = Value::make_object(new_view(vm, kind_of(o), new_buffer_object(vm, copy), 0, to - from));
  return true;
}

// array.fill(value, begin, end)
NATIVE(typed_fill) {
  JsObject* o;
  if (!typed_self(vm, self, &o)) return false;
  uint32_t from, to;
  bool ok;
  clamp_range(argc > 1 ? args + 1 : nullptr, argc > 1 ? argc - 1 : 0, vm, live_length(o), &from, &to, &ok);
  if (!ok) return false;
  for (uint32_t i = from; i < to; ++i)
    if (!store_value(vm, kind_of(o), element_ptr(o, i), argc > 0 ? args[0] : Value())) return false;
  *result = self;
  return true;
}

// --- DataView -----------------------------------------------------------------------------------------------------------------

struct ViewParts {
  JsString* cell;
  uint64_t offset, length;
};

bool view_parts(Vm& vm, const Value& self, ViewParts* parts) {
  Value flag, buffer, offset, length;
  if (!self.is_object() || !vm.get_plain(self.as_object(), vm.intern("__dataview"), &flag) || !vm.get_plain(self.as_object(), vm.intern("buffer"), &buffer) ||
      !vm.get_plain(self.as_object(), vm.intern("byteOffset"), &offset) || !vm.get_plain(self.as_object(), vm.intern("byteLength"), &length) || !is_buffer(buffer))
    return vm.throw_error(ErrorKind::TypeError, "Receiver is not a DataView");
  parts->cell = cell_of(buffer.as_object());
  parts->offset = static_cast<uint64_t>(offset.u.number);
  parts->length = static_cast<uint64_t>(length.u.number);
  return true;
}

NATIVE(data_view_constructor) {
  (void)self;
  const Value buffer = arg_at(args, argc, 0);
  if (!is_buffer(buffer)) return vm.throw_error(ErrorKind::TypeError, "First argument to DataView constructor must be an ArrayBuffer");
  const uint64_t total = cell_of(buffer.as_object())->length;
  double offset = 0, length = -1;
  if (argc > 1 && !args[1].is_undefined() && !vm.to_number(args[1], &offset)) return false;
  if (argc > 2 && !args[2].is_undefined() && !vm.to_number(args[2], &length)) return false;
  if (!(offset >= 0) || offset > static_cast<double>(total)) return vm.throw_error(ErrorKind::RangeError, "Start offset is outside the bounds of the buffer");
  if (length < 0) length = static_cast<double>(total) - offset;
  if (offset + length > static_cast<double>(total)) return vm.throw_error(ErrorKind::RangeError, "Invalid DataView length");
  Value proto;
  vm.get_named(Value::make_object(vm.global()), "__data_view_proto", &proto);
  JsObject* v = vm.new_object(proto.is_object() ? proto.as_object() : nullptr);
  vm.define_named(v, "__dataview", Value::make_bool(true), 0);
  vm.define_named(v, "buffer", buffer, 0);
  vm.define_named(v, "byteOffset", num_value(offset), 0);
  vm.define_named(v, "byteLength", num_value(length), 0);
  *result = Value::make_object(v);
  return true;
}

bool data_view_access(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result, ArrayKind kind, bool writing) {
  ViewParts parts;
  if (!view_parts(vm, self, &parts)) return false;
  double at = 0;
  if (!vm.to_number(arg_at(args, argc, 0), &at)) return false;
  if (at != at) at = 0;
  const uint32_t size = size_of(kind);
  if (!(at >= 0) || at + size > static_cast<double>(parts.length)) return vm.throw_error(ErrorKind::RangeError, "Offset is outside the bounds of the DataView");
  uint8_t* p = data_of(parts.cell) + parts.offset + static_cast<uint64_t>(at);
  if (writing) {
    if (!store_value(vm, kind, p, arg_at(args, argc, 1), Vm::to_boolean(arg_at(args, argc, 2)))) return false;
    *result = Value();
  } else {
    *result = load_value(vm, kind, p, Vm::to_boolean(arg_at(args, argc, 1)));
  }
  return true;
}

#define DATA_VIEW_ACCESSORS(Name, Kind)                                                                       \
  NATIVE(get_##Name) { return data_view_access(vm, self, args, argc, result, ArrayKind::Kind, false); }     \
  NATIVE(set_##Name) { return data_view_access(vm, self, args, argc, result, ArrayKind::Kind, true); }
DATA_VIEW_ACCESSORS(int8, Int8)
DATA_VIEW_ACCESSORS(uint8, Uint8)
DATA_VIEW_ACCESSORS(int16, Int16)
DATA_VIEW_ACCESSORS(uint16, Uint16)
DATA_VIEW_ACCESSORS(int32, Int32)
DATA_VIEW_ACCESSORS(uint32, Uint32)
DATA_VIEW_ACCESSORS(float32, Float32)
DATA_VIEW_ACCESSORS(float64, Float64)
DATA_VIEW_ACCESSORS(bigint64, BigInt64)
DATA_VIEW_ACCESSORS(biguint64, BigUint64)
#undef DATA_VIEW_ACCESSORS

}  // namespace

// --- The interface --------------------------------------------------------------------------------------------------------------

bool is_typed_array(const Value& v) { return v.is_object() && v.as_object()->okind == ObjKind::TypedArray; }

bool typed_array_bytes(const Value& v, ArrayView<uint8_t>* bytes) {
  if (!v.is_object()) return false;
  JsObject* o = v.as_object();
  if (o->okind == ObjKind::TypedArray) {
    *bytes = ArrayView<uint8_t>(element_ptr(o, 0), static_cast<uint64_t>(live_length(o)) * size_of(kind_of(o)));
    return true;
  }
  if (o->host == &kBufferClass) {
    *bytes = ArrayView<uint8_t>(data_of(cell_of(o)), cell_of(o)->length);
    return true;
  }
  return false;
}

// (the buffer of a WebAssembly memory: only the memory can detach it, so transfer() refuses; `length` 1 says so)
JsObject* new_array_buffer(Vm& vm, JsString* cell) {
  JsObject* b = new_buffer_object(vm, cell);
  b->length = 1;
  return b;
}

void detach_array_buffer(JsObject* buffer) {
  if (!is_buffer(Value::make_object(buffer))) return;
  cell_of(buffer)->length = 0;
  buffer->capacity = 1;
}

bool typed_array_get(JsObject* array, uint32_t index, Value* out) {
  if (index >= live_length(array)) {
    *out = Value();
    return false;
  }
  if (is_big(kind_of(array))) return false;   // (a BigInt needs the Vm: see typed_array_get_value; the interpreter asks that way)
  *out = num_value(load(kind_of(array), element_ptr(array, index)));
  return true;
}

bool typed_array_get_value(Vm& vm, JsObject* array, uint32_t index, Value* out) {
  if (index >= live_length(array)) {
    *out = Value();
    return true;
  }
  *out = load_value(vm, kind_of(array), element_ptr(array, index));
  return true;
}

bool typed_array_put(Vm& vm, JsObject* array, uint32_t index, const Value& value) {
  if (is_big(kind_of(array))) {   // (converted whether or not the index is in range)
    uint64_t bits;
    if (!bigint_bits(vm, value, &bits)) return false;
    if (index < array->length && live_length(array) != 0) write_bits(element_ptr(array, index), 8, bits, true);
    return true;
  }
  double d;
  if (!vm.to_number(value, &d)) return false;
  if (index < array->length && live_length(array) != 0) store(kind_of(array), element_ptr(array, index), d);
  return true;
}

void typed_array_describe(Vm& vm, JsObject* array, String* out, uint32_t depth) {
  (void)depth;
  out->append(kNames[static_cast<uint32_t>(kind_of(array))]);
  out->push_back('(');
  out->append_uint(array->length);
  out->append(") [");
  for (uint32_t i = 0; i < array->length && i < 100; ++i) {
    if (i) out->append(", ");
    Value v;
    typed_array_get_value(vm, array, i, &v);
    vm.describe(v, out, 1);
  }
  if (array->length > 100) out->append(", ...");
  out->push_back(']');
}

JsObject* new_typed_array(Vm& vm, ArrayKind kind, StringView bytes) {
  JsObject* buffer = new_buffer_object(vm, vm.new_string(bytes));
  return new_view(vm, kind, buffer, 0, static_cast<uint32_t>(bytes.size() / size_of(kind)));
}

JsObject* new_zero_typed_array(Vm& vm, ArrayKind kind, uint64_t length) {
  if (length * size_of(kind) > kMaxBytes) return nullptr;
  JsObject* buffer = new_buffer_object(vm, new_zeroed_cell(vm, length * size_of(kind)));
  return new_view(vm, kind, buffer, 0, static_cast<uint32_t>(length));
}

bool install_typed_arrays(Vm& vm) {
  JsObject* g = vm.global();
  // ArrayBuffer
  JsObject* buffer_proto = vm.new_plain();
  vm.define_native(buffer_proto, "slice", array_buffer_slice, 2);
  vm.define_native(buffer_proto, "resize", array_buffer_resize, 1);
  vm.define_native(buffer_proto, "transfer", array_buffer_transfer, 0);
  vm.define_native(buffer_proto, "transferToFixedLength", array_buffer_transfer_fixed, 0);
  JsObject* buffer_ctor = vm.new_native(array_buffer_constructor, "ArrayBuffer", 1);
  vm.define(buffer_ctor, vm.prototype_name(), Value::make_object(buffer_proto), 0);
  vm.define(buffer_proto, vm.intern("constructor"), Value::make_object(buffer_ctor), kWritable);
  vm.define_native(buffer_ctor, "isView", array_buffer_is_view, 1);
  vm.define_named(g, "ArrayBuffer", Value::make_object(buffer_ctor), kWritable);
  vm.define_named(g, "__array_buffer_proto", Value::make_object(buffer_proto), 0);
  // the typed arrays: one prototype for what they share, one for each kind
  JsObject* shared = vm.new_plain();
  vm.define_native(shared, "set", typed_set, 1);
  vm.define_native(shared, "subarray", typed_subarray, 2);
  vm.define_native(shared, "slice", typed_slice, 2);
  vm.define_native(shared, "fill", typed_fill, 1);
  vm.define_named(g, "__typed_array_proto", Value::make_object(shared), 0);
  JsObject* abstract_ctor = vm.new_native(typed_array_abstract, "TypedArray", 0);   // %TypedArray%: what every typed array constructor inherits from
  vm.define(abstract_ctor, vm.prototype_name(), Value::make_object(shared), 0);
  vm.define(shared, vm.intern("constructor"), Value::make_object(abstract_ctor), kWritable);
  JsObject* protos = vm.new_plain();
  vm.define_named(g, "__typed_protos", Value::make_object(protos), 0);
  static const NativeFn kConstructors[] = {ctor_int8, ctor_uint8, ctor_uint8_clamped, ctor_int16, ctor_uint16, ctor_int32, ctor_uint32, ctor_float32, ctor_float64, ctor_bigint64, ctor_biguint64};
  for (uint32_t k = 0; k < 11; ++k) {
    JsObject* proto = vm.new_object(shared);
    JsObject* ctor = vm.new_native(kConstructors[k], kNames[k], 3);
    vm.define(ctor, vm.prototype_name(), Value::make_object(proto), 0);
    vm.define(proto, vm.intern("constructor"), Value::make_object(ctor), kWritable);
    ctor->proto = abstract_ctor;
    vm.define_named(ctor, "BYTES_PER_ELEMENT", num_value(kSizes[k]), 0);
    vm.define_named(proto, "BYTES_PER_ELEMENT", num_value(kSizes[k]), 0);
    vm.define_named(g, kNames[k], Value::make_object(ctor), kWritable);
    vm.define_named(protos, kNames[k], Value::make_object(proto), 0);
  }
  // DataView
  JsObject* view_proto = vm.new_plain();
  struct Access {
    const char* name;
    NativeFn get, set;
  };
  static const Access kAccess[] = {{"Int8", get_int8, set_int8},       {"Uint8", get_uint8, set_uint8},     {"Int16", get_int16, set_int16},
                                   {"Uint16", get_uint16, set_uint16}, {"Int32", get_int32, set_int32},     {"Uint32", get_uint32, set_uint32},
                                   {"Float32", get_float32, set_float32}, {"Float64", get_float64, set_float64}, {"BigInt64", get_bigint64, set_bigint64},
                                   {"BigUint64", get_biguint64, set_biguint64}};
  for (const Access& a : kAccess) {
    String get_name(vm.allocator()), set_name(vm.allocator());
    get_name.append("get");
    get_name.append(a.name);
    set_name.append("set");
    set_name.append(a.name);
    vm.define_native(view_proto, get_name.view(), a.get, 1);
    vm.define_native(view_proto, set_name.view(), a.set, 2);
  }
  JsObject* view_ctor = vm.new_native(data_view_constructor, "DataView", 3);
  vm.define(view_ctor, vm.prototype_name(), Value::make_object(view_proto), 0);
  vm.define(view_proto, vm.intern("constructor"), Value::make_object(view_ctor), kWritable);
  vm.define_named(g, "DataView", Value::make_object(view_ctor), kWritable);
  vm.define_named(g, "__data_view_proto", Value::make_object(view_proto), 0);
  (void)g_protos_unused;
  return !vm.halted();
}

}  // namespace script
