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


// Host-side tests for the SMC mailbox (AmdSmu7) against a simulated SMC: the three registers of the interface with the behaviour the driver relies on (a result word
// of zero while a message is being handled, 1 for done, 0xFE for an unknown command), a record of every write in order, and the failure modes - an SMC that is not running
// (the result register stuck at zero), registers that read all ones, one that answers late. The register numbers are typed in from AMD's smu_7_1_3_d.h, independently of
// the ones the code uses.
#include <stdio.h>
#include <map>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdSmu7.hpp"
#include "FakeAmdVcn.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

constexpr uint32_t kMessage = 0x94, kResp = 0x95, kArg = 0xa4;

struct FakeSmc final : IVcnRegisters {
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  std::vector<std::pair<uint32_t, uint32_t>> handled;   // (message, argument register at the time)
  bool running = true, dead = false;
  uint32_t result = 1;                                   // what a handled message answers
  uint32_t answer_after_reads = 0;                       // reads of the result register that see zero before the answer shows
  uint32_t pending_reads = 0;
  uint32_t reply_argument = 0;
  std::map<uint32_t, uint32_t> sram;                     // the SMC's memory, by dword address (big-endian view, as the data register shows it)
  std::vector<std::pair<uint32_t, uint32_t>> sram_writes;
  bool busy_first = false;                               // the result register starts at zero and turns to 1 after a few reads (a previous message in flight)

  uint32_t read(uint32_t r) override {
    if (dead) return 0xFFFFFFFFu;
    if (r == kResp && pending_reads > 0) {
      if (--pending_reads == 0) regs[kResp] = result;
    }
    if (r == 0x1AD) { auto at = sram.find(regs[0x1AC]); return at == sram.end() ? 0 : at->second; }
    auto it = regs.find(r);
    return it == regs.end() ? 0 : it->second;
  }
  void write(uint32_t r, uint32_t v) override {
    writes.push_back({r, v});
    if (r == 0x1AD) { sram[regs[0x1AC]] = v; sram_writes.push_back({regs[0x1AC], v}); return; }
    regs[r] = v;
    if (r == kMessage && running) {
      handled.push_back({v, regs[kArg]});
      regs[kArg] = reply_argument;
      regs[kResp] = 0;
      if (answer_after_reads) pending_reads = answer_after_reads;
      else regs[kResp] = result;
    }
  }
};

static void test_power_messages() {
  FakeSmc smc;
  smc.regs[kResp] = 1;
  fakevcn::FakeClock clock;
  AmdSmu7 smu(smc, clock);
  CHECK(smu.uvd_power(true) == AmdSmu7::Result::Ok);
  // PPSMC_MSG_UVDPowerON = 0x61 with the argument 0 written first, then the result cleared, then the message (the driver's order).
  CHECK(smc.handled.size() == 1 && smc.handled[0] == std::make_pair(0x61u, 0u));
  CHECK(smc.writes.size() == 3 && smc.writes[0] == std::make_pair(kArg, 0u) && smc.writes[1] == std::make_pair(kResp, 0u) && smc.writes[2] == std::make_pair(kMessage, 0x61u));
  CHECK(smu.uvd_power(true, true) == AmdSmu7::Result::Ok && smc.handled[1] == std::make_pair(0x61u, 1u));   // dynamic power gating: argument 1
  // Power off is a plain message: the argument register is not written.
  smc.writes.clear();
  CHECK(smu.uvd_power(false) == AmdSmu7::Result::Ok && smc.handled[2].first == 0x60);
  CHECK(smc.writes.size() == 2 && smc.writes[0] == std::make_pair(kResp, 0u) && smc.writes[1] == std::make_pair(kMessage, 0x60u));
}

static void test_vce_power() {
  FakeSmc smc;
  smc.regs[kResp] = 1;
  fakevcn::FakeClock clock;
  AmdSmu7 smu(smc, clock);
  // PPSMC_MSG_VCEPowerON / OFF (0x10f / 0x10e in smu7_ppsmc.h), plain messages: the argument register is not written.
  CHECK(smu.vce_power(true) == AmdSmu7::Result::Ok && smu.vce_power(false) == AmdSmu7::Result::Ok);
  CHECK(smc.handled.size() == 2 && smc.handled[0].first == 0x10f && smc.handled[1].first == 0x10e);
  CHECK(smc.writes.size() == 4 && smc.writes[0] == std::make_pair(kResp, 0u) && smc.writes[1] == std::make_pair(kMessage, 0x10fu));
}

// The Vega M's SMC memory as the DPM code finds it: the header at 0x20000 names the DPM table (at 0x30000 here), whose VceLevelCount (byte 213) and VceBootLevel (byte 1649)
// sit in the dwords at 0x300D4 and 0x30670 - the second byte of each (bits 23:16: the SMC is big-endian). The bytes around them are other fields.
constexpr uint32_t kTable = 0x30000, kCountWord = kTable + 212, kBootWord = kTable + 1648;
static void put_table(FakeSmc& smc, uint32_t levels) {
  smc.regs[kResp] = 1;
  smc.sram[0x20000 + 52] = kTable;
  smc.sram[kCountWord] = 0x11000033u | (levels << 16) | 0x00002200u;
  smc.sram[kBootWord] = 0xAA00CCDDu | 0x00220000u;   // UvdBootLevel 0xAA, VceBootLevel 0x22, AcpBootLevel (bits 15:8) 0, SamuBootLevel 0xDD
}

static void test_sram() {
  fakevcn::FakeClock clock;
  FakeSmc smc;
  smc.regs[0x92] = 0xFFFFFFFFu;
  smc.sram[0x100] = 0xCAFEF00Du;
  AmdSmu7 smu(smc, clock);
  uint32_t v = 0;
  CHECK(smu.read_sram_dword(0x100, &v) && v == 0xCAFEF00Du);
  // The index register gets the address, and the auto-increment bit of SMC_IND_ACCESS_CNTL is cleared (only that bit).
  CHECK(smc.regs[0x1AC] == 0x100 && smc.regs[0x92] == 0xFFFFF7FFu);
  CHECK(smu.write_sram_dword(0x104, 0x12345678u) && smc.sram[0x104] == 0x12345678u);
  // Unaligned and out-of-range addresses are refused without touching the card.
  smc.writes.clear();
  CHECK(!smu.read_sram_dword(0x101, &v) && !smu.read_sram_dword(0x40000, &v) && !smu.write_sram_dword(0x3FFFE, 0) && !smu.write_sram_dword(0x40000, 0) && smc.writes.empty());
  CHECK(smu.read_sram_dword(0x3FFFC, &v));
}

static void test_vce_dpm() {
  fakevcn::FakeClock clock;
  {  // The top level: the boot level byte is rewritten (and nothing else in its dwords), the mask names that level only, then DPM is enabled.
    FakeSmc smc;
    put_table(smc, 4);
    AmdSmu7 smu(smc, clock);
    uint32_t level = 99;
    CHECK(smu.vce_dpm_enable(AmdSmu7::VceLevel::Highest, &level) == AmdSmu7::DpmResult::Ok && level == 3);
    CHECK(smc.sram_writes.size() == 1 && smc.sram_writes[0] == std::make_pair(kBootWord, 0xAA03CCDDu));
    CHECK(smc.handled.size() == 2 && smc.handled[0] == std::make_pair(0x12Eu, 8u) && smc.handled[1].first == 0x15a);
    CHECK(smc.sram[kCountWord] == (0x11000033u | (4u << 16) | 0x00002200u));   // the level count dword is as it was
    CHECK(smu.vce_dpm_disable() == AmdSmu7::Result::Ok && smc.handled.size() == 3 && smc.handled[2].first == 0x15b);
  }
  {  // The lowest level: boot level 0, no mask, DPM enabled.
    FakeSmc smc;
    put_table(smc, 3);
    AmdSmu7 smu(smc, clock);
    uint32_t level = 99;
    CHECK(smu.vce_dpm_enable(AmdSmu7::VceLevel::Lowest, &level) == AmdSmu7::DpmResult::Ok && level == 0);
    CHECK(smc.sram_writes.size() == 1 && smc.sram_writes[0] == std::make_pair(kBootWord, 0xAA00CCDDu));
    CHECK(smc.handled.size() == 1 && smc.handled[0].first == 0x15a);
  }
  {  // A level count that is not one: refused before anything is written.
    for (uint32_t levels : {0u, 9u, 255u}) {
      FakeSmc smc;
      put_table(smc, levels);
      AmdSmu7 smu(smc, clock);
      CHECK(smu.vce_dpm_enable(AmdSmu7::VceLevel::Highest) == AmdSmu7::DpmResult::BadDpmTable && smc.sram_writes.empty() && smc.handled.empty());
    }
  }
  {  // No DPM table: the header says zero, all ones, an unaligned address, or a table at the end of the memory.
    for (uint32_t header : {0u, 0xFFFFFFFFu, 0x30002u}) {
      FakeSmc smc;
      put_table(smc, 4);
      smc.sram[0x20000 + 52] = header;
      AmdSmu7 smu(smc, clock);
      CHECK(smu.vce_dpm_enable(AmdSmu7::VceLevel::Highest) == AmdSmu7::DpmResult::NoDpmTable && smc.sram_writes.empty() && smc.handled.empty());
    }
    FakeSmc smc;
    put_table(smc, 4);
    smc.sram[0x20000 + 52] = 0x3FF00;
    AmdSmu7 smu(smc, clock);
    CHECK(smu.vce_dpm_enable(AmdSmu7::VceLevel::Highest) == AmdSmu7::DpmResult::BadDpmTable && smc.sram_writes.empty());
  }
  {  // The mailbox: not answering, unknown message, failure.
    FakeSmc smc;
    put_table(smc, 4);
    smc.running = false;
    AmdSmu7 smu(smc, clock);
    CHECK(smu.vce_dpm_enable(AmdSmu7::VceLevel::Highest) == AmdSmu7::DpmResult::NotAnswering);
    FakeSmc unknown;
    put_table(unknown, 4);
    unknown.result = 0xFE;
    AmdSmu7 smu2(unknown, clock);
    CHECK(smu2.vce_dpm_enable(AmdSmu7::VceLevel::Lowest) == AmdSmu7::DpmResult::Unsupported);
    FakeSmc failing;
    put_table(failing, 4);
    failing.result = 0xFF;
    AmdSmu7 smu3(failing, clock);
    CHECK(smu3.vce_dpm_enable(AmdSmu7::VceLevel::Highest) == AmdSmu7::DpmResult::Failed && failing.handled.size() == 1);   // (the mask failed: DPM is not enabled)
  }
}

static void test_results() {
  fakevcn::FakeClock clock;
  {
    FakeSmc smc;
    smc.regs[kResp] = 1;
    smc.result = 0xFE;
    AmdSmu7 smu(smc, clock);
    CHECK(smu.uvd_power(true) == AmdSmu7::Result::Unsupported);
  }
  {
    FakeSmc smc;
    smc.regs[kResp] = 1;
    smc.result = 0xFF;
    AmdSmu7 smu(smc, clock);
    CHECK(smu.send(0x100) == AmdSmu7::Result::Failed);
  }
  {
    // The result word is the low 16 bits; the high ones are not part of it.
    FakeSmc smc;
    smc.regs[kResp] = 1;
    smc.result = 0xABCD0001u;
    AmdSmu7 smu(smc, clock);
    CHECK(smu.send(0x100) == AmdSmu7::Result::Ok);
  }
  {
    // The argument register after the message is handed back.
    FakeSmc smc;
    smc.regs[kResp] = 1;
    smc.reply_argument = 0x1234;
    AmdSmu7 smu(smc, clock);
    uint32_t reply = 0;
    CHECK(smu.send_with_parameter(0x129, 7, &reply) == AmdSmu7::Result::Ok && reply == 0x1234 && smc.handled[0] == std::make_pair(0x129u, 7u));
  }
}

static void test_not_running() {
  fakevcn::FakeClock clock;
  {
    // The SMC is not running: the result register stays zero. Nothing is written to it.
    FakeSmc smc;
    AmdSmu7 smu(smc, clock);
    CHECK(smu.uvd_power(true) == AmdSmu7::Result::NotAnswering && smc.writes.empty());
  }
  {
    // Registers that read all ones (no card, a closed BAR).
    FakeSmc smc;
    smc.dead = true;
    AmdSmu7 smu(smc, clock);
    CHECK(smu.uvd_power(true) == AmdSmu7::Result::NotAnswering && smc.writes.empty());
  }
  {
    // The message is written but never handled (the SMC stopped between the check and the message).
    FakeSmc smc;
    smc.regs[kResp] = 1;
    smc.running = false;
    AmdSmu7 smu(smc, clock);
    CHECK(smu.uvd_power(true) == AmdSmu7::Result::NotAnswering && smc.writes.size() == 3);
  }
  {
    // A slow answer within the time allowed, then one beyond it.
    FakeSmc smc;
    smc.regs[kResp] = 1;
    smc.answer_after_reads = 50;
    AmdSmu7 smu(smc, clock);
    CHECK(smu.send(0x100) == AmdSmu7::Result::Ok);
    smc.answer_after_reads = 1000000;
    CHECK(smu.send(0x100, nullptr, 2000) == AmdSmu7::Result::NotAnswering);
  }
  {
    // A previous message still in flight: wait for it, then go on.
    FakeSmc smc;
    smc.regs[kResp] = 0;
    smc.pending_reads = 5;
    AmdSmu7 smu(smc, clock);
    CHECK(smu.uvd_power(true) == AmdSmu7::Result::Ok && smc.handled.size() == 1);
  }
}

int main() {
  test_power_messages();
  test_vce_power();
  test_sram();
  test_vce_dpm();
  test_results();
  test_not_running();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("amd smu7 tests passed\n");
  return 0;
}
