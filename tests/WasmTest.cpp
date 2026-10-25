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

// Tests of the WebAssembly runtime (src/Wasm). tests/data/wasm_vectors.txt has modules with what a reference engine gives for calls into them (see
// tests/gen_wasm_vectors.js): every line is replayed here. The rest are tests of linking, host functions, traps, limits and of decoding damaged modules.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "Wasm/Wasm.hpp"

using namespace wasm;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static TestAllocator A;

static std::vector<uint8_t> from_hex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
  return out;
}

static bool parse_module(const std::vector<uint8_t>& bytes, SharedPointer<Module>* out, std::string* error = nullptr) {
  SharedPointer<Module> m = SharedPointer<Module>::make(A, A);
  String err(A);
  const bool ok = m->parse(ConstByteView(bytes.data(), bytes.size()), &err);
  if (error) *error = err.c_str();
  if (ok && out) *out = m;
  return ok;
}

// --- the reference vectors ---------------------------------------------------------------------------------------------------------------

struct Value {
  ValType type;
  uint64_t bits;
};

static bool parse_value(const std::string& tok, Value* v) {
  const size_t colon = tok.find(':');
  if (colon == std::string::npos) return false;
  const std::string kind = tok.substr(0, colon), text = tok.substr(colon + 1);
  if (kind == "i32") { v->type = ValType::I32; v->bits = static_cast<uint32_t>(static_cast<int32_t>(std::stoll(text))); return true; }
  if (kind == "i64") { v->type = ValType::I64; v->bits = static_cast<uint64_t>(std::stoll(text)); return true; }
  if (kind == "f32") { v->type = ValType::F32; v->bits = std::stoull(text, nullptr, 16); return true; }
  if (kind == "f64") { v->type = ValType::F64; v->bits = std::stoull(text, nullptr, 16); return true; }
  return false;
}

static bool same_value(const Value& want, const Slot& got) {
  switch (want.type) {
    case ValType::I32: return static_cast<uint32_t>(got.lo) == want.bits;
    case ValType::F32: {
      const uint32_t w = static_cast<uint32_t>(want.bits), g = static_cast<uint32_t>(got.lo);
      const bool wn = (w & 0x7F800000u) == 0x7F800000u && (w & 0x7FFFFFu), gn = (g & 0x7F800000u) == 0x7F800000u && (g & 0x7FFFFFu);
      return (wn && gn) || w == g;
    }
    case ValType::F64: {
      const uint64_t w = want.bits, g = got.lo;
      const bool wn = (w & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (w & 0xFFFFFFFFFFFFFull), gn = (g & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (g & 0xFFFFFFFFFFFFFull);
      return (wn && gn) || w == g;
    }
    default: return got.lo == want.bits;
  }
}

static std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < line.size()) {
    size_t j = line.find(' ', i);
    if (j == std::string::npos) j = line.size();
    if (j > i) out.push_back(line.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

static void test_vectors(const char* path) {
  FILE* f = fopen(path, "rb");
  CHECK(f != nullptr);
  if (!f) return;
  Store store(A);
  Instance* inst = nullptr;
  std::string line;
  int number = 0, calls = 0, valids = 0, invalids = 0, modules = 0, reported = 0;
  static char buffer[1 << 25];
  std::vector<std::string> unsupported;
  while (fgets(buffer, sizeof(buffer), f)) {
    line = buffer;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    ++number;
    if (line.rfind("module ", 0) == 0) {
      SharedPointer<Module> m;
      std::string err;
      inst = nullptr;
      if (!parse_module(from_hex(line.substr(7)), &m, &err)) {
        printf("line %d: module does not parse: %s\n", number, err.c_str());
        ++g_failed;
        continue;
      }
      String e(A);
      bool link;
      Status st;
      if (!store.instantiate(m, ArrayView<const ExternVal>(), &inst, &e, &link, &st)) {
        printf("line %d: module does not instantiate: %s\n", number, e.c_str());
        ++g_failed;
        inst = nullptr;
      }
      ++modules;
    } else if (line.rfind("valid ", 0) == 0 || line.rfind("invalid ", 0) == 0) {
      const bool want = line[0] == 'v';
      std::string err;
      const bool got = parse_module(from_hex(line.substr(want ? 6 : 8)), nullptr, &err);
      if (got != want) {
        if (reported++ < 25) printf("line %d: reference says %s, we say %s (%s)\n", number, want ? "valid" : "invalid", got ? "valid" : "invalid", err.c_str());
        ++g_failed;
      }
      want ? ++valids : ++invalids;
    } else if (line.rfind("call ", 0) == 0) {
      if (!inst) continue;
      std::vector<std::string> t = split(line);
      size_t arrow = 0;
      while (arrow < t.size() && t[arrow] != "=>") ++arrow;
      ExternVal ev;
      if (!Store::find_export(*inst, StringView(t[1].c_str(), t[1].size()), &ev) || ev.kind != ExternKind::Func) {
        printf("line %d: no export %s\n", number, t[1].c_str());
        ++g_failed;
        continue;
      }
      FuncInst* fn = static_cast<FuncInst*>(ev.ptr);
      std::vector<Slot> args(fn->sig->nparams);
      bool ok = args.size() == arrow - 2;
      for (size_t i = 0; ok && i < args.size(); ++i) {
        Value v;
        ok = parse_value(t[2 + i], &v);
        args[i].lo = v.bits;
      }
      if (!ok) {
        printf("line %d: bad call line\n", number);
        ++g_failed;
        continue;
      }
      std::vector<Slot> results(fn->sig->nresults + 1);
      const char* trap = nullptr;
      const Status st = store.invoke(*fn, args.data(), results.data(), &trap);
      ++calls;
      bool good;
      if (t.size() == arrow + 2 && t[arrow + 1] == "trap") {
        good = st == Status::Trap;
      } else if (t.size() == arrow + 2 && t[arrow + 1] == "exception") {
        good = st == Status::Exception && store.has_pending_exception();
        store.take_pending_exception();
      } else {
        good = st == Status::Ok && t.size() == arrow + 1 + fn->sig->nresults;
        for (uint32_t i = 0; good && i < fn->sig->nresults; ++i) {
          Value want;
          good = parse_value(t[arrow + 1 + i], &want) && same_value(want, results[i]);
        }
      }
      if (!good) {
        if (reported++ < 25) {
          printf("line %d: %s\n   got status %d%s", number, line.c_str(), static_cast<int>(st), st == Status::Trap ? (std::string(" trap: ") + trap).c_str() : "");
          for (uint32_t i = 0; st == Status::Ok && i < fn->sig->nresults; ++i) printf(" %llx:%llx", (unsigned long long)results[i].lo, (unsigned long long)results[i].hi);
          printf("\n");
        }
        ++g_failed;
      }
    }
  }
  fclose(f);
  printf("vectors: %d modules, %d calls, %d valid and %d invalid modules checked\n", modules, calls, valids, invalids);
}


// --- the API: linking, host functions, traps, limits ------------------------------------------------------------------------------------------

static const char* kModuleA =
    "0061736d01000000010d0360017f017f6000006000017f02110203656e760166000003656e760168000103070600010101010007260601670002047370696e000303726563000404747261700005057477696365000602"
    "66660007"
    "0a2c0609002000100041016a0b070003400c000b0b040010040b0300000b0600100110010b08002000100010000b";
static const char* kModuleB =
    "0061736d0100000001040160000002230303656e76036d656d0201020403656e7603746162017001020503656e760167037f0103020100070501016600000a040102000b";
static const char* kModuleC = "0061736d01000000010401600000030201000404017000010503010001070501016600000a040102000b0b0a010041ffff030b020102";
static const char* kModuleD = "0061736d01000000010401600000030201000404017000010503010001070501016600000907010041010b01000a040102000b";
static const char* kModuleE = "0061736d01000000010401600000030201000801000a05010300000b";
static const char* kModuleF = "0061736d01000000010b0260017f017f60027f7f0003030200010504010101040716030467726f7700000573746f72650001036d656d02000a12020600200040000b0900200020013602000b";

struct TestHost final : IHost {
  std::function<bool(Store&, FuncInst&, const Slot*, Slot*)> on_call;
  std::function<bool(uint64_t)> on_tick;
  uint64_t ticks = 0;
  bool call(Store& s, FuncInst& f, const Slot* args, Slot* results) override { return on_call ? on_call(s, f, args, results) : true; }
  bool tick(uint64_t n) override {
    ticks += n;
    return on_tick ? on_tick(n) : true;
  }
};

static FuncInst* export_func(Instance* inst, const char* name) {
  ExternVal ev;
  if (!Store::find_export(*inst, StringView(name, strlen(name)), &ev) || ev.kind != ExternKind::Func) return nullptr;
  return static_cast<FuncInst*>(ev.ptr);
}

static Status call_i32(Store& store, FuncInst* f, std::initializer_list<uint32_t> args, uint32_t* result, const char** trap) {
  Slot in[8], out[4];
  uint32_t n = 0;
  for (uint32_t a : args) in[n++].lo = a;
  const Status st = store.invoke(*f, in, out, trap);
  if (result) *result = static_cast<uint32_t>(out[0].lo);
  return st;
}

static void test_host_functions() {
  Store store(A);
  TestHost host;
  store.set_host(&host);
  SharedPointer<Module> m;
  CHECK(parse_module(from_hex(kModuleA), &m));
  FuncInst* f = store.new_host_function(store.intern_sig(*m->types[0]), 1, nullptr);
  FuncInst* h = store.new_host_function(store.intern_sig(*m->types[1]), 2, nullptr);
  ExternVal imports[2];
  imports[0].kind = imports[1].kind = ExternKind::Func;
  imports[0].ptr = f;
  imports[1].ptr = h;
  Instance* inst = nullptr;
  String err(A);
  bool link = false;
  Status st = Status::Ok;
  CHECK(store.instantiate(m, ArrayView<const ExternVal>(imports, 2), &inst, &err, &link, &st));
  if (!inst) return;
  int h_calls = 0;
  bool fail_next = false;
  FuncInst *g = export_func(inst, "g"), *ff = export_func(inst, "ff"), *twice = export_func(inst, "twice"), *spin = export_func(inst, "spin"), *rec = export_func(inst, "rec"),
           *trap_fn = export_func(inst, "trap");
  host.on_call = [&](Store& s, FuncInst& fn, const Slot* args, Slot* results) {
    if (fail_next) return false;
    if (fn.host_id == 1) {
      const uint32_t x = static_cast<uint32_t>(args[0].lo);
      results[0].lo = x * 2;
      return true;
    }
    ++h_calls;
    return true;
  };
  const char* trap = nullptr;
  uint32_t r = 0;
  CHECK(call_i32(store, g, {20}, &r, &trap) == Status::Ok && r == 41);
  CHECK(call_i32(store, ff, {3}, &r, &trap) == Status::Ok && r == 12);
  CHECK(call_i32(store, twice, {}, &r, &trap) == Status::Ok && h_calls == 2);
  // a host function that fails unwinds the whole call, and the machine works afterwards
  fail_next = true;
  CHECK(call_i32(store, g, {1}, &r, &trap) == Status::HostError);
  fail_next = false;
  CHECK(call_i32(store, g, {2}, &r, &trap) == Status::Ok && r == 5);
  // a host function that calls back in
  host.on_call = [&](Store& s, FuncInst& fn, const Slot* args, Slot* results) {
    if (fn.host_id != 1) return true;
    const uint32_t x = static_cast<uint32_t>(args[0].lo);
    if (x == 0) {
      results[0].lo = 0;
      return true;
    }
    Slot in, out;
    in.lo = x - 1;
    const char* t = nullptr;
    if (s.invoke(*g, &in, &out, &t) != Status::Ok) return false;
    results[0].lo = out.lo;
    return true;
  };
  CHECK(call_i32(store, g, {3}, &r, &trap) == Status::Ok && r == 4);   // g(3) = f(3) + 1, f(3) = g(2), g(2) = f(2) + 1 ... g(0) = 1
  // traps
  CHECK(call_i32(store, trap_fn, {}, &r, &trap) == Status::Trap && trap && std::string(trap) == "unreachable");
  CHECK(call_i32(store, rec, {}, &r, &trap) == Status::Trap && trap && std::string(trap) == "Maximum call stack size exceeded");
  host.on_call = nullptr;
  CHECK(call_i32(store, g, {7}, &r, &trap) == Status::Ok && r == 1);   // (an unset host function gives zero)
  // a run is stopped by tick()
  host.ticks = 0;
  int budget = 3;
  host.on_tick = [&](uint64_t) { return --budget > 0; };
  CHECK(call_i32(store, spin, {}, &r, &trap) == Status::HostError && host.ticks >= 3 * 4096);
  host.on_tick = nullptr;
  CHECK(call_i32(store, g, {7}, &r, &trap) == Status::Ok);
}

static void test_linking() {
  Store store(A);
  SharedPointer<Module> m;
  CHECK(parse_module(from_hex(kModuleB), &m));
  MemType mt;
  mt.limits.min = 3;
  mt.limits.has_max = true;
  mt.limits.max = 4;
  TableType tt;
  tt.elem = ValType::FuncRef;
  tt.limits.min = 2;
  tt.limits.has_max = true;
  tt.limits.max = 5;
  GlobalType gt;
  gt.type = ValType::I32;
  gt.mut = true;
  Memory* mem = store.new_memory(mt);
  Table* tab = store.new_table(tt, 0);
  Global* glob = store.new_global(gt, Slot());
  ExternVal good[3];
  good[0].kind = ExternKind::Memory;
  good[0].ptr = mem;
  good[1].kind = ExternKind::Table;
  good[1].ptr = tab;
  good[2].kind = ExternKind::Global;
  good[2].ptr = glob;
  Instance* inst = nullptr;
  String err(A);
  bool link = false;
  Status st = Status::Ok;
  CHECK(store.instantiate(m, ArrayView<const ExternVal>(good, 3), &inst, &err, &link, &st));
  // wrong number, wrong kinds
  CHECK(!store.instantiate(m, ArrayView<const ExternVal>(good, 2), &inst, &err, &link, &st) && link);
  ExternVal bad[3] = {good[1], good[0], good[2]};
  CHECK(!store.instantiate(m, ArrayView<const ExternVal>(bad, 3), &inst, &err, &link, &st) && link);
  // a memory that is too small, one without the maximum, a table with a larger maximum, a global of the other mutability or type
  MemType small = mt;
  small.limits.min = 1;
  ExternVal b2[3] = {good[0], good[1], good[2]};
  b2[0].ptr = store.new_memory(small);
  CHECK(!store.instantiate(m, ArrayView<const ExternVal>(b2, 3), &inst, &err, &link, &st) && link);
  MemType nomax = mt;
  nomax.limits.has_max = false;
  b2[0].ptr = store.new_memory(nomax);
  CHECK(!store.instantiate(m, ArrayView<const ExternVal>(b2, 3), &inst, &err, &link, &st) && link);
  b2[0] = good[0];
  TableType wide = tt;
  wide.limits.max = 9;
  b2[1].ptr = store.new_table(wide, 0);
  CHECK(!store.instantiate(m, ArrayView<const ExternVal>(b2, 3), &inst, &err, &link, &st) && link);
  b2[1] = good[1];
  GlobalType im = gt;
  im.mut = false;
  b2[2].ptr = store.new_global(im, Slot());
  CHECK(!store.instantiate(m, ArrayView<const ExternVal>(b2, 3), &inst, &err, &link, &st) && link);
  GlobalType other = gt;
  other.type = ValType::I64;
  b2[2].ptr = store.new_global(other, Slot());
  CHECK(!store.instantiate(m, ArrayView<const ExternVal>(b2, 3), &inst, &err, &link, &st) && link);
  // a function of the wrong type
  SharedPointer<Module> a;
  CHECK(parse_module(from_hex(kModuleA), &a));
  FuncSig other_sig(A);
  CHECK(other_sig.init(2, 0));
  ExternVal fs[2];
  fs[0].kind = fs[1].kind = ExternKind::Func;
  fs[0].ptr = store.new_host_function(store.intern_sig(other_sig), 1, nullptr);
  fs[1].ptr = store.new_host_function(store.intern_sig(*a->types[1]), 2, nullptr);
  CHECK(!store.instantiate(a, ArrayView<const ExternVal>(fs, 2), &inst, &err, &link, &st) && link);
}

static void test_segments_and_start() {
  Store store(A);
  Instance* inst = nullptr;
  String err(A);
  bool link = false;
  Status st = Status::Ok;
  SharedPointer<Module> m;
  CHECK(parse_module(from_hex(kModuleC), &m));
  CHECK(!store.instantiate(m, ArrayView<const ExternVal>(), &inst, &err, &link, &st) && !link && st == Status::Trap);
  CHECK(parse_module(from_hex(kModuleD), &m));
  CHECK(!store.instantiate(m, ArrayView<const ExternVal>(), &inst, &err, &link, &st) && !link && st == Status::Trap);
  CHECK(parse_module(from_hex(kModuleE), &m));
  CHECK(!store.instantiate(m, ArrayView<const ExternVal>(), &inst, &err, &link, &st) && st == Status::Trap && err.view().equals("unreachable"));
}

struct HostMemory {
  std::vector<uint8_t> bytes;
  int resizes = 0;
  bool fail = false;
};

static bool hosted_resize(Memory& m, uint64_t new_size) {
  HostMemory* h = static_cast<HostMemory*>(m.host);
  if (h->fail) return false;
  ++h->resizes;
  h->bytes.resize(new_size);
  m.data = h->bytes.data();
  m.size = new_size;
  return true;
}

static void test_memory_hook() {
  Store store(A);
  HostMemory host;
  store.set_memory_hook(hosted_resize, &host);
  SharedPointer<Module> m;
  CHECK(parse_module(from_hex(kModuleF), &m));
  Instance* inst = nullptr;
  String err(A);
  bool link = false;
  Status st = Status::Ok;
  CHECK(store.instantiate(m, ArrayView<const ExternVal>(), &inst, &err, &link, &st));
  if (!inst) return;
  CHECK(host.resizes == 1 && host.bytes.size() == 65536);
  const char* trap = nullptr;
  uint32_t r = 0;
  FuncInst *grow = export_func(inst, "grow"), *store_fn = export_func(inst, "store");
  CHECK(call_i32(store, store_fn, {65532, 0x01020304}, &r, &trap) == Status::Ok && host.bytes[65532] == 4 && host.bytes[65535] == 1);
  CHECK(call_i32(store, grow, {1}, &r, &trap) == Status::Ok && r == 1 && host.resizes == 2 && host.bytes.size() == 131072 && host.bytes[65532] == 4);
  host.fail = true;
  CHECK(call_i32(store, grow, {1}, &r, &trap) == Status::Ok && r == 0xFFFFFFFFu && host.bytes.size() == 131072);
  host.fail = false;
  CHECK(call_i32(store, grow, {3}, &r, &trap) == Status::Ok && r == 0xFFFFFFFFu);   // (the maximum is 4 pages)
  CHECK(call_i32(store, grow, {2}, &r, &trap) == Status::Ok && r == 2);
  CHECK(call_i32(store, store_fn, {4 * 65536 - 3, 7}, &r, &trap) == Status::Trap);
  // the page budget
  Store small(A);
  small.set_page_budget(3);
  MemType big;
  big.limits.min = 4;
  CHECK(small.new_memory(big) == nullptr);
  big.limits.min = 2;
  Memory* two = small.new_memory(big);
  CHECK(two && two->grow(1) == 2 && two->grow(1) == -1);
}

// Damaged modules: every prefix, and every single bit flipped, of the modules of the vectors. The parser must say yes or no without touching what is not there
// (the sanitizers watch), and a module that is still valid must be instantiable and its functions must not crash when called with zeros.
static void test_damage() {
  FILE* f = fopen("tests/data/wasm_vectors.txt", "rb");
  if (!f) return;
  std::vector<std::vector<uint8_t>> modules;
  static char buffer[1 << 25];
  while (fgets(buffer, sizeof(buffer), f)) {
    if (strncmp(buffer, "module ", 7) != 0) continue;
    std::string hex(buffer + 7);
    while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r')) hex.pop_back();
    std::vector<uint8_t> bytes = from_hex(hex);
    if (bytes.size() <= 160) modules.push_back(bytes);
  }
  fclose(f);
  uint64_t parsed = 0, valid = 0, ran = 0;
  Store store(A);
  store.set_page_budget(16);
  TestHost host;   // (a damaged module may loop for ever: the first tick ends the run)
  host.on_tick = [](uint64_t) { return false; };
  store.set_host(&host);
  for (size_t i = 0; i < modules.size() && i < 400; ++i) {
    const std::vector<uint8_t>& base = modules[i];
    for (size_t cut = 0; cut < base.size(); ++cut) {
      SharedPointer<Module> m;
      std::vector<uint8_t> part(base.begin(), base.begin() + cut);
      parse_module(part, &m);
      ++parsed;
    }
    for (size_t byte = 8; byte < base.size(); ++byte) {
      for (int bit = 0; bit < 8; ++bit) {
        std::vector<uint8_t> copy = base;
        copy[byte] ^= static_cast<uint8_t>(1 << bit);
        SharedPointer<Module> m;
        ++parsed;
        if (!parse_module(copy, &m)) continue;
        ++valid;
        if (m->imports.count() != 0 || m->memories.count() > 1 || (m->memories.count() == 1 && m->memories[0].limits.min > 4)) continue;
        Instance* inst = nullptr;
        String err(A);
        bool link;
        Status st;
        if (!store.instantiate(m, ArrayView<const ExternVal>(), &inst, &err, &link, &st) || !inst) continue;
        for (uint64_t e = 0; e < m->exports.count(); ++e) {
          ExternVal ev = Store::export_value(*inst, *m->exports[e]);
          if (ev.kind != ExternKind::Func) continue;
          FuncInst* fn = static_cast<FuncInst*>(ev.ptr);
          std::vector<Slot> args(fn->sig->nparams + 1), out(fn->sig->nresults + 1);
          const char* trap = nullptr;
          store.invoke(*fn, args.data(), out.data(), &trap);
          ++ran;
        }
      }
    }
  }
  printf("damage: %llu parsed, %llu still valid, %llu functions run\n", (unsigned long long)parsed, (unsigned long long)valid, (unsigned long long)ran);
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_vectors(argc > 1 ? argv[1] : "tests/data/wasm_vectors.txt");
  if (argc > 1) return g_failed ? 1 : 0;   // (a vector file of its own: only that)
  test_host_functions();
  test_linking();
  test_segments_and_start();
  test_memory_hook();
  test_damage();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  CHECK(A.live == 0);
  printf("WasmTest: all passed\n");
  return g_failed ? 1 : 0;
}
