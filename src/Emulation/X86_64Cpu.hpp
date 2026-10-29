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
#pragma once
#include <stdint.h>
#include "Emulation/GuestCpu.hpp"
#include "Emulation/SoftFloatExt.hpp"

namespace emulation {

// An x86-64 user-mode interpreter: the integer instruction set (including the legacy prefixes, SIB/RIP-relative addressing, string instructions with REP, and
// the 64-bit multiply and divide), SYSCALL, CPUID, and the SSE2 integer and move instructions compilers emit for memcpy and friends. Not covered: x87,
// floating-point SSE arithmetic (the kernel has no floating point), AVX, and the system instructions; these stop the program with an invalid-opcode fault.
//
// Flags are computed eagerly after every instruction that sets them. Execution is one instruction per decode (no translation cache yet): the dispatch is a plain
// switch, which keeps it simple enough to test against compiler output and leaves room for a block translator behind the same IGuestCpu interface.
struct EvexOp;

class X86_64Cpu final : public IGuestCpu {
 public:
  struct Xmm {
    uint64_t lo = 0, hi = 0;
  };
  // Bits 255:128 of a vector register, plus bits 511:256 for AVX-512. Assigning an Xmm (what every VEX-encoded write does) clears the upper half, as the architecture says;
  // copying a YmmHigh keeps it all, and the EVEX code writes the fields directly.
  struct YmmHigh : Xmm {
    Xmm upper[2];
    YmmHigh() = default;
    YmmHigh(const YmmHigh&) = default;
    YmmHigh& operator=(const YmmHigh&) = default;
    YmmHigh& operator=(const Xmm& x) {
      lo = x.lo;
      hi = x.hi;
      upper[0] = Xmm();
      upper[1] = Xmm();
      return *this;
    }
  };
  struct Vec512 {
    uint64_t q[8] = {};
  };
  struct State {
    uint64_t r[32] = {};      // RAX..R15, and R16..R31 (Intel APX; the context image and cpuid only know the first 16 unless xsave component 19 is used)
    uint64_t rip = 0;
    uint64_t rflags = 2;
    uint64_t fs_base = 0, gs_base = 0;
    Xmm xmm[17];      // XMM0..15 (the low halves of YMM0..15) and a scratch slot that the AVX code uses
    YmmHigh ymm_high[16];      // bits 255:128 of YMM0..15 (and 511:256 of ZMM0..15)
    Xmm evex_lo[16];           // XMM16..31
    YmmHigh evex_hi[16];       // bits 511:128 of ZMM16..31
    uint64_t k[8] = {};        // the AVX-512 mask registers
    uint32_t mxcsr = 0x1F80;      // all exceptions masked, round to nearest
    // x87: eight 80-bit registers used as a stack (physical registers; TOP is in bits 11..13 of the status word), the control and status words, and which
    // physical registers hold a value (the others are "empty").
    softfloat::Ext st[8];
    uint16_t fpu_control = 0x37F, fpu_status = 0;
    uint8_t fpu_valid = 0;
  };
  static constexpr uint64_t kCarry = 1, kParity = 4, kAdjust = 0x10, kZero = 0x40, kSign = 0x80, kDirection = 0x400, kOverflow = 0x800;

  process::UserArch arch() const override { return process::UserArch::X86_64; }
  void reset(const process::ProgramStart& start) override;
  StopReason run(GuestMemory& memory, uint64_t max_instructions) override;
  bool pending_syscall(process::SyscallAbi abi, process::SyscallRequest* out) const override;
  void finish_syscall(uint64_t result) override { s_.r[0] = result; }
  void set_thread_pointers(bool fs_changed, uint64_t fs, bool gs_changed, uint64_t gs) override {
    if (fs_changed) s_.fs_base = fs;
    if (gs_changed) s_.gs_base = gs;
  }
  const GuestFault& fault() const override { return fault_; }
  uint64_t instructions() const override { return retired_; }
  bool get_x86_context(process::X86Context* out) const override;
  bool set_x86_context(const process::X86Context& context) override;
  UniquePointer<IGuestCpu> clone_for_thread(allocators::IAllocator& allocator, uint64_t stack_pointer, bool set_tls, uint64_t tls) const override;

  State& state() { return s_; }
  const State& state() const { return s_; }

 private:
  enum class Step : uint8_t { Ok, Syscall, Fault };
  // What the instruction being executed looks like so far.
  struct Decoded {
    uint8_t bytes[16] = {};
    uint32_t available = 0, position = 0;
    bool truncated = false;      // it needed more bytes than there were
    bool rex_present = false, rex_w = false, rex_r = false, rex_x = false, rex_b = false;
    bool rex_r4 = false, rex_x4 = false, rex_b4 = false;      // the APX REX2 bits that name registers 16..31
    bool rex2 = false;
    uint32_t ext_b() const { return (rex_b ? 8u : 0u) | (rex_b4 ? 16u : 0u); }      // the bits that REX.B / B4 add to a register number in an opcode or modrm.rm field
    bool apx_high() const { return rex_r4 || rex_x4 || rex_b4; }
    bool operand16 = false, address32 = false, rep = false, repne = false;
    uint8_t segment = 0;         // 0 none, 1 FS, 2 GS
    uint8_t mod = 0;
    uint8_t reg = 0, rm = 0;     // with the REX extensions applied
    bool rip_relative = false;
    int64_t displacement = 0;
    uint64_t ea = 0;
    bool sib_index = false;      // the SIB byte had an index register (AMX tile loads and stores use it as the row stride)
    uint64_t index_offset = 0;   // the index register scaled, already part of `ea`
  };

  uint8_t next_u8();
  uint64_t next_imm(uint32_t bytes, bool sign_extend);
  bool decode_modrm();
  bool finish_decode();
  uint64_t operand_size() const { return d_.rex_w ? 8 : d_.operand16 ? 2 : 4; }

  uint64_t get_reg(uint32_t index, uint32_t size) const;
  void set_reg(uint32_t index, uint32_t size, uint64_t value);
  bool load(GuestMemory& m, uint64_t address, uint32_t size, uint64_t* out);
  bool store(GuestMemory& m, uint64_t address, uint32_t size, uint64_t value);
  uint64_t memory_address() const;
  bool read_rm(GuestMemory& m, uint32_t size, uint64_t* out);
  bool write_rm(GuestMemory& m, uint32_t size, uint64_t value);
  bool read_xmm_rm(GuestMemory& m, Xmm* out);
  bool write_xmm_rm(GuestMemory& m, const Xmm& value);
  bool push(GuestMemory& m, uint64_t value);
  bool pop(GuestMemory& m, uint64_t* out);
  Step fail(uint8_t vector, uint64_t address = 0, uint32_t error = 0x10);      // (a page fault is by default an instruction fetch; data accesses say otherwise)

  uint64_t alu(uint32_t op, uint64_t a, uint64_t b, uint32_t size);
  uint64_t shift(uint32_t op, uint64_t value, uint32_t count, uint32_t size);
  void set_logic_flags(uint64_t result, uint32_t size);
  void set_result_flags(uint64_t result, uint32_t size);
  bool condition(uint32_t code) const;
  bool divide(bool is_signed, uint32_t size, uint64_t hi, uint64_t lo, uint64_t divisor, uint64_t* quotient, uint64_t* remainder);
  Step string_instruction(GuestMemory& m, uint8_t opcode);
  Step step(GuestMemory& m);
  Step step_0f(GuestMemory& m);
  Step step_0f_op(GuestMemory& m, uint8_t opcode);      // (the opcode byte after 0F already read)
  Step step_op(GuestMemory& m, uint8_t opcode);         // (a one-byte-map opcode, prefixes decoded)
  Step apx_promoted_vex(GuestMemory& m, uint8_t p0, uint8_t p1, uint8_t p2, uint32_t map, uint8_t opcode);      // X86_64Apx.cpp: BMI, kmov, AMX tile loads and cmpccxadd in the extended EVEX prefix
  Step cmpxadd_instruction(GuestMemory& m, uint8_t opcode, bool wide, uint32_t vvvv);      // cmpccxadd (VEX map 2, 66 E0..EF)
  Step apx_map4(GuestMemory& m, uint8_t p0, uint8_t p1, uint8_t p2, uint8_t opcode);      // Intel APX: EVEX map 4, the promoted legacy instructions
  Step step_sse(GuestMemory& m, uint8_t opcode);
  static bool sse_integer_valid(uint8_t op);
  static bool sse_integer_core(uint8_t op, Xmm& dst, const Xmm& src);
  // The floating-point SSE/SSE2 opcodes (soft-float); sets *handled false for an opcode that is not one of them.
  Step step_float(GuestMemory& m, uint8_t opcode, bool* handled);
  bool read_scalar(GuestMemory& m, uint32_t size, uint64_t* out);
  Step step_group_f(GuestMemory& m, uint8_t opcode);
  Step step_x87(GuestMemory& m, uint8_t opcode);      // D8..DF (X86_64X87.cpp)
  // AVX/AVX2/FMA/BMI (X86_64Vex.cpp)
  Step step_vex(GuestMemory& m, uint8_t first_byte);
  Step vex_simd(GuestMemory& m, uint8_t opcode, uint32_t flags, bool wide, uint32_t vvvv);
  Step vex_map2(GuestMemory& m, uint8_t opcode, bool wide, uint32_t vvvv);
  Step vex_map3(GuestMemory& m, uint8_t opcode, bool wide, uint32_t vvvv);
  Step legacy_sse_map(GuestMemory& m, uint32_t map, uint8_t opcode);      // SSSE3 / SSE4 without a VEX prefix (0F 38 and 0F 3A)
  Step crc32_instruction(GuestMemory& m, uint8_t opcode);
  Step step_evex(GuestMemory& m);      // X86_64Evex.cpp: AVX-512 (the 0x62 prefix)
  Step kmask_instruction(GuestMemory& m, uint32_t map, uint8_t opcode, bool wide, uint32_t vvvv);      // the VEX-encoded opmask instructions
  struct EvexContext {
    uint8_t map = 0, pp = 0, opcode = 0;
    bool w = false, memory = false, bcst = false, z = false, er = false;      // er: embedded rounding or SAE (EVEX.b with a register operand)
    uint32_t reg = 0, rm = 0, vvvv = 0, vl = 512, aaa = 0, imm = 0, rc = 0;
    uint64_t kmask = ~uint64_t{0};
    Vec512 old_dest;
  } ev_;
  bool ev_active(uint32_t i) const { return ((ev_.kmask >> (i & 63)) & 1) != 0; }
  bool ev_read_rm(GuestMemory& m, Vec512* out, uint32_t elem_bits, bool allow_broadcast, bool suppress_faults = false);
  void ev_write_masked(uint32_t dest, const Vec512& result, uint32_t elem_bits);
  void ev_write_k(uint64_t bits, uint32_t count);
  bool ev_store_vector(GuestMemory& m, const Vec512& v, uint32_t elem_bits, uint32_t bytes);
  Step evex_int(GuestMemory& m, const EvexOp& def);
  Step evex_misc(GuestMemory& m, const EvexOp& def);
  Step evex_fp(GuestMemory& m, const EvexOp& def);
  Step evex_fp16(GuestMemory& m, const EvexOp& def);
  Step evex_ext(GuestMemory& m, const EvexOp& def);
  Step evex_er(GuestMemory& m, const EvexOp& def);
  Step evex_bf16(GuestMemory& m, const EvexOp& def);
  Step evex_minmax(GuestMemory& m, const EvexOp& def);
  Step evex_fp8(GuestMemory& m, const EvexOp& def);      // X86_64EvexBf16.cpp: AVX10.2 FP8 conversions and vcvt2ps2phx
  Step evex_avx102(GuestMemory& m, const EvexOp& def);      // X86_64EvexBf16.cpp: AVX10.2 vdpphps, vmpsadbw and the saturating conversions      // X86_64EvexBf16.cpp: AVX10.2 vminmax and vcomx / vucomx      // X86_64EvexBf16.cpp: AVX10.2 bfloat16 arithmetic      // X86_64Misc.cpp: AVX-512 ER
  Step evex_vsib(GuestMemory& m, const EvexOp& def);
  Vec512 get_vec(uint32_t reg) const;
  void set_vec(uint32_t reg, const Vec512& value, uint32_t vl_bits);
  Step crypto_map(GuestMemory& m, uint32_t map, uint8_t opcode, bool wide, uint32_t vvvv);      // X86_64Crypto.cpp: AES-NI, PCLMULQDQ, SHA
  Step sha512_instruction(GuestMemory& m, uint8_t opcode, uint32_t vvvv);
  Step sm4_instruction(GuestMemory& m, uint32_t vvvv);
  Step sm3_instruction(GuestMemory& m, uint32_t map, uint8_t op, uint32_t vvvv);
  Step vex_gather(GuestMemory& m, uint8_t opcode, bool wide, uint32_t vvvv);
  Step vex_fma(GuestMemory& m, uint8_t opcode, bool wide, uint32_t vvvv);
  Step lwp_instruction(GuestMemory& m, uint32_t map, uint32_t vvvv);      // AMD LWP: llwpcb, slwpcb (XOP map 9), lwpins, lwpval (map A)
  bool lwp_flush(GuestMemory& m);
  bool lwp_insert(GuestMemory& m, uint8_t event_id, uint16_t flags, uint32_t data1, uint64_t data2, bool* full);
  Step fma4_instruction(GuestMemory& m, uint8_t opcode, bool wide, uint32_t vvvv);      // AMD FMA4 (VEX map 3)
  Step vpermil2_instruction(GuestMemory& m, uint8_t opcode, bool wide, uint32_t vvvv);      // AMD vpermil2ps / vpermil2pd (VEX map 3)
  Step xop_instruction(GuestMemory& m, uint32_t map, uint8_t opcode, uint32_t vvvv);      // AMD XOP and TBM (the 8F prefix)
  Step vex_bmi(GuestMemory& m, int map, uint8_t opcode, bool wide, uint32_t vvvv);
  Step xsave_family(GuestMemory& m, uint32_t kind);
  uint64_t xstate_in_use() const;      // XINUSE, xgetbv with ECX = 1: the components that are not in their initial state
  // X86_64Misc.cpp: the instructions that are neither SSE nor AVX nor x87 and did not fit the main switch
  Step group9(GuestMemory& m);                          // 0F C7: cmpxchg8b/16b, xsavec, rdrand, rdseed, rdpid
  // Intel AMX: the VEX-encoded tile instructions (0F38 map: 49, 4B, 5C, 5E, 6C).
  Step amx_instruction(GuestMemory& m, uint8_t opcode, uint32_t vvvv);
  Step amx_dot(uint32_t kind, uint32_t dst, uint32_t src1, uint32_t src2);
  static bool tile_config_valid(ConstByteView config);
  void tile_config_load(ConstByteView config);      // (valid, as checked by tile_config_valid)
  void tile_release();
  uint32_t tile_rows(uint32_t t) const { return tiles_.config[48 + t]; }
  uint32_t tile_colsb(uint32_t t) const { return tiles_.config[16 + 2 * t] | (uint32_t{tiles_.config[17 + 2 * t]} << 8); }
  bool tiles_configured() const { return tiles_.config[0] != 0; }
  // Segment registers and far transfers (X86_64Segment.cpp).
  Step segment_move(GuestMemory& m, uint8_t opcode);
  Step far_pointer_load(GuestMemory& m, uint8_t opcode);
  Step far_branch(GuestMemory& m, bool is_call);
  Step far_return(GuestMemory& m, uint8_t opcode);
  Step interrupt_return(GuestMemory& m);
  bool push_sized(GuestMemory& m, uint64_t value, uint32_t size);
  bool load_selector(uint32_t segment, uint16_t selector);
  Step system_0f01(GuestMemory& m);                     // 0F 01 with a register operand: xgetbv, xend, xtest, rdtscp
  Step adx_instruction(GuestMemory& m, bool overflow);  // adcx (66 0F 38 F6) and adox (F3 0F 38 F6)
  Step fsgsbase_instruction(GuestMemory& m);            // F3 0F AE /0../3 with a register operand (already decoded): rdfsbase, rdgsbase, wrfsbase, wrgsbase
  Step rao_instruction(GuestMemory& m, uint32_t kind);      // aadd / aand / aor / axor (RAO-INT): an atomic read-modify-write that leaves the flags alone
  Step direct_store(GuestMemory& m, uint8_t op);        // 0F 38 F8 (movdir64b) / F9 (movdiri)
  Step movnti(GuestMemory& m);                          // 0F C3
  // X86_64Mmx.cpp
  bool mmx_opcode(uint8_t op) const;                    // does this 0F opcode, with the prefixes seen, name an MMX instruction?
  Step mmx_instruction(GuestMemory& m, uint8_t op);
  Step mmx_convert(GuestMemory& m, uint8_t op);
  bool mmx_ssse3_opcode(uint32_t map, uint8_t op) const;
  Step mmx_ssse3(GuestMemory& m, uint32_t map, uint8_t op);
  void mmx_enter();
  uint64_t mmx_get(uint32_t index) const;
  void mmx_set(uint32_t index, uint64_t value);
  Step vex_via_evex(GuestMemory& m, uint32_t map, uint8_t op, bool wide_w, uint32_t vvvv);      // VEX forms of instructions that exist as EVEX (AVX-VNNI, AVX-IFMA, AVX-VNNI-INT8/16, vcvtneps2bf16)
  Step vex_ne_convert(GuestMemory& m, uint8_t op, bool wide_w, uint32_t vvvv);      // AVX-NE-CONVERT
  Step sse4a_bitfield(GuestMemory& m, uint8_t op);      // 66 0F 78 / 79 extrq, F2 0F 78 / 79 insertq
  uint64_t next_random();
  bool read_y(GuestMemory& m, uint32_t bytes, Xmm out[2]);      // the r/m operand of a VEX instruction: a register or memory, 16 or 32 bytes
  struct VexState {
    bool redirect = false;      // the register r/m operand of an SSE core is read from the scratch slot
    uint32_t half = 0;          // 0 or 1: the 128-bit half being processed (memory operands move by 16 bytes)
    bool active = false;
    bool wide = false;          // VEX.L: 256-bit
    uint32_t vvvv = 0;
  } vex_;
  // the x87 stack
  uint32_t x87_top() const { return (s_.fpu_status >> 11) & 7; }
  void x87_set_top(uint32_t top) { s_.fpu_status = static_cast<uint16_t>((s_.fpu_status & ~0x3800) | ((top & 7) << 11)); }
  softfloat::Ext& x87_phys(uint32_t stack_index) { return s_.st[(x87_top() + stack_index) & 7]; }
  const softfloat::Ext& x87_phys(uint32_t stack_index) const { return s_.st[(x87_top() + stack_index) & 7]; }
  bool x87_empty(uint32_t stack_index) const { return !((s_.fpu_valid >> ((x87_top() + stack_index) & 7)) & 1); }
  softfloat::Ext x87_read(uint32_t stack_index, softfloat::Env& env);
  bool x87_push(const softfloat::Ext& value, softfloat::Env& env);
  void x87_pop();
  void x87_set_cc(bool c0, bool c1, bool c2, bool c3);
  // The exception flags are sticky; C1 reports "rounded up" for an instruction that produced a result (unless it already set C1 for a stack overflow).
  void x87_finish(const softfloat::Env& env) {
    uint16_t status = static_cast<uint16_t>(s_.fpu_status | (env.flags & 0x3F));
    if (!x87_c1_set_) status = static_cast<uint16_t>((status & ~0x0200) | ((env.flags & 0x20) && env.rounded_up ? 0x0200 : 0));
    s_.fpu_status = status;
  }
  bool x87_c1_set_ = false;
  bool x87_underflowed_ = false;      // an operand of the instruction being executed was an empty register (its result is the real indefinite)
  uint16_t x87_tag_word() const;
  void x87_reset();
  bool x87_load_ext(GuestMemory& m, uint64_t address, softfloat::Ext* out);
  bool x87_store_ext(GuestMemory& m, uint64_t address, const softfloat::Ext& value);
  void save_fpu_image(uint8_t* image) const;      // the 512-byte FXSAVE layout
  void load_fpu_image(const uint8_t* image);

  // The AMX state: TILECFG (palette 1: palette byte, start row, 16 column widths in bytes, 16 row counts) and TILEDATA (eight tiles of 16 rows of 64 bytes).
  struct Tiles {
    uint8_t config[64] = {};
    uint8_t data[8][1024] = {};
  };
  Tiles tiles_;
  // AMD Lightweight Profiling (X86_64Lwp.cpp): the running session's copy of the control block fields; cb is the LWPCB address, 0 when LWP is off.
  struct Lwp {
    uint64_t cb = 0, buffer_base = 0, missed = 0;
    uint32_t buffer_size = 0, flags = 0, threshold = 0, random_bits = 0, head = 0;
    int32_t counter = 0, interval = 0;
  };
  Lwp lwp_;
  uint16_t selector_[6] = {0x2B, 0x33, 0x2B, 0x2B, 0x2B, 0x2B};      // es cs ss ds fs gs
  State s_;
  Decoded d_;
  GuestFault fault_;
  uint64_t retired_ = 0;
  uint64_t instruction_start_ = 0;
  uint64_t random_state_ = 0;      // rdrand / rdseed: a counter-seeded generator (not a cryptographic source)
};

}  // namespace emulation
