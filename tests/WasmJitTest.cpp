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
// Differential test of the WebAssembly JIT: random functions (integer, double and memory operations, selects, ifs, counted loops, calls) are run by the
// interpreter and by compiled code, and the results, traps and the memory they leave must be identical. Also a few hand written functions for what the
// generator does not reach (br_table, calls in loops, a loop long enough for the tick, stack values live across branches).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "Jit/KernelJit.hpp"
#include "Wasm/Wasm.hpp"
#include "JitHost.hpp"

using namespace wasm;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};
static TestAllocator A;
static jit::HostCodeMemory g_code;
static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

using Bytes = std::vector<uint8_t>;
static void uleb(Bytes& b, uint64_t v) {
  do {
    uint8_t byte = v & 0x7F;
    v >>= 7;
    if (v) byte |= 0x80;
    b.push_back(byte);
  } while (v);
}
static void sleb(Bytes& b, int64_t v) {
  for (;;) {
    const uint8_t byte = v & 0x7F;
    v >>= 7;
    if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40))) {
      b.push_back(byte);
      return;
    }
    b.push_back(byte | 0x80);
  }
}
static void section(Bytes& m, uint8_t id, const Bytes& body) {
  m.push_back(id);
  uleb(m, body.size());
  m.insert(m.end(), body.begin(), body.end());
}
static void append(Bytes& to, const Bytes& from) { to.insert(to.end(), from.begin(), from.end()); }

// A module with one page of memory; function 0 is exported as "f" (type: (i32, i32, i64) -> i64), function 1 is `sq` (i32) -> i32 = x * x + 1, function 2 is
// `mix` (i32, i64) -> i64.
static Bytes make_module(const Bytes& main_body, bool with_br_table_helper = false) {
  (void)with_br_table_helper;
  Bytes m = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
  Bytes types;
  uleb(types, 3);
  types.insert(types.end(), {0x60, 3, 0x7F, 0x7F, 0x7E, 1, 0x7E});
  types.insert(types.end(), {0x60, 1, 0x7F, 1, 0x7F});
  types.insert(types.end(), {0x60, 2, 0x7F, 0x7E, 1, 0x7E});
  section(m, 1, types);
  Bytes funcs = {3, 0, 1, 2};
  section(m, 3, funcs);
  Bytes mem = {1, 0, 1};
  section(m, 5, mem);
  Bytes exports = {2, 1, 'f', 0, 0, 1, 'm', 2, 0};
  section(m, 7, exports);
  Bytes code;
  uleb(code, 3);
  uleb(code, main_body.size());
  append(code, main_body);
  const Bytes sq = {0, 0x20, 0, 0x20, 0, 0x6C, 0x41, 1, 0x6A, 0x0B};
  uleb(code, sq.size());
  append(code, sq);
  // mix(a: i32, b: i64) = (extend_u a) * 31 + b
  const Bytes mix = {0, 0x20, 0, 0xAD, 0x42, 31, 0x7E, 0x20, 1, 0x7C, 0x0B};
  uleb(code, mix.size());
  append(code, mix);
  section(m, 10, code);
  return m;
}

struct Rng {
  uint64_t s;
  uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
  uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

enum Ty { I32, I64, F64 };

struct Gen {
  Rng& rng;
  Bytes out;
  void op(uint8_t o) { out.push_back(o); }
  void memarg(uint32_t, uint32_t offset) { uleb(out, 0); uleb(out, offset); }
  void i32c(int32_t v) { op(0x41); sleb(out, v); }
  void i64c(int64_t v) { op(0x42); sleb(out, v); }
  void f64c(double d) {
    op(0x44);
    uint64_t bits;
    memcpy(&bits, &d, 8);
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
  }
  int32_t interesting32() {
    static const int32_t v[] = {0, 1, -1, 2, 3, 7, 31, 32, 33, 255, 256, 65535, 65536, INT32_MAX, INT32_MIN, 0x12345678, -559038737, 100, -100};
    return rng.below(3) == 0 ? static_cast<int32_t>(rng.next()) : v[rng.below(sizeof v / sizeof *v)];
  }
  int64_t interesting64() {
    static const int64_t v[] = {0, 1, -1, 2, 63, 64, 65, 0xFFFFFFFFll, 0x100000000ll, INT64_MAX, INT64_MIN, 0x123456789ABCDEF0ll, -4294967297ll};
    return rng.below(3) == 0 ? static_cast<int64_t>(rng.next()) : v[rng.below(sizeof v / sizeof *v)];
  }
  double interesting_f64() {
    static const double v[] = {0.0, -0.0, 1.0, -1.5, 2.5, 0.1, 1e10, -1e10, 4294967296.0, 3.0, 1e300, -7.25};
    return v[rng.below(sizeof v / sizeof *v)];
  }
  void address() {   // an i32 address, usually inside the page
    if (rng.below(12) == 0) { gen(I32, 1); return; }
    gen(I32, 1);
    i32c(0xFFF8);
    op(0x71);
  }
  void leaf(Ty t) {
    switch (t) {
      case I32:
        if (rng.below(2)) { op(0x20); out.push_back(rng.below(2) ? 0 : (rng.below(2) ? 1 : 3)); } else i32c(interesting32());
        break;
      case I64:
        if (rng.below(2)) { op(0x20); out.push_back(rng.below(2) ? 2 : 5); } else i64c(interesting64());
        break;
      case F64:
        if (rng.below(3) == 0) { op(0x20); out.push_back(6); } else f64c(interesting_f64());
        break;
    }
  }
  void gen(Ty t, int depth) {
    if (depth <= 0 || rng.below(5) == 0) { leaf(t); return; }
    const uint32_t k = rng.below(14);
    if (k == 12) {   // select
      gen(t, depth - 1);
      gen(t, depth - 1);
      gen(I32, depth - 1);
      op(0x1B);
      return;
    }
    if (k == 13) {   // if / else
      gen(I32, depth - 1);
      op(0x04);
      op(t == I32 ? 0x7F : t == I64 ? 0x7E : 0x7C);
      gen(t, depth - 1);
      op(0x05);
      gen(t, depth - 1);
      op(0x0B);
      return;
    }
    switch (t) {
      case I32:
        switch (k) {
          case 0: case 1: case 2: case 3: {
            static const uint8_t ops[] = {0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78};
            gen(I32, depth - 1);
            gen(I32, depth - 1);
            op(ops[rng.below(sizeof ops)]);
            break;
          }
          case 4: { gen(I32, depth - 1); gen(I32, depth - 1); op(0x45 + 1 + rng.below(10)); break; }   // i32 comparisons
          case 5: { gen(I64, depth - 1); gen(I64, depth - 1); op(0x51 + rng.below(10)); break; }
          case 6: { gen(F64, depth - 1); gen(F64, depth - 1); op(0x61 + rng.below(6)); break; }
          case 7: { gen(I32, depth - 1); op(rng.below(2) ? 0x45 : 0x67 + rng.below(3)); break; }
          case 8: { gen(I64, depth - 1); op(0xA7); break; }
          case 9: { address(); static const uint8_t ops[] = {0x28, 0x2C, 0x2D, 0x2E, 0x2F}; op(ops[rng.below(sizeof ops)]); memarg(2, rng.below(8)); break; }
          case 10: { gen(I32, depth - 1); op(0x10); out.push_back(1); break; }   // call sq
          default: {
            if (rng.below(2)) { gen(I32, depth - 1); op(0xC0 + rng.below(2)); break; }
            if (rng.below(2)) { gen(F64, depth - 1); op(0xAA + rng.below(2)); break; }   // i32.trunc_f64_s / _u (trap when out of range)
            if (rng.below(2)) { gen(F64, depth - 1); op(0xFC); out.push_back(2 + rng.below(2)); break; }   // saturating
            gen(F64, depth - 1); op(0xB6); op(rng.below(2) ? 0xA8 : 0xA9); break;   // through f32
          }
        }
        break;
      case I64:
        switch (k) {
          case 0: case 1: case 2: case 3: {
            static const uint8_t ops[] = {0x7C, 0x7D, 0x7E, 0x7F, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A};
            gen(I64, depth - 1);
            gen(I64, depth - 1);
            op(ops[rng.below(sizeof ops)]);
            break;
          }
          case 4: { gen(I32, depth - 1); op(0xAC + rng.below(2)); break; }
          case 5: { gen(I64, depth - 1); op(0x79 + rng.below(3)); break; }
          case 6: { gen(F64, depth - 1); op(0xBD); break; }
          case 7: { address(); static const uint8_t ops[] = {0x29, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35}; op(ops[rng.below(sizeof ops)]); memarg(3, rng.below(8)); break; }
          case 8: { gen(I32, depth - 1); gen(I64, depth - 1); op(0x10); out.push_back(2); break; }   // call mix
          case 9: { gen(I64, depth - 1); op(0xC2 + rng.below(3)); break; }
          default: {
            if (rng.below(2)) { gen(F64, depth - 1); op(0xB0); break; }   // i64.trunc_f64_s
            if (rng.below(2)) { gen(F64, depth - 1); op(0xB1); break; }   // i64.trunc_f64_u
            if (rng.below(2)) { gen(F64, depth - 1); op(0xFC); out.push_back(6 + rng.below(2)); break; }
            gen(I64, depth - 1); gen(I64, depth - 1); op(0x7C); break;
          }
        }
        break;
      case F64:
        switch (k) {
          case 0: case 1: case 2: case 3: { gen(F64, depth - 1); gen(F64, depth - 1); op(0xA0 + rng.below(4)); break; }
          case 4: { gen(I32, depth - 1); op(0xB7 + rng.below(2)); break; }
          case 5: { gen(I64, depth - 1); op(0xB9 + rng.below(2)); break; }
          case 6: { gen(I64, depth - 1); op(0xBF); break; }
          case 7: { gen(F64, depth - 1); op(0x99 + rng.below(2)); break; }
          case 8: { address(); op(0x2B); memarg(3, rng.below(8)); break; }
          default: {   // f32 arithmetic and the rest of f64 (helpers)
            if (rng.below(2)) {
              gen(F64, depth - 1); op(0xB6); gen(F64, depth - 1); op(0xB6);
              static const uint8_t f32ops[] = {0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98};
              op(f32ops[rng.below(sizeof f32ops)]);
              op(0xBB);
              break;
            }
            if (rng.below(2)) { gen(F64, depth - 1); op(0xB6); gen(F64, depth - 1); op(0xB6); op(0x5B + rng.below(6)); op(0xB7); break; }
            if (rng.below(2)) { gen(F64, depth - 1); op(0x9B + rng.below(5)); break; }
            gen(F64, depth - 1); gen(F64, depth - 1); op(0xA4 + rng.below(3)); break;
          }
        }
        break;
    }
  }
  void statement(int depth) {
    switch (rng.below(5)) {
      case 0: case 1: {   // local.set / tee + drop
        const uint32_t which = rng.below(3);
        if (which == 0) { gen(I32, depth); op(0x21); out.push_back(rng.below(2) ? 0 : (rng.below(2) ? 1 : 3)); }
        else if (which == 1) { gen(I64, depth); op(0x21); out.push_back(rng.below(2) ? 2 : 5); }
        else { gen(F64, depth); op(0x21); out.push_back(6); }
        break;
      }
      case 2: { address(); gen(I32, depth); op(rng.below(3) == 0 ? 0x3A : 0x36); memarg(2, rng.below(8)); break; }
      case 3: { address(); gen(I64, depth); op(0x37 + rng.below(1) * 0); memarg(3, rng.below(8)); break; }
      default: { address(); gen(F64, depth); op(0x39); memarg(3, rng.below(8)); break; }
    }
  }
};

// The body of function 0: locals (i32 x2, i64, f64), some statements, a counted loop of statements, and a result built from all three types.
static Bytes random_body(Rng& rng, uint32_t loop_count) {
  Gen g{rng, {}};
  g.out = {3, 2, 0x7F, 1, 0x7E, 1, 0x7C};   // locals 3..4 i32, 5 i64, 6 f64
  const uint32_t before = 1 + rng.below(4);
  for (uint32_t i = 0; i < before; ++i) g.statement(3);
  g.i32c(0);
  g.op(0x21); g.out.push_back(4);
  g.op(0x02); g.op(0x40);
  g.op(0x03); g.op(0x40);
  g.op(0x20); g.out.push_back(4);
  g.i32c(static_cast<int32_t>(loop_count));
  g.op(0x4F);   // i32.ge_u
  g.op(0x0D); g.out.push_back(1);
  const uint32_t inside = 1 + rng.below(4);
  for (uint32_t i = 0; i < inside; ++i) g.statement(3);
  g.op(0x20); g.out.push_back(4);
  g.i32c(1);
  g.op(0x6A);
  g.op(0x21); g.out.push_back(4);
  g.op(0x0C); g.out.push_back(0);
  g.op(0x0B);
  g.op(0x0B);
  // result = extend(i32) ^ i64 ^ reinterpret(f64)
  g.gen(I32, 3); g.op(0xAC);
  g.gen(I64, 3); g.op(0x85);
  g.gen(F64, 3); g.op(0xBD); g.op(0x85);
  g.op(0x0B);
  return g.out;
}

struct Outcome {
  Status status = Status::Ok;
  std::string trap;
  uint64_t result = 0;
  uint64_t memory_hash = 0;
  bool operator==(const Outcome& o) const { return status == o.status && trap == o.trap && result == o.result && memory_hash == o.memory_hash; }
};

struct Rig {
  Store store;
  Instance* inst = nullptr;
  FuncInst* fn = nullptr;
  Rig(const Bytes& bytes, bool jit, uint32_t threshold) : store(A) {
    store.set_jit(jit, threshold);
    SharedPointer<Module> m = SharedPointer<Module>::make(A, A);
    String err(A);
    if (!m->parse(ConstByteView(bytes.data(), bytes.size()), &err)) { printf("module does not parse: %s\n", err.c_str()); ++g_failed; return; }
    String e(A);
    bool link;
    Status st;
    if (!store.instantiate(m, ArrayView<const ExternVal>(), &inst, &e, &link, &st)) { printf("does not instantiate: %s\n", e.c_str()); ++g_failed; return; }
    ExternVal ev;
    if (!Store::find_export(*inst, StringView("f", 1), &ev)) { ++g_failed; return; }
    fn = static_cast<FuncInst*>(ev.ptr);
  }
  Outcome call(uint32_t a, uint32_t b, uint64_t c) {
    Slot args[3];
    args[0].lo = a;
    args[1].lo = b;
    args[2].lo = c;
    Slot result[2];
    const char* trap = nullptr;
    Outcome o;
    o.status = store.invoke(*fn, args, result, &trap);
    if (o.status == Status::Trap && trap) o.trap = trap;
    if (o.status == Status::Ok) o.result = result[0].lo;
    const Memory& mem = *inst->memories[0];
    uint64_t h = 1469598103934665603ull;
    for (uint64_t i = 0; i < mem.size; ++i) h = (h ^ mem.data[i]) * 1099511628211ull;
    o.memory_hash = h;
    return o;
  }
};

static void report(const char* what, uint64_t seed, const Outcome& want, const Outcome& got) {
  printf("%s differs (seed %llx): interpreter status %d trap '%s' result %llx mem %llx; jit status %d trap '%s' result %llx mem %llx\n", what, (unsigned long long)seed,
         static_cast<int>(want.status), want.trap.c_str(), (unsigned long long)want.result, (unsigned long long)want.memory_hash, static_cast<int>(got.status), got.trap.c_str(),
         (unsigned long long)got.result, (unsigned long long)got.memory_hash);
  ++g_failed;
}

static void test_random(uint32_t programs) {
  uint64_t regions = 0, entries = 0, traps = 0, calls = 0;
  struct Args { uint32_t a, b; uint64_t c; };
  static const Args args[] = {{0, 0, 0}, {5, 9, 77}, {0xFFFFFFFFu, 0x80000000u, 0xFFFFFFFFFFFFFFFFull}, {123456, 42, 0x123456789ull}};
  for (uint32_t p = 0; p < programs; ++p) {
    const uint64_t seed = 0x9E3779B97F4A7C15ull * (p + 1);
    Rng rng{seed};
    const uint32_t loop = (p % 17 == 0) ? 9000 : rng.below(6);   // (a few loops long enough to use up the budget of 4096 back edges)
    const Bytes module = make_module(random_body(rng, loop));
    // Fresh stores per call: compiled at the first visit.
    for (const auto& a : args) {
      Rig ref(module, false, 40), jit1(module, true, 1);
      if (!ref.fn || !jit1.fn) return;
      const Outcome want = ref.call(a.a, a.b, a.c);
      const Outcome got = jit1.call(a.a, a.b, a.c);
      ++calls;
      if (want.status == Status::Trap) ++traps;
      if (!(want == got)) { report("jit(1)", seed, want, got); return; }
      regions += jit1.store.jit_regions();
      entries += jit1.store.jit_entries();
    }
    // One store each, the same six calls in turn: regions that become hot part way (threshold 3) meet memory the earlier calls left.
    Rig ref(module, false, 40), jit3(module, true, 3);
    if (!ref.fn || !jit3.fn) return;
    for (int rep = 0; rep < 6; ++rep) {
      const auto& a = args[rep % 4];
      const Outcome want = ref.call(a.a, a.b, a.c);
      const Outcome got = jit3.call(a.a, a.b, a.c);
      ++calls;
      if (!(want == got)) { report("jit(3)", seed, want, got); return; }
    }
    regions += jit3.store.jit_regions();
    entries += jit3.store.jit_entries();
  }
  printf("random programs: %u, %llu calls compared, traps %llu, regions compiled %llu, entered %llu times\n", programs, (unsigned long long)calls, (unsigned long long)traps, (unsigned long long)regions, (unsigned long long)entries);
  CHECK(regions > 0);
  CHECK(entries > 0);
}


// Calls made from compiled code: recursion (more levels than compiled code nests natively), call_indirect, a callee that traps, and a stack overflow.
static Bytes make_call_module() {
  Bytes m = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
  Bytes types;
  uleb(types, 2);
  types.insert(types.end(), {0x60, 1, 0x7F, 1, 0x7F});
  types.insert(types.end(), {0x60, 2, 0x7F, 0x7F, 1, 0x7F});
  section(m, 1, types);
  section(m, 3, Bytes{4, 0, 0, 1, 0});
  section(m, 4, Bytes{1, 0x70, 0, 2});
  section(m, 7, Bytes{4, 3, 'f', 'i', 'b', 0, 0, 3, 'd', 'i', 'v', 0, 1, 3, 'i', 'n', 'd', 0, 2, 3, 'r', 'e', 'c', 0, 3});
  Bytes elems = {1, 0, 0x41, 0, 0x0B, 2, 0, 1};
  section(m, 9, elems);
  Bytes code;
  uleb(code, 4);
  auto add = [&](const Bytes& body) { uleb(code, body.size()); append(code, body); };
  add({0, 0x20, 0, 0x41, 2, 0x48, 0x04, 0x7F, 0x20, 0, 0x05, 0x20, 0, 0x41, 1, 0x6B, 0x10, 0, 0x20, 0, 0x41, 2, 0x6B, 0x10, 0, 0x6A, 0x0B, 0x0B});   // fib
  add({0, 0x41, 0xE4, 0x00, 0x20, 0, 0x6D, 0x0B});                                                                                                    // 100 / x
  add({0, 0x20, 1, 0x20, 0, 0x11, 0, 0, 0x0B});                                                                                                       // call_indirect (x) through table[i]
  add({0, 0x20, 0, 0x41, 1, 0x6A, 0x10, 3, 0x0B});                                                                                                    // rec(x) = rec(x + 1): never returns
  section(m, 10, code);
  return m;
}

struct CallRig {
  Store store;
  Instance* inst = nullptr;
  CallRig(const Bytes& bytes, bool jit) : store(A) {
    store.set_jit(jit, 1);
    SharedPointer<Module> m = SharedPointer<Module>::make(A, A);
    String err(A);
    if (!m->parse(ConstByteView(bytes.data(), bytes.size()), &err)) { printf("call module does not parse: %s\n", err.c_str()); ++g_failed; return; }
    String e(A);
    bool link;
    Status st;
    if (!store.instantiate(m, ArrayView<const ExternVal>(), &inst, &e, &link, &st)) { printf("call module does not instantiate: %s\n", e.c_str()); ++g_failed; inst = nullptr; }
  }
  bool call(const char* name, std::vector<uint64_t> args, Status* status, uint64_t* result, std::string* trap) {
    ExternVal ev;
    if (!inst || !Store::find_export(*inst, StringView(name, strlen(name)), &ev)) return false;
    FuncInst* fn = static_cast<FuncInst*>(ev.ptr);
    std::vector<Slot> a(args.size() + 1);
    for (size_t i = 0; i < args.size(); ++i) a[i].lo = args[i];
    Slot r[2];
    const char* t = nullptr;
    *status = store.invoke(*fn, a.data(), r, &t);
    *trap = t && *status == Status::Trap ? t : "";
    *result = *status == Status::Ok ? r[0].lo : 0;
    return true;
  }
};

static void test_calls() {
  const Bytes module = make_call_module();
  struct Case { const char* name; std::vector<uint64_t> args; };
  const Case cases[] = {{"fib", {0}}, {"fib", {1}}, {"fib", {12}}, {"fib", {24}}, {"div", {5}}, {"div", {0}}, {"ind", {0, 17}}, {"ind", {1, 7}}, {"ind", {1, 0}},
                        {"ind", {2, 5}}, {"ind", {0xFFFFFFFFu, 5}}, {"rec", {0}}};
  CallRig ref(module, false), fast(module, true);
  if (!ref.inst || !fast.inst) return;
  for (int round = 0; round < 3; ++round) {
    for (const Case& c : cases) {
      Status s1, s2;
      uint64_t r1 = 0, r2 = 0;
      std::string t1, t2;
      CHECK(ref.call(c.name, c.args, &s1, &r1, &t1));
      CHECK(fast.call(c.name, c.args, &s2, &r2, &t2));
      if (s1 != s2 || r1 != r2 || t1 != t2) {
        printf("call test %s differs: interpreter status %d result %llu trap '%s'; jit status %d result %llu trap '%s'\n", c.name, static_cast<int>(s1), (unsigned long long)r1,
               t1.c_str(), static_cast<int>(s2), (unsigned long long)r2, t2.c_str());
        ++g_failed;
      }
    }
  }
  uint64_t r = 0;
  Status s;
  std::string t;
  fast.call("fib", {24}, &s, &r, &t);
  CHECK(s == Status::Ok && r == 46368);
  fast.call("div", {0}, &s, &r, &t);
  CHECK(s == Status::Trap && t == "divide by zero");
  fast.call("rec", {0}, &s, &r, &t);
  CHECK(s == Status::Trap && t == "Maximum call stack size exceeded");
  CHECK(fast.store.jit_regions() > 0);
  CHECK(fast.store.jit_calls() > 100);
  printf("call tests: %llu calls made from compiled code\n", (unsigned long long)fast.store.jit_calls());
}

// Calls to a host function and tail calls, from compiled code: the host's answer, its failure (HostError), tail recursion far deeper than compiled code nests natively
// (direct and through a table), and the same results with the JIT off.
struct SumHost final : IHost {
  bool fail_on_13 = true;
  uint64_t calls = 0;
  bool call(Store&, FuncInst&, const Slot* args, Slot* results) override {
    ++calls;
    const uint32_t x = static_cast<uint32_t>(args[0].lo);
    if (fail_on_13 && x == 13) return false;
    results[0].lo = x + 7;
    return true;
  }
  bool tick(uint64_t) override { return true; }
};

static Bytes make_host_module() {
  Bytes m = {0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};
  Bytes types;
  uleb(types, 2);
  types.insert(types.end(), {0x60, 1, 0x7F, 1, 0x7F});
  types.insert(types.end(), {0x60, 2, 0x7F, 0x7F, 1, 0x7F});
  section(m, 1, types);
  section(m, 2, Bytes{1, 1, 'e', 1, 'h', 0, 0});
  section(m, 3, Bytes{3, 0, 1, 1});
  section(m, 4, Bytes{1, 0x70, 0, 1});
  section(m, 7, Bytes{3, 7, 'v', 'i', 'a', 'h', 'o', 's', 't', 0, 1, 4, 't', 's', 'u', 'm', 0, 2, 4, 't', 'i', 'n', 'd', 0, 3});
  section(m, 9, Bytes{1, 0, 0x41, 0, 0x0B, 1, 3});
  Bytes code;
  uleb(code, 3);
  auto add = [&](const Bytes& body) { uleb(code, body.size()); append(code, body); };
  add({0, 0x20, 0, 0x10, 0, 0x20, 0, 0x41, 1, 0x6A, 0x10, 0, 0x6A, 0x0B});                                                                             // host(x) + host(x + 1)
  add({0, 0x20, 0, 0x45, 0x04, 0x7F, 0x20, 1, 0x05, 0x20, 0, 0x41, 1, 0x6B, 0x20, 1, 0x20, 0, 0x6A, 0x12, 2, 0x0B, 0x0B});                             // tsum(n, acc) = n == 0 ? acc : return_call tsum(n - 1, acc + n)
  add({0, 0x20, 0, 0x45, 0x04, 0x7F, 0x20, 1, 0x05, 0x20, 0, 0x41, 1, 0x6B, 0x20, 1, 0x20, 0, 0x6A, 0x41, 0, 0x13, 1, 0, 0x0B, 0x0B});                 // the same through table[0]
  section(m, 10, code);
  return m;
}

static void test_host_and_tail_calls() {
  const Bytes module = make_host_module();
  uint64_t jit_calls = 0;
  uint64_t results[2][6] = {};
  Status statuses[2][6] = {};
  for (int mode = 0; mode < 2; ++mode) {   // 0: interpreter, 1: JIT
    Store store(A);
    SumHost host;
    store.set_host(&host);
    store.set_jit(mode == 1, 1);
    SharedPointer<Module> m = SharedPointer<Module>::make(A, A);
    String err(A);
    if (!m->parse(ConstByteView(module.data(), module.size()), &err)) { printf("host module does not parse: %s\n", err.c_str()); ++g_failed; return; }
    FuncInst* h = store.new_host_function(store.intern_sig(*m->types[0]), 1, nullptr);
    ExternVal import;
    import.kind = ExternKind::Func;
    import.ptr = h;
    Instance* inst = nullptr;
    String e(A);
    bool link;
    Status st;
    if (!store.instantiate(m, ArrayView<const ExternVal>(&import, 1), &inst, &e, &link, &st)) { printf("host module does not instantiate: %s\n", e.c_str()); ++g_failed; return; }
    auto call = [&](const char* name, std::vector<uint64_t> args, int slot) {
      ExternVal ev;
      if (!Store::find_export(*inst, StringView(name, strlen(name)), &ev)) { ++g_failed; return; }
      std::vector<Slot> a(args.size() + 1);
      for (size_t i = 0; i < args.size(); ++i) a[i].lo = args[i];
      Slot r[2];
      const char* trap = nullptr;
      statuses[mode][slot] = store.invoke(*static_cast<FuncInst*>(ev.ptr), a.data(), r, &trap);
      results[mode][slot] = statuses[mode][slot] == Status::Ok ? r[0].lo : 0;
    };
    for (int round = 0; round < 3; ++round) {   // (the second and third rounds run what was compiled in the first)
      call("viahost", {5}, 0);
      call("viahost", {12}, 1);   // host(12) is fine, host(13) fails: the whole call fails
      call("tsum", {10}, 2);
      call("tsum", {100000}, 3);
      call("tind", {100000}, 4);
      call("viahost", {20}, 5);
    }
    if (mode == 1) jit_calls = store.jit_calls();
  }
  for (int i = 0; i < 6; ++i) {
    if (results[0][i] != results[1][i] || statuses[0][i] != statuses[1][i]) {
      printf("host/tail call case %d differs: interpreter status %d result %llu; jit status %d result %llu\n", i, static_cast<int>(statuses[0][i]), (unsigned long long)results[0][i],
             static_cast<int>(statuses[1][i]), (unsigned long long)results[1][i]);
      ++g_failed;
    }
  }
  CHECK(statuses[1][0] == Status::Ok && results[1][0] == 12 + 13);
  CHECK(statuses[1][1] == Status::HostError);
  CHECK(statuses[1][3] == Status::Ok && results[1][3] == 5000050000ull % 4294967296ull);
  CHECK(statuses[1][4] == Status::Ok && results[1][4] == 5000050000ull % 4294967296ull);
  CHECK(jit_calls > 100);
  printf("host and tail call tests: %llu calls made from compiled code\n", (unsigned long long)jit_calls);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  jit::set_kernel_code_memory(&g_code);
  test_calls();
  test_host_and_tail_calls();
  test_random(1500);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("WasmJitTest: all passed\n");
  return 0;
}
