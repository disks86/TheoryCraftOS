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
#include "Emulation/HalfFloat.hpp"
#include "Emulation/X86_64EvexInternal.hpp"

namespace emulation {

using softfloat::Env;
using softfloat::Rounding;

// AVX-512: data movement, broadcasts, shuffles and permutes, conversions between integer widths, mask-to-vector moves, compress and expand.
X86_64Cpu::Step X86_64Cpu::evex_misc(GuestMemory& m, const EvexOp& defn) {
  const EvexOp* def = &defn;
  const uint32_t reg = ev_.reg, vvvv = ev_.vvvv, vl = ev_.vl, aaa = ev_.aaa, imm = ev_.imm, rm_reg = ev_.rm;
  const bool memory = ev_.memory, z = ev_.z, w = ev_.w;
  const uint32_t elem = def->elem ? def->elem : (w ? 64 : 32);
  const uint32_t vl_bytes = vl / 8;
  const Vec old_dest = ev_.old_dest;
  auto active = [&](uint32_t i) { return ev_active(i); };
  auto finish = [&]() {
    s_.rip += d_.position;
    return Step::Ok;
  };
  Vec a, b, result;
  const uint32_t count = vl / elem;

  switch (def->kind) {
    case kMoveScalar: {      // vmovss / vmovsd
      const uint32_t e = def->elem;
      const bool lane_active = aaa == 0 || active(0);
      if (def->sub == 1) {      // store: memory or the r/m register
        if (memory) {
          if (lane_active) {
            if (!store(m, memory_address(), e / 8, element(get_vec(reg), e, 0))) return Step::Fault;
          }
          return finish();
        }
        Vec dst = get_vec(vvvv);
        const Vec old = get_vec(rm_reg);
        set_element(dst, e, 0, lane_active ? element(get_vec(reg), e, 0) : (z ? 0 : element(old, e, 0)));
        set_vec(rm_reg, dst, 128);
        return finish();
      }
      if (memory) {
        uint64_t v = 0;
        if (lane_active && !load(m, memory_address(), e / 8, &v)) return Step::Fault;
        result.q[0] = 0;
        set_element(result, e, 0, lane_active ? v : (z ? 0 : element(old_dest, e, 0)));
        set_vec(reg, result, 128);
        return finish();
      }
      result = get_vec(vvvv);
      const Vec src = get_vec(rm_reg);
      set_element(result, e, 0, lane_active ? element(src, e, 0) : (z ? 0 : element(old_dest, e, 0)));
      set_vec(reg, result, 128);
      return finish();
    }
    case kMoveGpr: {      // vmovd / vmovq
      const uint32_t e = def->elem;
      if (def->sub == 0) {      // xmm <- r/m
        uint64_t v;
        if (memory) {
          if (!load(m, memory_address(), e / 8, &v)) return Step::Fault;
        } else {
          v = get_reg(d_.rm, e / 8);
        }
        result.q[0] = v & width_mask(e);
        set_vec(reg, result, 128);
        return finish();
      }
      if (def->sub == 2) {      // vmovq xmm <- xmm/m64, vmovw xmm <- xmm/m16
        uint64_t v;
        if (memory) {
          if (!load(m, memory_address(), e / 8, &v)) return Step::Fault;
        } else {
          v = get_vec(rm_reg).q[0] & width_mask(e);
        }
        result.q[0] = v;
        set_vec(reg, result, 128);
        return finish();
      }
      const uint64_t v = get_vec(reg).q[0] & width_mask(e);
      if (def->sub == 1) {      // r/m <- xmm
        if (memory) {
          if (!store(m, memory_address(), e / 8, v)) return Step::Fault;
        } else {
          set_reg(d_.rm, e < 32 ? 4 : e / 8, v);      // (vmovw zero-extends into the 32-bit register)
        }
      } else if (memory) {      // vmovq xmm/m64 <- xmm (vmovd xmm/m32 <- xmm)
        if (!store(m, memory_address(), e / 8, v)) return Step::Fault;
      } else {
        result.q[0] = v;
        set_vec(rm_reg, result, 128);
      }
      return finish();
    }
    case kDup: {
      if (def->sub == 6 && vl == 128 && memory) {
        uint64_t v;
        if (!load(m, memory_address(), 8, &v)) return Step::Fault;
        b.q[0] = v;
      } else if (!ev_read_rm(m, &b, elem, false, false)) {
        return Step::Fault;
      }
      for (uint32_t i = 0; i < vl / 64; i += 2) {
        if (def->sub == 6) {
          result.q[i] = result.q[i + 1] = b.q[i];
        }
      }
      if (def->sub != 6) {
        for (uint32_t i = 0; i < vl / 32; i += 2) {
          const uint32_t from = def->sub == 7 ? i : i + 1;
          set_element(result, 32, i, element(b, 32, from));
          set_element(result, 32, i + 1, element(b, 32, from));
        }
      }
      ev_write_masked(reg, result, def->sub == 6 ? 64 : 32);
      return finish();
    }
    case kMovHL: {
      if (def->sub == 1 || def->sub == 3) {      // stores of the low / high qword
        if (!memory) return fail(6);
        if (!store(m, memory_address(), 8, get_vec(reg).q[def->sub == 1 ? 0 : 1])) return Step::Fault;
        return finish();
      }
      result = get_vec(vvvv);
      uint64_t v;
      if (memory) {
        if (!load(m, memory_address(), 8, &v)) return Step::Fault;
      } else {
        if (ev_.pp != 0) return fail(6);
        v = get_vec(rm_reg).q[def->sub == 0 ? 1 : 0];      // vmovhlps takes the high qword, vmovlhps the low one
      }
      result.q[def->sub == 0 ? 0 : 1] = v;
      set_vec(reg, result, 128);
      return finish();
    }
    case kExtractElem: {
      const uint32_t e = def->sub & 0x7F, n = 128 / e;
      const uint64_t v = element(get_vec((def->sub & 0x80) ? rm_reg : reg), e, imm % n);
      if (def->sub & 0x80) {      // vpextrw (C5): a general register
        set_reg(d_.reg, 4, v);
        return finish();
      }
      if (memory) {
        if (!store(m, memory_address(), e / 8, v)) return Step::Fault;
      } else {
        set_reg(d_.rm, e == 64 ? 8 : 4, v);
      }
      return finish();
    }
    case kInsertElem: {
      const uint32_t e = def->sub & 0x7F;
      result = get_vec(vvvv);
      if (def->sub & 0x80) {      // vinsertps
        uint64_t v;
        uint32_t src_index = (imm >> 6) & 3;
        if (memory) {
          if (!load(m, memory_address(), 4, &v)) return Step::Fault;
          src_index = 0;
        } else {
          v = element(get_vec(rm_reg), 32, src_index);
        }
        set_element(result, 32, (imm >> 4) & 3, v);
        for (uint32_t i = 0; i < 4; ++i)
          if ((imm >> i) & 1) set_element(result, 32, i, 0);
        set_vec(reg, result, 128);
        return finish();
      }
      uint64_t v;
      if (memory) {
        if (!load(m, memory_address(), e / 8, &v)) return Step::Fault;
      } else {
        v = get_reg(d_.rm, e == 64 ? 8 : 4);
      }
      set_element(result, e, imm % (128 / e), v & width_mask(e));
      set_vec(reg, result, 128);
      return finish();
    }
    case kHalfConv: {
      Env env;
      env.mode = ev_.er ? static_cast<Rounding>(ev_.rc) : static_cast<Rounding>((s_.mxcsr >> 13) & 3);
      env.daz = (s_.mxcsr & 0x40) != 0;
      env.ftz = (s_.mxcsr & 0x8000) != 0;
      const uint32_t n = vl / 32;
      uint32_t flags = 0;
      if (def->sub == 0) {      // vcvtph2ps
        const uint32_t saved = ev_.vl;
        ev_.vl = vl / 2;
        if (!ev_read_rm(m, &b, 16, false, false)) {
          ev_.vl = saved;
          return Step::Fault;
        }
        ev_.vl = saved;
        for (uint32_t i = 0; i < n; ++i) {
          if (aaa != 0 && !active(i)) continue;
          const uint32_t h = static_cast<uint32_t>(element(b, 16, i));
          if (((h >> 10) & 31) == 31 && (h & 0x3FF) && !(h & 0x200)) flags |= softfloat::kInvalid;
          set_element(result, 32, i, half::half_to_single(static_cast<uint16_t>(h)));
        }
        ev_write_masked(reg, result, 32);
        if (!ev_.er) s_.mxcsr |= flags;
        return finish();
      }
      const Vec src = get_vec(reg);
      const Rounding mode = (imm & 4) ? static_cast<Rounding>((s_.mxcsr >> 13) & 3) : static_cast<Rounding>(imm & 3);
      for (uint32_t i = 0; i < n; ++i) {
        if (aaa != 0 && !active(i)) continue;
        uint32_t bits = static_cast<uint32_t>(element(src, 32, i));
        if (env.daz && (bits & 0x7F800000u) == 0) bits &= 0x80000000u;      // (MXCSR.DAZ: a denormal single is a zero)
        set_element(result, 16, i, half::single_to_half(bits, mode, &flags));
      }
      if (memory) {
        if (!ev_store_vector(m, result, 16, n * 2)) return Step::Fault;
      } else {
        Vec out = result;
        if (aaa != 0) {
          const Vec old = get_vec(rm_reg);
          for (uint32_t i = 0; i < n; ++i)
            if (!active(i)) set_element(out, 16, i, z ? 0 : element(old, 16, i));
        }
        set_vec(rm_reg, out, n * 16 < 128 ? 128 : n * 16);
      }
      if (!ev_.er) s_.mxcsr |= flags;
      return finish();
    }
    case kBroadcast: case kBroadcastGpr: {
      const uint32_t e = def->elem;
      uint32_t chunk;      // bytes
      Vec src;
      if (def->kind == kBroadcastGpr) {
        if (memory) return fail(6);
        chunk = e / 8;
        src.q[0] = get_reg(d_.rm, e == 64 ? 8 : 4);
        src.q[0] &= width_mask(e);
      } else {
        chunk = def->sub & 0x3F;
        if ((def->sub & 0x80) && !memory) return fail(6);
        if ((def->sub & 0x40) && vl < 512) return fail(6);
        if (memory) {
          for (uint32_t k = 0; k < chunk; k += 8) {
            const uint32_t n = chunk - k < 8 ? chunk - k : 8;
            uint64_t v;
            if (!load(m, memory_address() + k, n, &v)) return Step::Fault;
            src.q[k / 8] = v;
          }
        } else {
          src = get_vec(rm_reg);
          for (uint32_t k = chunk; k < 64; ++k) src.q[k / 8] &= ~(uint64_t{0xFF} << (8 * (k & 7)));
        }
      }
      // chunk bytes repeated across the vector
      for (uint32_t pos = 0; pos < vl_bytes; pos += chunk) {
        for (uint32_t k = 0; k < chunk; ++k) {
          const uint32_t from = k, to = pos + k;
          const uint64_t byte = (src.q[from / 8] >> (8 * (from & 7))) & 0xFF;
          result.q[to / 8] |= byte << (8 * (to & 7));
        }
      }
      ev_write_masked(reg, result, e);
      return finish();
    }
    case kShufImm: {
      if (!ev_read_rm(m, &b, elem, def->tuple == kFull)) return Step::Fault;
      switch (def->sub) {
        case 0: case 3:      // vpshufd, vpermilps: four dwords of each lane
          for (uint32_t l = 0; l < vl / 128; ++l)
            for (uint32_t i = 0; i < 4; ++i) set_element(result, 32, 4 * l + i, element(b, 32, 4 * l + ((imm >> (2 * i)) & 3)));
          break;
        case 1: case 2:      // vpshufhw, vpshuflw
          for (uint32_t l = 0; l < vl / 128; ++l)
            for (uint32_t i = 0; i < 8; ++i) {
              const bool moved = def->sub == 1 ? i >= 4 : i < 4;
              const uint32_t base = (def->sub == 1 ? 4 : 0), sel = (imm >> (2 * (i & 3))) & 3;
              set_element(result, 16, 8 * l + i, moved ? element(b, 16, 8 * l + base + sel) : element(b, 16, 8 * l + i));
            }
          break;
        case 4:      // vpermilpd
          for (uint32_t i = 0; i < vl / 64; ++i) set_element(result, 64, i, element(b, 64, (i & ~1u) + ((imm >> i) & 1)));
          break;
        default:      // vpermq / vpermpd: four qwords of each 256-bit half
          for (uint32_t h = 0; h < vl / 256; ++h)
            for (uint32_t i = 0; i < 4; ++i) set_element(result, 64, 4 * h + i, element(b, 64, 4 * h + ((imm >> (2 * i)) & 3)));
          break;
      }
      ev_write_masked(reg, result, def->sub == 1 || def->sub == 2 ? 16 : (def->sub >= 4 ? 64 : 32));
      return finish();
    }
    case kPermute: {
      const Vec index = get_vec(vvvv);
      if (!ev_read_rm(m, &b, elem, def->tuple == kFull)) return Step::Fault;
      const uint32_t n = vl / elem;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = element(index, elem, i);
        uint64_t r;
        switch (def->sub) {
          case 1: r = element(index, 32, (i & ~3u) + (x & 3)); break;      // vpermilps: the source is vvvv, the control the r/m operand
          case 2: r = element(index, 64, (i & ~1u) + ((x >> 1) & 1)); break;
          default: r = element(b, elem, x & (n - 1)); break;
        }
        if (def->sub == 1 || def->sub == 2) {
          const uint64_t ctl = element(b, elem, i);
          r = def->sub == 1 ? element(index, 32, (i & ~3u) + (ctl & 3)) : element(index, 64, (i & ~1u) + ((ctl >> 1) & 1));
        }
        set_element(result, elem, i, r);
      }
      ev_write_masked(reg, result, elem);
      return finish();
    }
    case kPermute2: {
      if (!ev_read_rm(m, &b, elem, def->tuple == kFull)) return Step::Fault;
      const uint32_t n = vl / elem;
      const Vec first = def->sub == 0 ? get_vec(vvvv) : old_dest, index = def->sub == 0 ? old_dest : get_vec(vvvv);
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = element(index, elem, i);
        const uint32_t sel = static_cast<uint32_t>(x & (n - 1));
        set_element(result, elem, i, (x & n) ? element(b, elem, sel) : element(first, elem, sel));
      }
      ev_write_masked(reg, result, elem);
      return finish();
    }
    case kInsert: {
      const uint32_t chunk = def->sub, e = def->elem;
      if (chunk >= vl_bytes) return fail(6);
      Vec ins;
      for (uint32_t k = 0; k < chunk; k += 8) {
        uint64_t v;
        if (memory) {
          if (!load(m, memory_address() + k, 8, &v)) return Step::Fault;
        } else {
          v = get_vec(rm_reg).q[k / 8];
        }
        ins.q[k / 8] = v;
      }
      result = get_vec(vvvv);
      const uint32_t lanes = vl_bytes / chunk;
      const uint32_t at = (imm & (lanes - 1)) * chunk;
      for (uint32_t k = 0; k < chunk; ++k) {
        const uint32_t to = at + k;
        result.q[to / 8] = (result.q[to / 8] & ~(uint64_t{0xFF} << (8 * (to & 7)))) | (((ins.q[k / 8] >> (8 * (k & 7))) & 0xFF) << (8 * (to & 7)));
      }
      ev_write_masked(reg, result, e);
      return finish();
    }
    case kExtract: {
      const uint32_t chunk = def->sub, e = def->elem;
      const Vec src = get_vec(reg);
      const uint32_t lanes = vl_bytes / chunk;
      if (lanes < 2) return fail(6);
      const uint32_t from = (imm & (lanes - 1)) * chunk;
      for (uint32_t k = 0; k < chunk; ++k) {
        const uint32_t f = from + k;
        result.q[k / 8] |= ((src.q[f / 8] >> (8 * (f & 7))) & 0xFF) << (8 * (k & 7));
      }
      if (memory) {
        if (!ev_store_vector(m, result, e, chunk)) return Step::Fault;
        return finish();
      }
      Vec out = result;
      if (aaa != 0) {
        const Vec old = get_vec(rm_reg);
        for (uint32_t i = 0; i < chunk * 8 / e; ++i)
          if (!active(i)) set_element(out, e, i, z ? 0 : element(old, e, i));
      }
      set_vec(rm_reg, out, chunk * 8);
      return finish();
    }
    case kBlend: {
      a = get_vec(vvvv);
      if (!ev_read_rm(m, &b, elem, def->tuple == kFull)) return Step::Fault;
      for (uint32_t i = 0; i < count; ++i) set_element(result, elem, i, (aaa == 0 || active(i)) ? element(b, elem, i) : (z ? 0 : element(a, elem, i)));
      set_vec(reg, result, vl);
      return finish();
    }
    case kAlign: {
      a = get_vec(vvvv);
      if (!ev_read_rm(m, &b, elem, true)) return Step::Fault;
      const uint32_t n = vl / elem;
      for (uint32_t i = 0; i < n; ++i) {
        const uint32_t idx = (i + (imm & (n - 1))) & (2 * n - 1);
        set_element(result, elem, i, idx < n ? element(b, elem, idx) : element(a, elem, idx - n));
      }
      ev_write_masked(reg, result, elem);
      return finish();
    }
    case kShuf128: {
      a = get_vec(vvvv);
      if (!ev_read_rm(m, &b, elem, true)) return Step::Fault;
      const uint32_t lanes = vl / 128;
      for (uint32_t l = 0; l < lanes; ++l) {
        const Vec& from = l < lanes / 2 ? a : b;
        const uint32_t sel = lanes == 4 ? (imm >> (2 * l)) & 3 : (imm >> l) & 1;
        result.q[2 * l] = from.q[2 * sel];
        result.q[2 * l + 1] = from.q[2 * sel + 1];
      }
      ev_write_masked(reg, result, elem);
      return finish();
    }
    case kIntUnary: {
      if (!ev_read_rm(m, &b, elem, def->tuple == kFull, true)) return Step::Fault;
      for (uint32_t i = 0; i < count; ++i) {
        const uint64_t x = element(b, elem, i);
        uint64_t r = 0;
        switch (def->sub) {
          case 0: r = signed_of(x, elem) < 0 ? uint64_t{0} - static_cast<uint64_t>(signed_of(x, elem)) : x; break;      // (the most negative value stays itself)
          case 1: {
            uint32_t n = 0;
            while (n < elem && !((x >> (elem - 1 - n)) & 1)) ++n;
            r = n;
            break;
          }
          case 2: {
            uint64_t v = x;
            for (; v; v &= v - 1) ++r;
            break;
          }
          default:      // vpconflict: a bit for each earlier element that equals this one
            for (uint32_t j = 0; j < i; ++j)
              if (element(b, elem, j) == x) r |= uint64_t{1} << j;
            break;
        }
        set_element(result, elem, i, r & width_mask(elem));
      }
      ev_write_masked(reg, result, elem);
      return finish();
    }
    case kExtend: {
      const uint32_t src_bytes = def->sub & 0x3F, dst = def->elem;
      const bool is_signed = def->sub & 0x80;
      const uint32_t n = vl / dst;
      const uint32_t src_total = n * src_bytes;
      // read the source: a register's low bytes or src_total bytes of memory
      Vec src;
      if (memory) {
        for (uint32_t k = 0; k < src_total; k += 8) {
          const uint32_t cnt = src_total - k < 8 ? src_total - k : 8;
          uint64_t v;
          if (!load(m, memory_address() + k, cnt, &v)) return Step::Fault;
          src.q[k / 8] = v;
        }
      } else {
        src = get_vec(rm_reg);
      }
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = element(src, src_bytes * 8, i);
        set_element(result, dst, i, (is_signed ? static_cast<uint64_t>(signed_of(x, src_bytes * 8)) : x) & width_mask(dst));
      }
      ev_write_masked(reg, result, dst);
      return finish();
    }
    case kNarrow: {
      const uint32_t src_bits = (def->sub & 0x0F) * 8, dst = def->elem, mode = (def->sub >> 4) & 3;
      const Vec src = get_vec(reg);
      const uint32_t n = vl / src_bits;
      for (uint32_t i = 0; i < n; ++i) {
        const uint64_t x = element(src, src_bits, i);
        uint64_t r;
        if (mode == 0) r = x & width_mask(dst);
        else if (mode == 1) r = evex_saturate_signed(signed_of(x, src_bits), dst);
        else r = x > width_mask(dst) ? width_mask(dst) : x;
        set_element(result, dst, i, r);
      }
      const uint32_t out_bytes = n * dst / 8;
      if (memory) {
        if (!ev_store_vector(m, result, dst, out_bytes)) return Step::Fault;
        return finish();
      }
      Vec out = result;
      if (aaa != 0) {
        const Vec old = get_vec(rm_reg);
        for (uint32_t i = 0; i < n; ++i)
          if (!active(i)) set_element(out, dst, i, z ? 0 : element(old, dst, i));
      }
      set_vec(rm_reg, out, out_bytes * 8 < 128 ? 128 : out_bytes * 8);
      return finish();
    }
    case kMaskMove: {
      const uint32_t e = def->elem;
      if (def->sub == 0) {      // vpmovm2*: all ones where the mask bit is set
        const Vec none;
        for (uint32_t i = 0; i < vl / e; ++i) set_element(result, e, i, ((s_.k[d_.rm & 7] >> i) & 1) ? width_mask(e) : 0);
        set_vec(reg, result, vl);
      } else {      // vpmov*2m
        const Vec src = get_vec(rm_reg);
        uint64_t bits = 0;
        for (uint32_t i = 0; i < vl / e; ++i) bits |= ((element(src, e, i) >> (e - 1)) & 1) << i;
        s_.k[reg & 7] = bits;
      }
      return finish();
    }
    case kCompress: {
      const Vec src = get_vec(reg);
      uint32_t k = 0;
      for (uint32_t i = 0; i < count; ++i) {
        if (aaa != 0 && !active(i)) continue;
        set_element(result, elem, k++, element(src, elem, i));
      }
      if (memory) {
        for (uint32_t i = 0; i < k; ++i)
          if (!store(m, memory_address() + uint64_t{i} * (elem / 8), elem / 8, element(result, elem, i))) return Step::Fault;
        return finish();
      }
      Vec out = get_vec(rm_reg);
      for (uint32_t i = 0; i < count; ++i) {
        if (i < k) set_element(out, elem, i, element(result, elem, i));
        else set_element(out, elem, i, z ? 0 : element(out, elem, i));
      }
      set_vec(rm_reg, out, vl);
      return finish();
    }
    case kExpand: {
      Vec src;
      uint32_t need = 0;
      for (uint32_t i = 0; i < count; ++i)
        if (aaa == 0 || active(i)) ++need;
      if (memory) {
        for (uint32_t i = 0; i < need; ++i) {
          uint64_t v;
          if (!load(m, memory_address() + uint64_t{i} * (elem / 8), elem / 8, &v)) return Step::Fault;
          set_element(src, elem, i, v);
        }
      } else {
        src = get_vec(rm_reg);
      }
      uint32_t k = 0;
      result = old_dest;
      for (uint32_t i = 0; i < count; ++i) {
        if (aaa == 0 || active(i)) set_element(result, elem, i, element(src, elem, k++));
        else if (z) set_element(result, elem, i, 0);
      }
      set_vec(reg, result, vl);
      return finish();
    }
    default: return fail(6);
  }
}

}  // namespace emulation

namespace emulation {

// The gathers and scatters: the memory operand has a vector index (VSIB), so it is decoded here instead of by decode_modrm.
X86_64Cpu::Step X86_64Cpu::evex_vsib(GuestMemory& m, const EvexOp& def) {
  auto bad_decode = [&]() { return fail(d_.available < 15 ? 14 : 6, s_.rip + d_.available); };
  const uint8_t modrm = next_u8();
  if ((modrm >> 6) == 3 || (modrm & 7) != 4) return fail(6);
  const uint32_t mod = modrm >> 6;
  const uint32_t reg = ((modrm >> 3) & 7) | (d_.rex_r ? 8 : 0) | ev_.reg;
  const uint8_t sib = next_u8();
  const uint32_t scale = sib >> 6;
  const uint32_t index = ((sib >> 3) & 7) | (d_.rex_x ? 8 : 0) | ((ev_.vvvv & 16) ? 16 : 0);
  const uint32_t base_reg = (sib & 7) | d_.ext_b();
  const bool no_base = (sib & 7) == 5 && mod == 0;
  int64_t disp = 0;
  const uint32_t elem = def.elem, index_bits = def.sub;
  if (mod == 1) disp = signed_of(next_u8(), 8) * static_cast<int64_t>(elem / 8);
  else if (mod == 2 || no_base) disp = signed_of(next_imm(4, false), 32);
  if (d_.truncated) return bad_decode();
  if (ev_.aaa == 0) return fail(6);      // the mask register is required
  if ((ev_.vvvv & 15) != 15 - 0 && false) return fail(6);
  const uint64_t base = (no_base ? 0 : s_.r[base_reg]) + static_cast<uint64_t>(disp);
  const uint32_t vl = ev_.vl, n = vl / (elem > index_bits ? elem : index_bits);
  const uint32_t aaa = ev_.aaa;
  const Vec idx = get_vec(index);
  const bool scatter = def.kind == kScatter;
  if (!scatter && reg == index) return fail(6);      // (the destination may not be the index register)
  Vec data = scatter ? get_vec(reg) : get_vec(reg);
  uint64_t kmask = s_.k[aaa];
  for (uint32_t i = 0; i < n; ++i) {
    if (!((kmask >> i) & 1)) continue;
    const uint64_t addr = base + (static_cast<uint64_t>(signed_of(element(idx, index_bits, i), index_bits)) << scale);
    if (scatter) {
      if (!store(m, addr, elem / 8, element(data, elem, i))) {
        s_.k[aaa] = kmask;
        return Step::Fault;
      }
    } else {
      uint64_t v;
      if (!load(m, addr, elem / 8, &v)) {
        s_.k[aaa] = kmask;
        set_vec(reg, data, n * elem < 128 ? 128 : n * elem);
        return Step::Fault;
      }
      set_element(data, elem, i, v);
    }
    kmask &= ~(uint64_t{1} << i);
  }
  s_.k[aaa] = 0;
  if (!scatter) {
    for (uint32_t i = n; i < 512 / elem; ++i) set_element(data, elem, i, 0);      // (the elements the gather does not name are cleared)
    set_vec(reg, data, n * elem < 128 ? 128 : n * elem);
  }
  s_.rip += d_.position;
  return Step::Ok;
}

}  // namespace emulation
