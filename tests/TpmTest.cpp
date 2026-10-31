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

// The TPM 2.0 code against models of the two interfaces (the FIFO "TIS" with a small burst count, the CRB with its control area and buffers) and of a TPM that answers Startup, GetRandom,
// PCR_Read and GetCapability: the register protocol of each interface, the marshalling of the commands, the parsing of the responses (also damaged ones), and the ACPI table.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "Hardware/Tpm/TpmCrb.hpp"
#include "Hardware/Tpm/TpmDevice.hpp"
#include "Hardware/Tpm/TpmDriver.hpp"
#include "Hardware/Tpm/TpmTis.hpp"

namespace hardware { void delay_us(uint64_t) {} }
using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  std::vector<void*> blocks;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment); blocks.push_back(p); return p; }
  void deallocate(void*) override {}
  ~TestAllocator() { for (void* p : blocks) free(p); }
};

typedef std::vector<uint8_t> Bytes;

// The TPM itself: a command in, a response out.
struct FakeTpm {
  bool started = false;
  uint32_t seed = 1;
  bool lie_about_size = false, wrong_code = false, short_random = false;
  uint32_t commands = 0;

  static void put16(Bytes& b, uint32_t v) { b.push_back(v >> 8); b.push_back(v); }
  static void put32(Bytes& b, uint32_t v) { put16(b, v >> 16); put16(b, v & 0xFFFF); }
  static uint32_t get16(const Bytes& c, size_t at) { return (c[at] << 8) | c[at + 1]; }
  static uint32_t get32(const Bytes& c, size_t at) { return (get16(c, at) << 16) | get16(c, at + 2); }
  Bytes response(uint32_t rc, const Bytes& body) {
    Bytes r;
    put16(r, rc == 0 ? 0x8001 : 0x8001);
    put32(r, static_cast<uint32_t>(10 + body.size()) + (lie_about_size ? 4 : 0));
    put32(r, wrong_code && rc == 0 ? 0x101 : rc);
    r.insert(r.end(), body.begin(), body.end());
    return r;
  }
  Bytes run(const Bytes& c) {
    ++commands;
    if (c.size() < 10 || get32(c, 2) != c.size()) return response(0x1E, Bytes());          // TPM_RC_COMMAND_SIZE
    const uint32_t code = get32(c, 6);
    Bytes body;
    if (code == 0x144) {                                                                     // Startup
      if (started) return response(0x100, Bytes());
      started = true;
      return response(0, Bytes());
    }
    if (!started) return response(0x100, Bytes());
    if (code == 0x17B) {                                                                     // GetRandom
      uint32_t want = get16(c, 10);
      if (want > 32) want = 32;
      if (short_random && want > 4) want = 4;
      put16(body, want);
      for (uint32_t i = 0; i < want; ++i) { seed = seed * 1664525u + 1013904223u; body.push_back(seed >> 24); }
      return response(0, body);
    }
    if (code == 0x17E) {                                                                     // PCR_Read: PCR n holds 32 bytes of n+1
      const uint32_t select_bytes = c[16];
      uint32_t index = 0;
      for (uint32_t i = 0; i < select_bytes; ++i)
        for (uint32_t b = 0; b < 8; ++b)
          if (c[17 + i] & (1u << b)) index = i * 8 + b;
      put32(body, 7);                                                                        // update counter
      put32(body, 1);
      put16(body, get16(c, 14));
      body.push_back(static_cast<uint8_t>(select_bytes));
      for (uint32_t i = 0; i < select_bytes; ++i) body.push_back(c[17 + i]);
      put32(body, 1);
      put16(body, 32);
      for (int i = 0; i < 32; ++i) body.push_back(static_cast<uint8_t>(index + 1));
      return response(0, body);
    }
    if (code == 0x17A) {                                                                     // GetCapability
      body.push_back(0);
      put32(body, get32(c, 10));
      put32(body, 1);
      const uint32_t property = get32(c, 14);
      put32(body, property);
      put32(body, property == 0x105 ? 0x49465800 : property == 0x10B ? 0x00070002 : 0x322E3000);
      return response(0, body);
    }
    return response(0x143, Bytes());                                                         // TPM_RC_COMMAND_CODE
  }
};

// The FIFO interface in front of it.
struct FakeTisRegisters final : ITpmRegisters {
  FakeTpm* tpm;
  uint8_t access = 0x80;                      // valid, nobody has the locality
  uint32_t burst = 4;
  bool ready = false, go_seen = false, absent = false, never_answer = false, short_answer = false;
  Bytes in, out;
  size_t out_at = 0;
  uint32_t max_command = 4096;

  explicit FakeTisRegisters(FakeTpm* t) : tpm(t) {}
  uint8_t sts_byte() const {
    uint8_t s = 0x80;
    if (ready) s |= 0x40;
    if (!out.empty() && out_at < out.size()) s |= 0x10;
    if (ready && !go_seen && !in.empty() && in.size() < expected()) s |= 0x08;
    return s;
  }
  size_t expected() const { return in.size() >= 6 ? ((in[2] << 24) | (in[3] << 16) | (in[4] << 8) | in[5]) : 10; }
  uint8_t read8(uint32_t offset) override {
    if (absent) return 0xFF;
    if (offset == tis::kAccess) return access;
    if (offset == tis::kSts) return sts_byte();
    if (offset == tis::kDataFifo) return out_at < out.size() ? out[out_at++] : 0xFF;
    return 0;
  }
  void write8(uint32_t offset, uint8_t v) override {
    if (offset == tis::kAccess && (v & tis::kAccessRequestUse)) access |= tis::kAccessActiveLocality;
    if (offset == tis::kSts) {
      if (v & tis::kStsCommandReady) { ready = true; in.clear(); out.clear(); out_at = 0; go_seen = false; }
      if (v & tis::kStsGo) {
        go_seen = true;
        if (never_answer) return;
        out = tpm->run(in);
        if (short_answer && out.size() > 12) out.resize(12);
        out_at = 0;
      }
    }
    if (offset == tis::kDataFifo && ready && !go_seen) in.push_back(v);
  }
  uint32_t read32(uint32_t offset) override {
    if (offset == tis::kSts) return (uint32_t{burst} << 8) | sts_byte();
    if (offset == tis::kDidVid) return 0x001B15D1;
    return read8(offset);
  }
  void write32(uint32_t offset, uint32_t v) override { write8(offset, static_cast<uint8_t>(v)); }
};

// The CRB in front of it: the control area at 0x40, the buffers at 0x80.
struct FakeCrbRegisters final : ITpmRegisters {
  FakeTpm* tpm;
  uint8_t mem[0x1000] = {};
  bool fatal = false, stuck = false, bad_address = false, cancelled = false;
  explicit FakeCrbRegisters(FakeTpm* t) : tpm(t) {
    set32(crb::kCmdSize, 0xF80);
    set32(crb::kCmdPaLow, 0xFED40080);
    set32(crb::kRspSize, 0xF80);
    set32(crb::kRspPa, 0xFED40080);
    set32(crb::kCtrlSts, crb::kStsIdle);
  }
  void set32(uint32_t o, uint32_t v) { for (int i = 0; i < 4; ++i) mem[o + i] = static_cast<uint8_t>(v >> (8 * i)); }
  uint32_t get32(uint32_t o) const { return mem[o] | (mem[o + 1] << 8) | (mem[o + 2] << 16) | (static_cast<uint32_t>(mem[o + 3]) << 24); }
  uint8_t read8(uint32_t o) override { return mem[o]; }
  void write8(uint32_t o, uint8_t v) override { mem[o] = v; }
  uint32_t read32(uint32_t o) override { return get32(o); }
  void write32(uint32_t o, uint32_t v) override {
    if (o == crb::kLocCtrl && (v & 1)) { set32(crb::kLocSts, 1); return; }
    if (o == crb::kCtrlReq) {
      if (v == crb::kReqCmdReady) { set32(crb::kCtrlSts, 0); set32(crb::kCtrlReq, 0); return; }
      if (v == crb::kReqGoIdle) { set32(crb::kCtrlSts, crb::kStsIdle); set32(crb::kCtrlReq, 0); return; }
    }
    if (o == crb::kCtrlStart && v == 1) {
      if (stuck) { set32(crb::kCtrlStart, 1); return; }
      Bytes c(mem + 0x80, mem + 0x80 + 4);
      const size_t n = (mem[0x80 + 2] << 24) | (mem[0x80 + 3] << 16) | (mem[0x80 + 4] << 8) | mem[0x80 + 5];
      c.assign(mem + 0x80, mem + 0x80 + (n < 0xF80 ? n : 0xF80));
      const Bytes r = tpm->run(c);
      for (size_t i = 0; i < r.size(); ++i) mem[0x80 + i] = r[i];
      if (fatal) set32(crb::kCtrlSts, crb::kStsFatal);
      set32(crb::kCtrlStart, 0);
      return;
    }
    if (o == crb::kCtrlCancel) { cancelled = cancelled || v == 1; set32(o, v); if (v == 1) set32(crb::kCtrlStart, 0); return; }
    if (bad_address && o == crb::kCmdPaLow) { set32(o, v); return; }
    set32(o, v);
  }
};

static void test_tis() {
  TestAllocator a;
  FakeTpm tpm;
  FakeTisRegisters regs(&tpm);
  TpmTis tis(regs);
  CHECK(tis.probe() && tis.vendor_device() == 0x001B15D1);
  Bytes cmd = {0x80, 0x01, 0, 0, 0, 12, 0, 0, 0x01, 0x44, 0, 0};                          // Startup(CLEAR)
  ByteBuffer response(a);
  CHECK(tis.transmit(ConstByteView(cmd.data(), cmd.size()), &response) && response.size() == 10 && response.view()[9] == 0 && tpm.started);
  CHECK(tis.transmit(ConstByteView(cmd.data(), cmd.size()), &response) && response.size() == 10 && response.view()[8] == 0x01);      // (already started: TPM_RC_INITIALIZE)
  CHECK((regs.access & tis::kAccessActiveLocality) && regs.ready);                         // the locality was taken and the TPM left ready
  TpmDevice device(tis, a);
  uint8_t bytes[100];
  CHECK(device.startup() && device.get_random(ByteView(bytes, 100)));                      // 4 commands of 32, 32, 32 and 4 bytes, each through a burst of 4
  CHECK(tpm.commands >= 6);
  // a TPM that is not there, one that never answers, one that answers less than it says
  FakeTisRegisters gone(&tpm);
  gone.absent = true;
  TpmTis none(gone);
  CHECK(!none.probe() && !none.transmit(ConstByteView(cmd.data(), cmd.size()), &response));
  FakeTisRegisters silent(&tpm);
  silent.never_answer = true;
  TpmTis s(silent);
  CHECK(!s.transmit(ConstByteView(cmd.data(), cmd.size()), &response));
  FakeTisRegisters cut(&tpm);
  cut.short_answer = true;
  TpmTis c2(cut);
  Bytes random = {0x80, 0x01, 0, 0, 0, 12, 0, 0, 0x01, 0x7B, 0, 32};
  CHECK(!c2.transmit(ConstByteView(random.data(), random.size()), &response));
  // commands that are not commands
  CHECK(!tis.transmit(ConstByteView(cmd.data(), 5), &response) && !tis.transmit(ConstByteView(), &response));
  Bytes cut_command(cmd.begin(), cmd.end() - 2);                                           // shorter than its own header says: the TPM still expects data
  CHECK(!tis.transmit(ConstByteView(cut_command.data(), cut_command.size()), &response));
}

static void test_crb() {
  TestAllocator a;
  FakeTpm tpm;
  FakeCrbRegisters regs(&tpm);
  TpmCrb crb(regs, 0xFED40000);
  CHECK(crb.probe());
  TpmDevice device(crb, a);
  CHECK(device.startup() && tpm.started);
  uint8_t bytes[70];
  CHECK(device.get_random(ByteView(bytes, 70)));
  bool differ = false;
  for (int i = 1; i < 70; ++i) differ = differ || bytes[i] != bytes[0];
  CHECK(differ);
  uint8_t pcr[32];
  CHECK(device.pcr_read(0, pcr) && pcr[0] == 1 && pcr[31] == 1 && device.pcr_read(7, pcr) && pcr[5] == 8 && device.pcr_read(23, pcr) && pcr[0] == 24 && !device.pcr_read(24, pcr));
  uint32_t value;
  CHECK(device.property(tpm::kPtManufacturer, &value) && value == 0x49465800 && device.property(tpm::kPtFirmwareVersion1, &value) && value == 0x00070002);
  CHECK(regs.get32(crb::kCtrlSts) == crb::kStsIdle);                                       // idle again
  // the status register says the TPM is in failure mode
  regs.fatal = true;
  CHECK(!device.get_random(ByteView(bytes, 4)));
  regs.fatal = false;
  regs.set32(crb::kCtrlSts, crb::kStsIdle);
  CHECK(device.get_random(ByteView(bytes, 4)));
  regs.set32(crb::kCtrlSts, crb::kStsFatal);
  CHECK(!device.get_random(ByteView(bytes, 4)));
  regs.set32(crb::kCtrlSts, crb::kStsIdle);
  // a command that never completes is cancelled
  regs.stuck = true;
  CHECK(!device.get_random(ByteView(bytes, 4)) && regs.cancelled);
  regs.stuck = false;
  // buffers outside the page, or sizes that do not fit, are refused
  regs.set32(crb::kCmdPaLow, 0xFED41000 + 1);
  CHECK(!device.get_random(ByteView(bytes, 4)));
  regs.set32(crb::kCmdPaLow, 0xFED40080);
  regs.set32(crb::kCmdSize, 0xFFFFFFF0);
  CHECK(!device.get_random(ByteView(bytes, 4)));
  regs.set32(crb::kCmdSize, 0xF80);
  CHECK(device.get_random(ByteView(bytes, 4)));
  FakeCrbRegisters empty(&tpm);
  empty.set32(crb::kCmdSize, 0);
  TpmCrb none(empty, 0xFED40000);
  CHECK(!none.probe());
}

static void test_device_failures() {
  TestAllocator a;
  FakeTpm tpm;
  FakeCrbRegisters regs(&tpm);
  TpmCrb crb(regs, 0xFED40000);
  TpmDevice device(crb, a);
  uint8_t bytes[40];
  CHECK(!device.get_random(ByteView(bytes, 40)) && device.last_response_code() == 0x100);   // before Startup the TPM says so
  CHECK(device.startup());
  tpm.wrong_code = true;
  CHECK(!device.get_random(ByteView(bytes, 4)) && device.last_response_code() == 0x101);
  tpm.wrong_code = false;
  tpm.short_random = true;                                                                   // a TPM that gives fewer bytes: asked again, up to what was asked
  CHECK(device.get_random(ByteView(bytes, 40)));
  // the entropy source
  TpmEntropySource entropy(device);
  tpm.short_random = false;
  CHECK(entropy.read(ByteView(bytes, 40)) && entropy.read(ByteView(bytes, 0)));
}

static void test_table() {
  Bytes t(52, 0);
  memcpy(t.data(), "TPM2", 4);
  t[4] = 52;
  t[36] = 1;
  t[40] = 0x40; t[41] = 0x00; t[42] = 0xD4; t[43] = 0xFE;                                    // control area 0xFED40040
  t[48] = 6;
  Tpm2Table table;
  CHECK(tpm2_parse_table(ConstByteView(t.data(), t.size()), &table) && table.platform_class == 1 && table.control_area == 0xFED40040ull && table.start_method == Tpm2Table::kStartCrb);
  CHECK(!tpm2_parse_table(ConstByteView(t.data(), 51), &table));
  Bytes bad = t;
  bad[0] = 'X';
  CHECK(!tpm2_parse_table(ConstByteView(bad.data(), bad.size()), &table));
  bad = t;
  bad[4] = 200;                                                                              // longer than the table
  CHECK(!tpm2_parse_table(ConstByteView(bad.data(), bad.size()), &table));
  for (size_t n = 0; n <= t.size(); ++n) tpm2_parse_table(ConstByteView(t.data(), n), &table);
  for (size_t bit = 0; bit < t.size() * 8; ++bit) { Bytes f = t; f[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8)); tpm2_parse_table(ConstByteView(f.data(), f.size()), &table); }
}

struct FakeMapper final : IRegisterMapper {
  bool map_registers(uint64_t, uint64_t, MmioRegion*) override { return false; }
};

static void test_driver() {
  TestAllocator a;
  FakeTpm tpm;
  FakeCrbRegisters crb_regs(&tpm);
  TpmDriver crb_driver(a);
  CHECK(crb_driver.start_registers(crb_regs, 0xFED40000, true) == TpmDriver::Outcome::Started && crb_driver.device());
  uint32_t manufacturer;
  CHECK(crb_driver.device()->property(tpm::kPtManufacturer, &manufacturer) && manufacturer == 0x49465800 && strcmp(crb_driver.device()->transport_name(), "TPM 2.0 (CRB)") == 0);
  FakeTpm tpm2;
  FakeTisRegisters tis_regs(&tpm2);
  TpmDriver tis_driver(a);
  CHECK(tis_driver.start_registers(tis_regs, 0xFED40000, false) == TpmDriver::Outcome::Started && tis_driver.device() && tpm2.started && strcmp(tis_driver.device()->transport_name(), "TPM 2.0 (FIFO)") == 0);
  FakeTisRegisters gone(&tpm2);
  gone.absent = true;
  TpmDriver none(a);
  CHECK(none.start_registers(gone, 0xFED40000, false) == TpmDriver::Outcome::NoTpm && !none.device());
  FakeTisRegisters silent(&tpm2);
  silent.never_answer = true;
  TpmDriver mute(a);
  CHECK(mute.start_registers(silent, 0xFED40000, false) == TpmDriver::Outcome::NoAnswer);
  // the table picks the interface: an ACPI start method is not supported; a mapping that fails is reported
  Bytes t(52, 0);
  memcpy(t.data(), "TPM2", 4);
  t[4] = 52;
  t[48] = 7;
  FakeMapper mapper;
  TpmDriver acpi_start(a);
  CHECK(acpi_start.start(ConstByteView(t.data(), t.size()), mapper) == TpmDriver::Outcome::UnsupportedStartMethod);
  t[48] = 6;
  TpmDriver mapping(a);
  CHECK(mapping.start(ConstByteView(t.data(), t.size()), mapper) == TpmDriver::Outcome::MapFailed);
  CHECK(mapping.start(ConstByteView(), mapper) == TpmDriver::Outcome::MapFailed);
  CHECK(strlen(TpmDriver::outcome_name(TpmDriver::Outcome::Started)) > 0);
}

int main() {
  test_driver();
  test_tis();
  test_crb();
  test_device_failures();
  test_table();
  printf(g_failed ? "TpmTest: %d FAILED\n" : "TpmTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
