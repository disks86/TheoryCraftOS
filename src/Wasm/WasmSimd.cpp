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

// The fixed-width SIMD operations of WebAssembly (the 0xFD prefix) on the 128 bit values of the interpreter's stack. A v128 is two 64 bit halves of a Slot; lane i of
// a type T is the sizeof(T) bytes at offset i * sizeof(T), little-endian. Written lane by lane: the compiler may turn that into vector code, but it has to be right
// first.
#include <type_traits>
#include "Wasm/WasmInternal.hpp"
#include "Wasm/WasmNumeric.hpp"

namespace wasm {

namespace {

template <uint32_t N>
struct UnsignedOf;
template <>
struct UnsignedOf<1> {
  using type = uint8_t;
};
template <>
struct UnsignedOf<2> {
  using type = uint16_t;
};
template <>
struct UnsignedOf<4> {
  using type = uint32_t;
};
template <>
struct UnsignedOf<8> {
  using type = uint64_t;
};

template <class T>
inline T get(const Slot& s, uint32_t i) {
  using U = typename UnsignedOf<sizeof(T)>::type;
  const uint32_t bit = i * sizeof(T) * 8;
  const uint64_t w = bit < 64 ? s.lo : s.hi;
  return __builtin_bit_cast(T, static_cast<U>(w >> (bit & 63)));
}

template <class T>
inline void put(Slot& s, uint32_t i, T v) {
  using U = typename UnsignedOf<sizeof(T)>::type;
  const uint32_t bit = i * sizeof(T) * 8;
  uint64_t& w = bit < 64 ? s.lo : s.hi;
  const uint32_t shift = bit & 63;
  const uint64_t mask = sizeof(T) == 8 ? ~uint64_t{0} : ((uint64_t{1} << (8 * sizeof(T))) - 1);
  w = (w & ~(mask << shift)) | (static_cast<uint64_t>(__builtin_bit_cast(U, v)) << shift);
}

template <class T, class F>
inline void map1(Slot& r, const Slot& a, F f) {
  Slot out;
  for (uint32_t i = 0; i < 16 / sizeof(T); ++i) put<T>(out, i, f(get<T>(a, i)));
  r = out;
}
template <class T, class F>
inline void map2(Slot& r, const Slot& a, const Slot& b, F f) {
  Slot out;
  for (uint32_t i = 0; i < 16 / sizeof(T); ++i) put<T>(out, i, f(get<T>(a, i), get<T>(b, i)));
  r = out;
}
// A comparison: every lane becomes all ones or all zeros.
template <class T, class F>
inline void cmp2(Slot& r, const Slot& a, const Slot& b, F f) {
  using U = typename UnsignedOf<sizeof(T)>::type;
  Slot out;
  for (uint32_t i = 0; i < 16 / sizeof(T); ++i) put<U>(out, i, f(get<T>(a, i), get<T>(b, i)) ? static_cast<U>(~U{0}) : U{0});
  r = out;
}

template <class T>
inline T sat(int64_t v) {
  constexpr int64_t lo = std::is_signed_v<T> ? -(int64_t{1} << (8 * sizeof(T) - 1)) : 0;
  constexpr int64_t hi = std::is_signed_v<T> ? (int64_t{1} << (8 * sizeof(T) - 1)) - 1 : (int64_t{1} << (8 * sizeof(T))) - 1;
  return static_cast<T>(v < lo ? lo : (v > hi ? hi : v));
}

// Integer lane operations by their number relative to the lane width (see simd_exec). T is the signed lane type.
template <class T>
bool int_lane_op(uint32_t rel, Slot& r, const Slot& a, const Slot& b, uint32_t count) {
  using U = std::make_unsigned_t<T>;
  constexpr uint32_t bits = 8 * sizeof(T);
  switch (rel) {
    case 96: map1<T>(r, a, [](T x) { return static_cast<T>(x < 0 ? U(0) - U(x) : U(x)); }); return true;
    case 97: map1<T>(r, a, [](T x) { return static_cast<T>(U(0) - U(x)); }); return true;
    case 107: map1<U>(r, a, [&](U x) { return static_cast<U>(x << (count & (bits - 1))); }); return true;
    case 108: map1<T>(r, a, [&](T x) { return static_cast<T>(x >> (count & (bits - 1))); }); return true;
    case 109: map1<U>(r, a, [&](U x) { return static_cast<U>(x >> (count & (bits - 1))); }); return true;
    case 110: map2<U>(r, a, b, [](U x, U y) { return static_cast<U>(x + y); }); return true;
    case 113: map2<U>(r, a, b, [](U x, U y) { return static_cast<U>(x - y); }); return true;
    case 117: map2<U>(r, a, b, [](U x, U y) { return static_cast<U>(uint64_t{x} * y); }); return true;
    case 118: map2<T>(r, a, b, [](T x, T y) { return x < y ? x : y; }); return true;
    case 119: map2<U>(r, a, b, [](U x, U y) { return x < y ? x : y; }); return true;
    case 120: map2<T>(r, a, b, [](T x, T y) { return x > y ? x : y; }); return true;
    case 121: map2<U>(r, a, b, [](U x, U y) { return x > y ? x : y; }); return true;
  }
  if constexpr (sizeof(T) <= 2) {
    switch (rel) {
      case 111: map2<T>(r, a, b, [](T x, T y) { return sat<T>(int64_t{x} + y); }); return true;
      case 112: map2<U>(r, a, b, [](U x, U y) { return sat<U>(int64_t{x} + y); }); return true;
      case 114: map2<T>(r, a, b, [](T x, T y) { return sat<T>(int64_t{x} - y); }); return true;
      case 115: map2<U>(r, a, b, [](U x, U y) { return sat<U>(int64_t{x} - y); }); return true;
      case 123: map2<U>(r, a, b, [](U x, U y) { return static_cast<U>((uint32_t{x} + y + 1) >> 1); }); return true;
    }
  }
  return false;
}

template <class T>
uint32_t all_true(const Slot& a) {
  for (uint32_t i = 0; i < 16 / sizeof(T); ++i)
    if (get<T>(a, i) == 0) return 0;
  return 1;
}
template <class T>
uint32_t bitmask(const Slot& a) {
  uint32_t m = 0;
  for (uint32_t i = 0; i < 16 / sizeof(T); ++i)
    if (get<T>(a, i) < 0) m |= 1u << i;
  return m;
}

// Widens the low or high half of `a` (lanes of S) to lanes of D, and optionally multiplies with the same half of `b`.
template <class S, class D>
void widen(Slot& r, const Slot& a, const Slot& b, bool high, bool multiply) {
  constexpr uint32_t n = 16 / sizeof(D);
  const uint32_t base = high ? n : 0;
  Slot out;
  for (uint32_t i = 0; i < n; ++i) {
    D x = static_cast<D>(get<S>(a, base + i));
    if (multiply) x = static_cast<D>(x * static_cast<D>(get<S>(b, base + i)));
    put<D>(out, i, x);
  }
  r = out;
}

// Narrows lanes of S (signed) from a and then b to lanes of D with saturation.
template <class S, class D>
void narrow(Slot& r, const Slot& a, const Slot& b) {
  constexpr uint32_t n = 16 / sizeof(S);
  Slot out;
  for (uint32_t i = 0; i < n; ++i) {
    put<D>(out, i, sat<D>(get<S>(a, i)));
    put<D>(out, n + i, sat<D>(get<S>(b, i)));
  }
  r = out;
}

template <class S, class D>
void extadd(Slot& r, const Slot& a) {
  constexpr uint32_t n = 16 / sizeof(D);
  Slot out;
  for (uint32_t i = 0; i < n; ++i) put<D>(out, i, static_cast<D>(static_cast<D>(get<S>(a, 2 * i)) + static_cast<D>(get<S>(a, 2 * i + 1))));
  r = out;
}

inline Slot v128(uint64_t lo, uint64_t hi) {
  Slot s;
  s.lo = lo;
  s.hi = hi;
  return s;
}

}  // namespace

const char* simd_exec(uint32_t sub, const Instr* in, Slot*& sp, Instance& instance, uint32_t* extra) {
  *extra = 0;
  // --- memory
  if (sub <= 11 || (sub >= 84 && sub <= 93)) {
    Memory* m = instance.memories[in->aux];
    const bool is_store = sub == 11 || (sub >= 88 && sub <= 91);
    const bool lane_op = sub >= 84 && sub <= 91;
    const uint32_t addr_at = (is_store || lane_op) ? 2 : 1;
    const uint64_t ea = static_cast<uint64_t>(static_cast<uint32_t>(sp[-static_cast<int>(addr_at)].lo)) + in->a;
    uint32_t width;
    if (sub == 0 || sub == 11) width = 16;
    else if (sub >= 1 && sub <= 6) width = 8;
    else if (sub >= 7 && sub <= 10) width = 1u << (sub - 7);
    else if (sub >= 84 && sub <= 91) width = 1u << ((sub >= 88 ? sub - 88 : sub - 84));
    else width = sub == 92 ? 4 : 8;
    if (ea + width > m->size) return kTrapMemory;
    const uint8_t* p = m->data + ea;
    Slot r;
    switch (sub) {
      case 0: r = v128(num::load_le(p, 8), num::load_le(p + 8, 8)); break;
      case 1: for (uint32_t i = 0; i < 8; ++i) put<int16_t>(r, i, static_cast<int8_t>(p[i])); break;
      case 2: for (uint32_t i = 0; i < 8; ++i) put<uint16_t>(r, i, p[i]); break;
      case 3: for (uint32_t i = 0; i < 4; ++i) put<int32_t>(r, i, static_cast<int16_t>(num::load_le(p + 2 * i, 2))); break;
      case 4: for (uint32_t i = 0; i < 4; ++i) put<uint32_t>(r, i, static_cast<uint32_t>(num::load_le(p + 2 * i, 2))); break;
      case 5: for (uint32_t i = 0; i < 2; ++i) put<int64_t>(r, i, static_cast<int32_t>(num::load_le(p + 4 * i, 4))); break;
      case 6: for (uint32_t i = 0; i < 2; ++i) put<uint64_t>(r, i, num::load_le(p + 4 * i, 4)); break;
      case 7: for (uint32_t i = 0; i < 16; ++i) put<uint8_t>(r, i, p[0]); break;
      case 8: for (uint32_t i = 0; i < 8; ++i) put<uint16_t>(r, i, static_cast<uint16_t>(num::load_le(p, 2))); break;
      case 9: for (uint32_t i = 0; i < 4; ++i) put<uint32_t>(r, i, static_cast<uint32_t>(num::load_le(p, 4))); break;
      case 10: r = v128(num::load_le(p, 8), num::load_le(p, 8)); break;
      case 11: {
        uint8_t* q = m->data + ea;
        num::store_le(q, 8, sp[-1].lo);
        num::store_le(q + 8, 8, sp[-1].hi);
        sp -= 2;
        return nullptr;
      }
      case 92: r = v128(num::load_le(p, 4), 0); break;
      case 93: r = v128(num::load_le(p, 8), 0); break;
      default: {   // 84..87 load lane, 88..91 store lane
        const uint32_t lane = static_cast<uint32_t>(in->b);
        if (sub >= 88) {
          const Slot& v = sp[-1];
          uint8_t* q = m->data + ea;
          switch (sub) {
            case 88: num::store_le(q, 1, get<uint8_t>(v, lane)); break;
            case 89: num::store_le(q, 2, get<uint16_t>(v, lane)); break;
            case 90: num::store_le(q, 4, get<uint32_t>(v, lane)); break;
            default: num::store_le(q, 8, get<uint64_t>(v, lane)); break;
          }
          sp -= 2;
          return nullptr;
        }
        r = sp[-1];
        switch (sub) {
          case 84: put<uint8_t>(r, lane, p[0]); break;
          case 85: put<uint16_t>(r, lane, static_cast<uint16_t>(num::load_le(p, 2))); break;
          case 86: put<uint32_t>(r, lane, static_cast<uint32_t>(num::load_le(p, 4))); break;
          default: put<uint64_t>(r, lane, num::load_le(p, 8)); break;
        }
        break;
      }
    }
    if (lane_op) --sp;   // (the address is replaced by nothing: the vector it was combined with is the result)
    sp[-1] = r;
    return nullptr;
  }

  Slot& top = sp[-1];
  switch (sub) {
    case 12:
      *sp = v128(in->b, in[1].b);
      ++sp;
      *extra = 1;
      return nullptr;
    case 13: {   // shuffle
      const Slot& a = sp[-2];
      const Slot& b = sp[-1];
      const uint64_t lo = in->b, hi = in[1].b;
      Slot r;
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t idx = static_cast<uint32_t>(((i < 8 ? lo : hi) >> (8 * (i & 7))) & 0xFF);
        put<uint8_t>(r, i, idx < 16 ? get<uint8_t>(a, idx) : get<uint8_t>(b, idx - 16));
      }
      --sp;
      sp[-1] = r;
      *extra = 1;
      return nullptr;
    }
    case 14:
    case 256: {   // swizzle (the relaxed one gives zero for an index that is out of range, as the other does)
      const Slot& a = sp[-2];
      const Slot& s = sp[-1];
      Slot r;
      for (uint32_t i = 0; i < 16; ++i) {
        const uint8_t idx = get<uint8_t>(s, i);
        put<uint8_t>(r, i, idx < 16 ? get<uint8_t>(a, idx) : 0);
      }
      --sp;
      sp[-1] = r;
      return nullptr;
    }
    case 15: { Slot r; const uint8_t x = static_cast<uint8_t>(top.lo); for (uint32_t i = 0; i < 16; ++i) put<uint8_t>(r, i, x); top = r; return nullptr; }
    case 16: { Slot r; const uint16_t x = static_cast<uint16_t>(top.lo); for (uint32_t i = 0; i < 8; ++i) put<uint16_t>(r, i, x); top = r; return nullptr; }
    case 17:
    case 19: { const uint32_t x = static_cast<uint32_t>(top.lo); top = v128(uint64_t{x} | (uint64_t{x} << 32), uint64_t{x} | (uint64_t{x} << 32)); return nullptr; }
    case 18:
    case 20: top = v128(top.lo, top.lo); return nullptr;
    case 21: top = v128(static_cast<uint32_t>(static_cast<int32_t>(get<int8_t>(top, in->a))), 0); return nullptr;
    case 22: top = v128(get<uint8_t>(top, in->a), 0); return nullptr;
    case 24: top = v128(static_cast<uint32_t>(static_cast<int32_t>(get<int16_t>(top, in->a))), 0); return nullptr;
    case 25: top = v128(get<uint16_t>(top, in->a), 0); return nullptr;
    case 27:
    case 31: top = v128(get<uint32_t>(top, in->a), 0); return nullptr;
    case 29:
    case 33: top = v128(get<uint64_t>(top, in->a), 0); return nullptr;
    case 23: { Slot v = sp[-2]; put<uint8_t>(v, in->a, static_cast<uint8_t>(sp[-1].lo)); --sp; sp[-1] = v; return nullptr; }
    case 26: { Slot v = sp[-2]; put<uint16_t>(v, in->a, static_cast<uint16_t>(sp[-1].lo)); --sp; sp[-1] = v; return nullptr; }
    case 28:
    case 32: { Slot v = sp[-2]; put<uint32_t>(v, in->a, static_cast<uint32_t>(sp[-1].lo)); --sp; sp[-1] = v; return nullptr; }
    case 30:
    case 34: { Slot v = sp[-2]; put<uint64_t>(v, in->a, sp[-1].lo); --sp; sp[-1] = v; return nullptr; }
    case 77: top = v128(~top.lo, ~top.hi); return nullptr;
    case 78: sp[-2] = v128(sp[-2].lo & sp[-1].lo, sp[-2].hi & sp[-1].hi); --sp; return nullptr;
    case 79: sp[-2] = v128(sp[-2].lo & ~sp[-1].lo, sp[-2].hi & ~sp[-1].hi); --sp; return nullptr;
    case 80: sp[-2] = v128(sp[-2].lo | sp[-1].lo, sp[-2].hi | sp[-1].hi); --sp; return nullptr;
    case 81: sp[-2] = v128(sp[-2].lo ^ sp[-1].lo, sp[-2].hi ^ sp[-1].hi); --sp; return nullptr;
    case 82: {   // bitselect: v1 v2 mask
      const Slot c = sp[-1];
      sp -= 2;
      sp[-1] = v128((sp[-1].lo & c.lo) | (sp[0].lo & ~c.lo), (sp[-1].hi & c.hi) | (sp[0].hi & ~c.hi));
      return nullptr;
    }
    case 83: top = v128((top.lo | top.hi) != 0, 0); return nullptr;
    case 261: case 262: case 263: case 264: {   // relaxed fused multiply-add (computed in two steps) and its negation
      const Slot c = sp[-1], b = sp[-2], a = sp[-3];
      sp -= 2;
      Slot r;
      if (sub <= 262) {
        for (uint32_t i = 0; i < 4; ++i) {
          const float p = get<float>(a, i) * get<float>(b, i);
          put<float>(r, i, (sub == 261 ? p : -p) + get<float>(c, i));
        }
      } else {
        for (uint32_t i = 0; i < 2; ++i) {
          const double p = get<double>(a, i) * get<double>(b, i);
          put<double>(r, i, (sub == 263 ? p : -p) + get<double>(c, i));
        }
      }
      sp[-1] = r;
      return nullptr;
    }
    case 265: case 266: case 267: case 268: {   // relaxed lane select: where the mask has its bits
      const Slot c = sp[-1];
      sp -= 2;
      sp[-1] = v128((sp[-1].lo & c.lo) | (sp[0].lo & ~c.lo), (sp[-1].hi & c.hi) | (sp[0].hi & ~c.hi));
      return nullptr;
    }
    case 275: {   // i32x4.relaxed_dot_i8x16_i7x16_add_s
      const Slot c = sp[-1], b = sp[-2], a = sp[-3];
      sp -= 2;
      Slot r;
      for (uint32_t i = 0; i < 4; ++i) {
        int32_t sum = 0;
        for (uint32_t k = 0; k < 4; ++k) sum += int32_t{get<int8_t>(a, 4 * i + k)} * get<int8_t>(b, 4 * i + k);
        put<uint32_t>(r, i, static_cast<uint32_t>(sum) + get<uint32_t>(c, i));
      }
      sp[-1] = r;
      return nullptr;
    }
  }

  // The remaining operations take one or two vectors (and a shift count) and give a vector or a number.
  Slot a, b;
  uint32_t count = 0;
  const bool shift = (sub >= 107 && sub <= 109) || (sub >= 139 && sub <= 141) || (sub >= 171 && sub <= 173) || (sub >= 203 && sub <= 205);
  if (shift) {
    count = static_cast<uint32_t>(sp[-1].lo);
    --sp;
    a = sp[-1];
  } else {
    a = sp[-1];
  }
  bool binary = false;
  switch (sub) {
    case 35 ... 76: case 78 ... 81: case 101: case 102: case 110 ... 115: case 118 ... 121: case 123: case 130: case 133: case 134: case 142 ... 147: case 149 ... 153:
    case 155 ... 159: case 269 ... 274: case 174: case 177: case 181 ... 186: case 188 ... 191: case 206: case 209: case 213 ... 223: case 228 ... 235: case 240 ... 247:
      binary = true;
      break;
  }
  if (binary) {
    b = a;
    a = sp[-2];
    --sp;
  }
  Slot& out = sp[-1];

  // comparisons
  if (sub >= 35 && sub <= 76) {
    if (sub <= 44) {
      switch (sub) {
        case 35: cmp2<int8_t>(out, a, b, [](int8_t x, int8_t y) { return x == y; }); break;
        case 36: cmp2<int8_t>(out, a, b, [](int8_t x, int8_t y) { return x != y; }); break;
        case 37: cmp2<int8_t>(out, a, b, [](int8_t x, int8_t y) { return x < y; }); break;
        case 38: cmp2<uint8_t>(out, a, b, [](uint8_t x, uint8_t y) { return x < y; }); break;
        case 39: cmp2<int8_t>(out, a, b, [](int8_t x, int8_t y) { return x > y; }); break;
        case 40: cmp2<uint8_t>(out, a, b, [](uint8_t x, uint8_t y) { return x > y; }); break;
        case 41: cmp2<int8_t>(out, a, b, [](int8_t x, int8_t y) { return x <= y; }); break;
        case 42: cmp2<uint8_t>(out, a, b, [](uint8_t x, uint8_t y) { return x <= y; }); break;
        case 43: cmp2<int8_t>(out, a, b, [](int8_t x, int8_t y) { return x >= y; }); break;
        default: cmp2<uint8_t>(out, a, b, [](uint8_t x, uint8_t y) { return x >= y; }); break;
      }
    } else if (sub <= 54) {
      switch (sub) {
        case 45: cmp2<int16_t>(out, a, b, [](int16_t x, int16_t y) { return x == y; }); break;
        case 46: cmp2<int16_t>(out, a, b, [](int16_t x, int16_t y) { return x != y; }); break;
        case 47: cmp2<int16_t>(out, a, b, [](int16_t x, int16_t y) { return x < y; }); break;
        case 48: cmp2<uint16_t>(out, a, b, [](uint16_t x, uint16_t y) { return x < y; }); break;
        case 49: cmp2<int16_t>(out, a, b, [](int16_t x, int16_t y) { return x > y; }); break;
        case 50: cmp2<uint16_t>(out, a, b, [](uint16_t x, uint16_t y) { return x > y; }); break;
        case 51: cmp2<int16_t>(out, a, b, [](int16_t x, int16_t y) { return x <= y; }); break;
        case 52: cmp2<uint16_t>(out, a, b, [](uint16_t x, uint16_t y) { return x <= y; }); break;
        case 53: cmp2<int16_t>(out, a, b, [](int16_t x, int16_t y) { return x >= y; }); break;
        default: cmp2<uint16_t>(out, a, b, [](uint16_t x, uint16_t y) { return x >= y; }); break;
      }
    } else if (sub <= 64) {
      switch (sub) {
        case 55: cmp2<int32_t>(out, a, b, [](int32_t x, int32_t y) { return x == y; }); break;
        case 56: cmp2<int32_t>(out, a, b, [](int32_t x, int32_t y) { return x != y; }); break;
        case 57: cmp2<int32_t>(out, a, b, [](int32_t x, int32_t y) { return x < y; }); break;
        case 58: cmp2<uint32_t>(out, a, b, [](uint32_t x, uint32_t y) { return x < y; }); break;
        case 59: cmp2<int32_t>(out, a, b, [](int32_t x, int32_t y) { return x > y; }); break;
        case 60: cmp2<uint32_t>(out, a, b, [](uint32_t x, uint32_t y) { return x > y; }); break;
        case 61: cmp2<int32_t>(out, a, b, [](int32_t x, int32_t y) { return x <= y; }); break;
        case 62: cmp2<uint32_t>(out, a, b, [](uint32_t x, uint32_t y) { return x <= y; }); break;
        case 63: cmp2<int32_t>(out, a, b, [](int32_t x, int32_t y) { return x >= y; }); break;
        default: cmp2<uint32_t>(out, a, b, [](uint32_t x, uint32_t y) { return x >= y; }); break;
      }
    } else if (sub <= 70) {
      switch (sub) {
        case 65: cmp2<float>(out, a, b, [](float x, float y) { return x == y; }); break;
        case 66: cmp2<float>(out, a, b, [](float x, float y) { return x != y; }); break;
        case 67: cmp2<float>(out, a, b, [](float x, float y) { return x < y; }); break;
        case 68: cmp2<float>(out, a, b, [](float x, float y) { return x > y; }); break;
        case 69: cmp2<float>(out, a, b, [](float x, float y) { return x <= y; }); break;
        default: cmp2<float>(out, a, b, [](float x, float y) { return x >= y; }); break;
      }
    } else {
      switch (sub) {
        case 71: cmp2<double>(out, a, b, [](double x, double y) { return x == y; }); break;
        case 72: cmp2<double>(out, a, b, [](double x, double y) { return x != y; }); break;
        case 73: cmp2<double>(out, a, b, [](double x, double y) { return x < y; }); break;
        case 74: cmp2<double>(out, a, b, [](double x, double y) { return x > y; }); break;
        case 75: cmp2<double>(out, a, b, [](double x, double y) { return x <= y; }); break;
        default: cmp2<double>(out, a, b, [](double x, double y) { return x >= y; }); break;
      }
    }
    return nullptr;
  }

  // conversions and the operations whose number does not follow the lane width
  switch (sub) {
    case 257: for (uint32_t i = 0; i < 4; ++i) put<int32_t>(out, i, num::sat_i32(get<float>(a, i))); return nullptr;   // relaxed truncations: the saturating ones
    case 258: for (uint32_t i = 0; i < 4; ++i) put<uint32_t>(out, i, num::sat_u32(get<float>(a, i))); return nullptr;
    case 259: { Slot r; for (uint32_t i = 0; i < 2; ++i) put<int32_t>(r, i, num::sat_i32(get<double>(a, i))); out = r; return nullptr; }
    case 260: { Slot r; for (uint32_t i = 0; i < 2; ++i) put<uint32_t>(r, i, num::sat_u32(get<double>(a, i))); out = r; return nullptr; }
    case 269: map2<float>(out, a, b, [](float x, float y) { return num::f_min(x, y); }); return nullptr;
    case 270: map2<float>(out, a, b, [](float x, float y) { return num::f_max(x, y); }); return nullptr;
    case 271: map2<double>(out, a, b, [](double x, double y) { return num::f_min(x, y); }); return nullptr;
    case 272: map2<double>(out, a, b, [](double x, double y) { return num::f_max(x, y); }); return nullptr;
    case 273: map2<int16_t>(out, a, b, [](int16_t x, int16_t y) { return sat<int16_t>((int64_t{x} * y + 0x4000) >> 15); }); return nullptr;
    case 274: {   // i16x8.relaxed_dot_i8x16_i7x16_s
      Slot r;
      for (uint32_t i = 0; i < 8; ++i) put<int16_t>(r, i, static_cast<int16_t>(int32_t{get<int8_t>(a, 2 * i)} * get<int8_t>(b, 2 * i) + int32_t{get<int8_t>(a, 2 * i + 1)} * get<int8_t>(b, 2 * i + 1)));
      out = r;
      return nullptr;
    }
    case 94: {   // f32x4.demote_f64x2_zero
      Slot r;
      put<float>(r, 0, static_cast<float>(get<double>(a, 0)));
      put<float>(r, 1, static_cast<float>(get<double>(a, 1)));
      out = r;
      return nullptr;
    }
    case 95: {   // f64x2.promote_low_f32x4
      Slot r;
      put<double>(r, 0, static_cast<double>(get<float>(a, 0)));
      put<double>(r, 1, static_cast<double>(get<float>(a, 1)));
      out = r;
      return nullptr;
    }
    case 98: map1<uint8_t>(out, a, [](uint8_t x) { return static_cast<uint8_t>(num::popcnt32(x)); }); return nullptr;
    case 99: out = v128(all_true<uint8_t>(a), 0); return nullptr;
    case 100: out = v128(bitmask<int8_t>(a), 0); return nullptr;
    case 101: narrow<int16_t, int8_t>(out, a, b); return nullptr;
    case 102: narrow<int16_t, uint8_t>(out, a, b); return nullptr;
    case 103: map1<float>(out, a, [](float x) { return num::f_ceil(x); }); return nullptr;
    case 104: map1<float>(out, a, [](float x) { return num::f_floor(x); }); return nullptr;
    case 105: map1<float>(out, a, [](float x) { return num::f_trunc(x); }); return nullptr;
    case 106: map1<float>(out, a, [](float x) { return num::f_nearest(x); }); return nullptr;
    case 116: map1<double>(out, a, [](double x) { return num::f_ceil(x); }); return nullptr;
    case 117: map1<double>(out, a, [](double x) { return num::f_floor(x); }); return nullptr;
    case 122: map1<double>(out, a, [](double x) { return num::f_trunc(x); }); return nullptr;
    case 148: map1<double>(out, a, [](double x) { return num::f_nearest(x); }); return nullptr;
    case 124: extadd<int8_t, int16_t>(out, a); return nullptr;
    case 125: extadd<uint8_t, uint16_t>(out, a); return nullptr;
    case 126: extadd<int16_t, int32_t>(out, a); return nullptr;
    case 127: extadd<uint16_t, uint32_t>(out, a); return nullptr;
    case 130: map2<int16_t>(out, a, b, [](int16_t x, int16_t y) { return sat<int16_t>((int64_t{x} * y + 0x4000) >> 15); }); return nullptr;
    case 131: out = v128(all_true<uint16_t>(a), 0); return nullptr;
    case 132: out = v128(bitmask<int16_t>(a), 0); return nullptr;
    case 133: narrow<int32_t, int16_t>(out, a, b); return nullptr;
    case 134: narrow<int32_t, uint16_t>(out, a, b); return nullptr;
    case 135: widen<int8_t, int16_t>(out, a, a, false, false); return nullptr;
    case 136: widen<int8_t, int16_t>(out, a, a, true, false); return nullptr;
    case 137: widen<uint8_t, uint16_t>(out, a, a, false, false); return nullptr;
    case 138: widen<uint8_t, uint16_t>(out, a, a, true, false); return nullptr;
    case 156: widen<int8_t, int16_t>(out, a, b, false, true); return nullptr;
    case 157: widen<int8_t, int16_t>(out, a, b, true, true); return nullptr;
    case 158: widen<uint8_t, uint16_t>(out, a, b, false, true); return nullptr;
    case 159: widen<uint8_t, uint16_t>(out, a, b, true, true); return nullptr;
    case 163: out = v128(all_true<uint32_t>(a), 0); return nullptr;
    case 164: out = v128(bitmask<int32_t>(a), 0); return nullptr;
    case 167: widen<int16_t, int32_t>(out, a, a, false, false); return nullptr;
    case 168: widen<int16_t, int32_t>(out, a, a, true, false); return nullptr;
    case 169: widen<uint16_t, uint32_t>(out, a, a, false, false); return nullptr;
    case 170: widen<uint16_t, uint32_t>(out, a, a, true, false); return nullptr;
    case 186: {   // i32x4.dot_i16x8_s
      Slot r;
      for (uint32_t i = 0; i < 4; ++i) {
        const int64_t s = int64_t{get<int16_t>(a, 2 * i)} * get<int16_t>(b, 2 * i) + int64_t{get<int16_t>(a, 2 * i + 1)} * get<int16_t>(b, 2 * i + 1);
        put<uint32_t>(r, i, static_cast<uint32_t>(s));
      }
      out = r;
      return nullptr;
    }
    case 188: widen<int16_t, int32_t>(out, a, b, false, true); return nullptr;
    case 189: widen<int16_t, int32_t>(out, a, b, true, true); return nullptr;
    case 190: widen<uint16_t, uint32_t>(out, a, b, false, true); return nullptr;
    case 191: widen<uint16_t, uint32_t>(out, a, b, true, true); return nullptr;
    case 195: out = v128(all_true<uint64_t>(a), 0); return nullptr;
    case 196: out = v128(bitmask<int64_t>(a), 0); return nullptr;
    case 199: widen<int32_t, int64_t>(out, a, a, false, false); return nullptr;
    case 200: widen<int32_t, int64_t>(out, a, a, true, false); return nullptr;
    case 201: widen<uint32_t, uint64_t>(out, a, a, false, false); return nullptr;
    case 202: widen<uint32_t, uint64_t>(out, a, a, true, false); return nullptr;
    case 214: cmp2<int64_t>(out, a, b, [](int64_t x, int64_t y) { return x == y; }); return nullptr;
    case 215: cmp2<int64_t>(out, a, b, [](int64_t x, int64_t y) { return x != y; }); return nullptr;
    case 216: cmp2<int64_t>(out, a, b, [](int64_t x, int64_t y) { return x < y; }); return nullptr;
    case 217: cmp2<int64_t>(out, a, b, [](int64_t x, int64_t y) { return x > y; }); return nullptr;
    case 218: cmp2<int64_t>(out, a, b, [](int64_t x, int64_t y) { return x <= y; }); return nullptr;
    case 219: cmp2<int64_t>(out, a, b, [](int64_t x, int64_t y) { return x >= y; }); return nullptr;
    case 220: widen<int32_t, int64_t>(out, a, b, false, true); return nullptr;
    case 221: widen<int32_t, int64_t>(out, a, b, true, true); return nullptr;
    case 222: widen<uint32_t, uint64_t>(out, a, b, false, true); return nullptr;
    case 223: widen<uint32_t, uint64_t>(out, a, b, true, true); return nullptr;
    case 224: map1<float>(out, a, [](float x) { return num::f_abs(x); }); return nullptr;
    case 225: map1<float>(out, a, [](float x) { return num::f_neg(x); }); return nullptr;
    case 227: map1<float>(out, a, [](float x) { return num::f_sqrt(x); }); return nullptr;
    case 228: map2<float>(out, a, b, [](float x, float y) { return x + y; }); return nullptr;
    case 229: map2<float>(out, a, b, [](float x, float y) { return x - y; }); return nullptr;
    case 230: map2<float>(out, a, b, [](float x, float y) { return x * y; }); return nullptr;
    case 231: map2<float>(out, a, b, [](float x, float y) { return x / y; }); return nullptr;
    case 232: map2<float>(out, a, b, [](float x, float y) { return num::f_min(x, y); }); return nullptr;
    case 233: map2<float>(out, a, b, [](float x, float y) { return num::f_max(x, y); }); return nullptr;
    case 234: map2<float>(out, a, b, [](float x, float y) { return y < x ? y : x; }); return nullptr;
    case 235: map2<float>(out, a, b, [](float x, float y) { return x < y ? y : x; }); return nullptr;
    case 236: map1<double>(out, a, [](double x) { return num::f_abs(x); }); return nullptr;
    case 237: map1<double>(out, a, [](double x) { return num::f_neg(x); }); return nullptr;
    case 239: map1<double>(out, a, [](double x) { return num::f_sqrt(x); }); return nullptr;
    case 240: map2<double>(out, a, b, [](double x, double y) { return x + y; }); return nullptr;
    case 241: map2<double>(out, a, b, [](double x, double y) { return x - y; }); return nullptr;
    case 242: map2<double>(out, a, b, [](double x, double y) { return x * y; }); return nullptr;
    case 243: map2<double>(out, a, b, [](double x, double y) { return x / y; }); return nullptr;
    case 244: map2<double>(out, a, b, [](double x, double y) { return num::f_min(x, y); }); return nullptr;
    case 245: map2<double>(out, a, b, [](double x, double y) { return num::f_max(x, y); }); return nullptr;
    case 246: map2<double>(out, a, b, [](double x, double y) { return y < x ? y : x; }); return nullptr;
    case 247: map2<double>(out, a, b, [](double x, double y) { return x < y ? y : x; }); return nullptr;
  }
  if (sub >= 248 && sub <= 255) {
    Slot r;
    switch (sub) {
      case 248: for (uint32_t i = 0; i < 4; ++i) put<int32_t>(r, i, num::sat_i32(get<float>(a, i))); break;
      case 249: for (uint32_t i = 0; i < 4; ++i) put<uint32_t>(r, i, num::sat_u32(get<float>(a, i))); break;
      case 250: for (uint32_t i = 0; i < 4; ++i) put<float>(r, i, static_cast<float>(get<int32_t>(a, i))); break;
      case 251: for (uint32_t i = 0; i < 4; ++i) put<float>(r, i, static_cast<float>(get<uint32_t>(a, i))); break;
      case 252: for (uint32_t i = 0; i < 2; ++i) put<int32_t>(r, i, num::sat_i32(get<double>(a, i))); break;
      case 253: for (uint32_t i = 0; i < 2; ++i) put<uint32_t>(r, i, num::sat_u32(get<double>(a, i))); break;
      case 254: for (uint32_t i = 0; i < 2; ++i) put<double>(r, i, static_cast<double>(get<int32_t>(a, i))); break;
      default: for (uint32_t i = 0; i < 2; ++i) put<double>(r, i, static_cast<double>(get<uint32_t>(a, i))); break;
    }
    out = r;
    return nullptr;
  }

  // integer lanes: the width follows from the number (96..127 bytes, 128..159 halfwords, 160..191 words, 192..223 doublewords), the operation from what is left
  if (sub >= 96 && sub < 224) {
    const uint32_t width = (sub - 96) / 32;
    const uint32_t rel = sub - 32 * width;
    bool ok = false;
    switch (width) {
      case 0: ok = int_lane_op<int8_t>(rel, out, a, b, count); break;
      case 1: ok = int_lane_op<int16_t>(rel, out, a, b, count); break;
      case 2: ok = int_lane_op<int32_t>(rel, out, a, b, count); break;
      default: ok = int_lane_op<int64_t>(rel, out, a, b, count); break;
    }
    if (ok) return nullptr;
  }
  return "invalid instruction";
}

}  // namespace wasm
