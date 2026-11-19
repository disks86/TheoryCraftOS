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

// Follows nouveau (MIT license): nvkm/subdev/devinit/gf100.c (gf100_devinit_preinit), tu102.c (tu102_devinit_wait), nv50.c and nv04.c
// (the VGA CRTC heuristics: detect() does not do them, post() does), base.c (nvkm_devinit_preinit / _post), gm107.c (the display fuse),
// gt215.c (its register filter), engine/disp/vga.c (nvkm_lockvgac), gm200.c (the PMU devinit: pmu_code/pmu_data/pmu_args/pmu_exec,
// gm200_devinit_post/preos), bios/pmu.c (the BIT 'p' table), falcon/gm200.c and gp102.c (the falcon reset).
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDevinit.hpp"

namespace hardware {
namespace nv {

PostStatus NvDevinit::detect(INvRegisters& r, const Chip& chip) {
  PostStatus status;
  if (chip.family == Family::Unknown) {
    status.reason = "unknown chip";
    return status;
  }
  if (chip.tegra) {
    status.state = PostState::Posted;
    status.reason = "a Tegra GPU: the SoC's boot firmware brings it up";
    return status;
  }
  if (chip.card_type < 0xc0) {
    // nv50_devinit_preinit reads VGA CR00/CR1A, nv04_devinit_preinit the CRTC's horizontal total: both need 8-bit writes of the CRTC index
    // port (0x6013d4), which INvRegisters (32-bit only) cannot do without clobbering the data port next to it.
    status.reason = "not detectable yet before Fermi (needs 8-bit VGA CRTC access)";
    return status;
  }
  const uint32_t posted = r.read32(kPostedGf100);
  if (posted == 0xFFFFFFFFu) {
    status.reason = "the GPU does not answer (BAR0 reads all ones)";
    return status;
  }
  status.state = (posted & kPostedBit) ? PostState::Posted : PostState::NotPosted;
  status.reason = status.state == PostState::Posted ? "devinit has run (0x2240c bit 1)"
                                                    : "not POSTed (0x2240c bit 1 clear): no init-script interpreter yet, the GPU cannot be brought up";
  if (chip.card_type >= 0x160) {
    status.devinit_complete_known = true;
    status.devinit_complete = (r.read32(kDevinitStatusTu102) & 1) && (r.read32(kDevinitDoneTu102) & 0xff) == 0xff;
  }
  return status;
}

const char* post_outcome_name(PostOutcome outcome) {
  switch (outcome) {
    case PostOutcome::Ran: return "ran";
    case PostOutcome::DryRun: return "dry run";
    case PostOutcome::AlreadyPosted: return "already POSTed";
    case PostOutcome::Refused: return "refused";
    case PostOutcome::Failed: return "failed";
  }
  return "?";
}

namespace {

constexpr uint32_t kCrtcIndex = 0x6013d4;   // PRMCIO's CRTC index port (NV50+: one for both heads)

void trace_vga(const PostInputs& in, uint8_t index, uint8_t value) {
  if (!in.trace) return;
  NvInitWrite w;
  w.kind = NvInitAccess::VgaIndexed;
  w.address = kCrtcIndex;
  w.index = index;
  w.value = value;
  in.trace->on_write(w);
}

uint8_t read_crtc(INvVgaIo& vga, uint8_t index) {
  vga.write8(kCrtcIndex, index);
  return vga.read8(kCrtcIndex + 1);
}

// --- The PMU path (gm200_devinit.c, bios/pmu.c, falcon/gm200.c and gp102.c) -------------------------------------------------------------

constexpr uint32_t kFalconMailbox0 = 0x10a040, kFalconIrqMaskClr = 0x10a014, kFalconIrqDest = 0x10a048, kFalconCpuCtl = 0x10a100,
                   kFalconBootVec = 0x10a104, kFalconHwCfg = 0x10a108, kFalconDmaCtl = 0x10a10c, kFalconImemCtl = 0x10a180,
                   kFalconImemData = 0x10a184, kFalconImemTag = 0x10a188, kFalconDmemCtl = 0x10a1c0, kFalconDmemData = 0x10a1c4,
                   kFalconEngine = 0x10a3c0, kFalconRm = 0x10a084;
constexpr uint32_t kMailboxStart = 0x00005000, kMailboxDone = 0x00002000;
constexpr uint32_t kScrubTimeoutUs = 10000;   // gm200_flcn_reset_wait_mem_scrubbing: 10 ms

uint64_t round4(uint64_t n) { return (n + 3) & ~uint64_t{3}; }
bool in_image(const NvBios& bios, uint64_t at, uint64_t len) { return at <= bios.data().size() && round4(len) <= bios.data().size() - at; }

// The writes of the PMU path, traced like the interpreter's (NvInitAccess::Mmio) and counted.
struct Falcon {
  const PostInputs& in;
  INvRegisters& r;
  PmuPostReport& rep;
  void wr(uint32_t reg, uint32_t value) {
    r.write32(reg, value);
    if (in.trace) {
      NvInitWrite w;
      w.kind = NvInitAccess::Mmio;
      w.address = reg;
      w.value = value;
      in.trace->on_write(w);
    }
  }
  void mask(uint32_t reg, uint32_t clear, uint32_t set) { wr(reg, (r.read32(reg) & ~clear) | set); }
  // nvkm_msec: poll until (reg & m) == want, at most timeout_us.
  bool poll(uint32_t reg, uint32_t m, uint32_t want, uint32_t timeout_us, uint32_t* waited) {
    const uint32_t step = in.pmu_poll_us ? in.pmu_poll_us : 1;
    uint32_t spent = 0;
    for (;;) {
      if ((r.read32(reg) & m) == want) {
        if (waited) *waited = spent;
        return true;
      }
      if (spent >= timeout_us) break;
      r.delay_us(step);
      spent = timeout_us - spent < step ? timeout_us : spent + step;
    }
    if (waited) *waited = spent;
    return false;
  }
  // gm200_flcn_reset_wait_mem_scrubbing
  bool wait_scrub() {
    mask(kFalconMailbox0, 0, 0);
    return poll(kFalconDmaCtl, 0x00000006, 0, kScrubTimeoutUs, nullptr);
  }
  // gp102_flcn_reset_eng
  bool reset_eng() {
    mask(kFalconEngine, 1, 1);
    r.delay_us(10);
    mask(kFalconEngine, 1, 0);
    return wait_scrub();
  }
  // nvkm_falcon_reset: gm200_flcn_disable + gm200_flcn_enable, PMC_ENABLE (gm200_pmu_flcn: GM20x, GP100) or the engine reset register
  // (gp102_pmu_flcn: GP102 .. GV100).
  bool reset() {
    mask(kFalconIrqDest, 0x00000003, 0);
    wr(kFalconIrqMaskClr, 0xffffffff);
    if (rep.engine_reset) {
      if (!reset_eng() || !reset_eng()) return false;   // (disable, then enable: each pulses the engine reset)
    } else {
      mask(NvDevinit::kPmcEnable, NvDevinit::kPmcEnablePmu, 0);
      mask(NvDevinit::kPmcEnable, NvDevinit::kPmcEnablePmu, NvDevinit::kPmcEnablePmu);
      (void)r.read32(NvDevinit::kPmcEnable);
    }
    if (!wait_scrub()) return false;
    wr(kFalconRm, r.read32(0x000000));
    return true;
  }
  // pmu_code: IMEM through port 0, a tag every 256 bytes, padded to 256 bytes.
  void code(uint32_t pmu, uint32_t img, uint32_t len, bool sec) {
    wr(kFalconImemCtl, 0x01000000 | (sec ? 0x10000000 : 0) | pmu);
    uint32_t i = 0;
    for (; i < len; i += 4) {
      if ((i & 0xff) == 0) wr(kFalconImemTag, (pmu + i) >> 8);
      uint32_t word = 0;
      (void)in.bios->rd32(uint64_t{img} + i, &word);   // (in the image: find_pmu_app checked the section)
      wr(kFalconImemData, word);
      ++rep.imem_words;
    }
    while (i & 0xff) {
      wr(kFalconImemData, 0);
      ++rep.imem_words;
      i += 4;
    }
  }
  // pmu_data: DMEM through port 0 with auto-increment.
  void data(uint32_t pmu, uint32_t img, uint32_t len) {
    wr(kFalconDmemCtl, 0x01000000 | pmu);
    for (uint32_t i = 0; i < len; i += 4) {
      uint32_t word = 0;
      (void)in.bios->rd32(uint64_t{img} + i, &word);
      wr(kFalconDmemData, word);
      ++rep.dmem_words;
    }
  }
  // pmu_args: the DMEM word at (the word at argp) + argi.
  uint32_t args(uint32_t argp, uint32_t argi) {
    wr(kFalconDmemCtl, argp);
    wr(kFalconDmemCtl, r.read32(kFalconDmemData) + argi);
    return r.read32(kFalconDmemData);
  }
  // pmu_exec
  void exec(uint32_t init_addr) {
    wr(kFalconBootVec, init_addr);
    wr(kFalconDmaCtl, 0);
    wr(kFalconCpuCtl, 0x00000002);
  }
  // pmu_load's upload, with the falcon's own memory sizes (HWCFG) checked first: false if the application does not fit.
  bool fits(const NvPmuApp& app) {
    const uint32_t hwcfg = r.read32(kFalconHwCfg);
    if (hwcfg == 0 || hwcfg == 0xffffffffu) return true;   // (unknown: as nouveau, which never checks)
    const uint64_t imem = uint64_t{hwcfg & 0x1ff} << 8, dmem = uint64_t{hwcfg & 0x3fe00} >> 1;
    const uint64_t boot_end = uint64_t{app.boot_addr_pmu} + ((round4(app.boot_size) + 0xff) & ~uint64_t{0xff});
    const uint64_t code_end = uint64_t{app.code_addr_pmu} + ((round4(app.code_size) + 0xff) & ~uint64_t{0xff});
    const uint64_t data_end = uint64_t{app.data_addr_pmu} + round4(app.data_size);
    return boot_end <= imem && code_end <= imem && data_end <= dmem;
  }
  // A DMEM range the argument block chose (the tables' targets) against the falcon's DMEM size.
  bool data_fits(uint32_t pmu, uint32_t len) {
    const uint32_t hwcfg = r.read32(kFalconHwCfg);
    if (hwcfg == 0 || hwcfg == 0xffffffffu) return true;
    return uint64_t{pmu} + round4(len) <= (uint64_t{hwcfg & 0x3fe00} >> 1);
  }
  void upload(const NvPmuApp& app) {
    code(app.boot_addr_pmu, app.boot_addr, app.boot_size, false);
    code(app.code_addr_pmu, app.code_addr, app.code_size, true);
    data(app.data_addr_pmu, app.data_addr, app.data_size);
  }
};

// The DMEM word at `addr` after the application's data section is uploaded, read from the image (a dry run's pmu_args).
bool image_dmem(const NvBios& bios, const NvPmuApp& app, uint32_t addr, uint32_t* out) {
  if (addr < app.data_addr_pmu || uint64_t{addr} + 4 > uint64_t{app.data_addr_pmu} + round4(app.data_size)) return false;
  return bios.rd32(uint64_t{app.data_addr} + (addr - app.data_addr_pmu), out);
}
bool image_args(const NvBios& bios, const NvPmuApp& app, uint32_t argp, uint32_t argi, uint32_t* out) {
  uint32_t base = 0;
  return image_dmem(bios, app, argp, &base) && image_dmem(bios, app, base + argi, out);
}

// gm200_devinit_post (post = true). The checks were made by the caller. False on a failure (report.reason says which).
bool pmu_post(const PostInputs& in, PostReport& report) {
  PmuPostReport& rep = report.pmu;
  const NvBios& bios = *in.bios;
  INvRegisters& r = *in.registers;
  Falcon f{in, r, rep};
  const NvPmuApp& app = rep.devinit;

  if (in.dry_run) {
    // What would be written, without touching the falcon: the DMEM targets of the two tables come from the image's data section.
    uint32_t a = 0, b = 0;
    rep.dmem_targets_known = image_args(bios, app, app.args_addr_pmu + 0x08, 0x08, &a) &&
                             image_args(bios, app, app.args_addr_pmu + 0x08, 0x10, &b);
    if (rep.dmem_targets_known) {
      rep.init_table_dmem = a;
      rep.boot_scripts_dmem = b;
    }
    auto padded = [](uint32_t n) { return static_cast<uint32_t>(((round4(n) + 0xff) & ~uint64_t{0xff}) / 4); };
    rep.imem_words = padded(app.boot_size) + padded(app.code_size);
    rep.dmem_words = static_cast<uint32_t>((round4(app.data_size) + round4(rep.init_table_length) + round4(rep.boot_scripts_length)) / 4);
    NvPmuApp preos;
    rep.preos_status = find_pmu_app(bios, kPmuAppPreOs, &preos);
    return true;
  }

  if (!f.reset()) {
    report.reason = "the PMU falcon did not come out of reset (memory scrubbing did not finish)";
    return false;
  }
  if (!f.fits(app)) {
    report.reason = "the VBIOS's DEVINIT application does not fit the PMU falcon's memories";
    return false;
  }
  f.upload(app);
  // The tables the boot scripts' opcodes use, then the boot scripts, where the argument block says.
  // (Each target is checked against the DMEM size: the argument block is the image's data.)
  rep.init_table_dmem = f.args(app.args_addr_pmu + 0x08, 0x08);
  rep.dmem_targets_known = true;
  if (!f.data_fits(rep.init_table_dmem, rep.init_table_length)) {
    report.reason = "the DEVINIT application's argument block puts the init tables outside the PMU's DMEM";
    return false;
  }
  f.data(rep.init_table_dmem, rep.init_table, rep.init_table_length);
  rep.boot_scripts_dmem = f.args(app.args_addr_pmu + 0x08, 0x10);
  if (!f.data_fits(rep.boot_scripts_dmem, rep.boot_scripts_length)) {
    report.reason = "the DEVINIT application's argument block puts the boot scripts outside the PMU's DMEM";
    return false;
  }
  f.data(rep.boot_scripts_dmem, rep.boot_scripts, rep.boot_scripts_length);
  // Execute DEVINIT.
  f.wr(kFalconMailbox0, kMailboxStart);
  f.exec(app.init_addr_pmu);
  rep.started = true;
  rep.completed = f.poll(kFalconMailbox0, kMailboxDone, kMailboxDone, in.pmu_timeout_us, &rep.waited_us);
  if (!rep.completed) {
    report.reason = "the VBIOS's DEVINIT application did not finish on the PMU (timeout)";
    return false;
  }
  // gm200_devinit_preos: the optional PRE_OS application (fan control until a full PMU firmware is loaded); its failures are ignored.
  NvPmuApp preos;
  rep.preos_status = find_pmu_app(bios, kPmuAppPreOs, &preos);
  if (in.run_preos && rep.preos_status == NvPmuAppStatus::Found && f.reset() && f.fits(preos)) {
    f.upload(preos);
    f.exec(preos.init_addr_pmu);
    rep.preos_started = true;
  }
  return true;
}

}  // namespace

const char* pmu_app_status_name(NvPmuAppStatus status) {
  switch (status) {
    case NvPmuAppStatus::Found: return "found";
    case NvPmuAppStatus::NoTable: return "no BIT 'p' table";
    case NvPmuAppStatus::NotFound: return "no such application";
    case NvPmuAppStatus::Truncated: return "the falcon ucode table runs out of the image";
    case NvPmuAppStatus::BadSections: return "an application section lies outside the image";
  }
  return "?";
}

NvPmuAppStatus find_pmu_app(const NvBios& bios, uint8_t type, NvPmuApp* out) {
  // nvbios_pmuTe
  BitEntry p;
  if (!bios.bit_entry('p', &p) || p.version != 2 || p.length < 4) return NvPmuAppStatus::NoTable;
  uint32_t table = 0;
  if (!bios.rd32(p.offset, &table)) return NvPmuAppStatus::Truncated;
  if (!table) return NvPmuAppStatus::NoTable;
  uint8_t hdr = 0, len = 0, cnt = 0;
  if (!bios.rd08(uint64_t{table} + 1, &hdr) || !bios.rd08(uint64_t{table} + 2, &len) || !bios.rd08(uint64_t{table} + 3, &cnt))
    return NvPmuAppStatus::Truncated;
  // nvbios_pmuEp / nvbios_pmuRm
  for (uint32_t idx = 0; idx < cnt; ++idx) {
    const uint64_t entry = uint64_t{table} + hdr + uint64_t{idx} * len;
    uint8_t etype = 0;
    uint32_t data = 0;
    if (!bios.rd08(entry, &etype) || !bios.rd32(entry + 2, &data)) return NvPmuAppStatus::Truncated;
    if (etype != type || !data) continue;
    uint32_t f[10] = {};   // the descriptor's words at +0x08 .. +0x2c
    for (uint32_t i = 0; i < 10; ++i)
      if (!bios.rd32(uint64_t{data} + 0x08 + i * 4, &f[i])) return NvPmuAppStatus::Truncated;
    const uint32_t init = f[0], args = f[1], boot_base = f[2] /* +0x10 */, boot_start = f[4] /* +0x18 */, boot_end = f[5] /* +0x1c */,
                   code_size = f[6] /* +0x20 */, data_off = f[7] /* +0x24 */, data_pmu = f[8] /* +0x28 */, data_size = f[9] /* +0x2c */;
    NvPmuApp app;
    app.descriptor = data;
    app.init_addr_pmu = init;
    app.args_addr_pmu = args;
    app.boot_addr = data + 0x30;
    app.boot_addr_pmu = boot_base + boot_start;
    app.boot_size = boot_end - boot_start;
    app.code_addr = app.boot_addr + app.boot_size;
    app.code_addr_pmu = app.boot_addr_pmu + app.boot_size;
    app.code_size = code_size;
    app.data_addr = data + 0x30 + data_off;
    app.data_addr_pmu = data_pmu;
    app.data_size = data_size;
    // The ranges nouveau's upload reads (it never checks them): inside the image, without wrapping.
    const uint64_t boot = uint64_t{data} + 0x30, code = boot + app.boot_size, dat = boot + data_off;
    if (boot_end < boot_start || !in_image(bios, boot, app.boot_size) || !in_image(bios, code, code_size) ||
        !in_image(bios, dat, data_size) || code > UINT32_MAX || dat > UINT32_MAX ||
        uint64_t{boot_base} + boot_start > UINT32_MAX || uint64_t{app.boot_addr_pmu} + app.boot_size > UINT32_MAX)
      return NvPmuAppStatus::BadSections;
    *out = app;
    return NvPmuAppStatus::Found;
  }
  return NvPmuAppStatus::NotFound;
}

namespace {

// tu102_devinit_wait (nouveau) / wait_gfw_boot_completion (nova-core gfw.rs): nothing is written, FWSEC does the work.
PostReport gfw_boot_wait(const PostInputs& in, PostReport& report) {
  INvRegisters& r = *in.registers;
  GfwBootReport& g = report.gfw;
  g.used = true;
  const uint32_t step = in.gfw_poll_us ? in.gfw_poll_us : 1;
  for (;;) {
    ++g.polls;
    const uint32_t level = r.read32(NvDevinit::kDevinitStatusTu102);
    if (level == 0xFFFFFFFFu) {
      report.outcome = PostOutcome::Failed;
      report.reason = "the GPU does not answer (0x118128 reads all ones)";
      return report;
    }
    g.protection_lowered = (level & NvDevinit::kGfwReadLevel0) != 0;
    if (g.protection_lowered) {
      // (read only once the protection is lowered: before that the scratch group reads as a priv error, not the progress)
      g.progress = r.read32(NvDevinit::kDevinitDoneTu102) & NvDevinit::kGfwProgressMask;
      g.completed = g.progress == NvDevinit::kGfwProgressDone;
    }
    if (g.completed) {
      report.needed = g.polls > 1;
      report.outcome = g.polls == 1 ? PostOutcome::AlreadyPosted : PostOutcome::Ran;
      report.reason = g.polls == 1 ? "Turing+: the GPU's firmware had already finished its boot (GFW_BOOT 0xff)"
                                   : "Turing+: the GPU's firmware finished its boot while we waited (GFW_BOOT 0xff)";
      report.posted_after_known = true;
      report.posted_after = true;
      return report;
    }
    report.needed = true;
    if (in.dry_run) {
      report.outcome = PostOutcome::DryRun;
      report.reason = g.protection_lowered ? "Turing+: the GPU's firmware has not finished its boot (dry run: read once, no wait)"
                                           : "Turing+: GFW_BOOT is still read-protected (dry run: read once, no wait)";
      return report;
    }
    if (g.waited_us >= in.gfw_timeout_us) break;
    r.delay_us(step);
    g.waited_us = in.gfw_timeout_us - g.waited_us < step ? in.gfw_timeout_us : g.waited_us + step;
  }
  report.outcome = PostOutcome::Failed;
  report.reason = g.protection_lowered ? "Turing+: the GPU's firmware did not finish its boot in time (GFW_BOOT progress not 0xff)"
                                       : "Turing+: FWSEC never lowered GFW_BOOT's read protection (0x118128 bit 0 stayed clear)";
  return report;
}

}  // namespace

PostReport NvDevinit::post(const PostInputs& in) {
  PostReport report;
  if (!in.bios || !in.registers) {
    report.reason = "no video BIOS image or no registers";
    return report;
  }
  const Chip& chip = in.chip;
  INvRegisters& r = *in.registers;
  report.before = detect(r, chip);
  if (chip.family == Family::Unknown) {
    report.reason = "unknown chip";
    return report;
  }
  if (chip.tegra) {
    report.outcome = PostOutcome::AlreadyPosted;
    report.reason = "a Tegra GPU: the SoC's boot firmware brings it up";
    return report;
  }
  if (chip.card_type < 0x50) {
    report.reason = "pre-Tesla devinit (VGA owner selection, nv04/nv10/nv20 memory init) is not implemented";
    return report;
  }
  if (chip.card_type >= 0x160) return gfw_boot_wait(in, report);
  if (report.before.state == PostState::Unknown && chip.card_type >= 0xc0) {
    // (0x2240c read all ones: the GPU is not answering)
    report.reason = report.before.reason;
    return report;
  }
  // GM20x to Volta: gm200_devinit_post's checks, made before anything is written. (nouveau makes them even when no POST is needed and
  // fails the device; here a GPU whose 0x2240c says POSTed is reported as such.)
  const bool pmu_path = chip.chipset >= 0x120;
  if (pmu_path) {
    PmuPostReport& pmu = report.pmu;
    pmu.used = true;
    pmu.engine_reset = chip.chipset >= 0x132;   // gp102_pmu (GP102 .. GV100); gm200_pmu (GM20x, GP100) resets through PMC_ENABLE
    const char* problem = nullptr;
    BitEntry bit_i;
    uint16_t t = 0, tl = 0, b = 0, bl = 0;
    if (!in.bios->bit_entry('I', &bit_i) || bit_i.version != 1 || bit_i.length < 0x1c) {
      problem = "VBIOS PMU init data not found (BIT 'I' version 1, 0x1c bytes)";
    } else if (!in.bios->rd16(uint64_t{bit_i.offset} + 0x14, &t) || !in.bios->rd16(uint64_t{bit_i.offset} + 0x16, &tl) ||
               !in.bios->rd16(uint64_t{bit_i.offset} + 0x18, &b) || !in.bios->rd16(uint64_t{bit_i.offset} + 0x1a, &bl)) {
      problem = "the BIT 'I' table runs out of the image";
    } else if (!in_image(*in.bios, t, tl) || !in_image(*in.bios, b, bl)) {
      problem = "the init tables or boot scripts for the PMU lie outside the image";
    } else {
      pmu.init_table = t;
      pmu.init_table_length = tl;
      pmu.boot_scripts = b;
      pmu.boot_scripts_length = bl;
      pmu.devinit_status = find_pmu_app(*in.bios, kPmuAppDevinit, &pmu.devinit);
      if (pmu.devinit_status != NvPmuAppStatus::Found) problem = "VBIOS PMU/DEVINIT application not usable (BIT 'p' type 0x04)";
    }
    if (problem) {
      if (report.before.state == PostState::Posted && !in.force) {
        report.outcome = PostOutcome::AlreadyPosted;
        report.reason = "the GPU was POSTed (and the image has no usable PMU devinit)";
      } else {
        report.reason = problem;
      }
      return report;
    }
  }

  // nvkm_devinit_preinit: unlock the extended VGA CRTC registers (nvkm_lockvgac(false); NV50+ use CR3f).
  if (in.vga) {
    trace_vga(in, 0x3f, 0x57);
    if (!in.dry_run) in.vga->write_indexed(kCrtcIndex, 0x3f, 0x57);
  }

  // The preinit decision.
  bool needed = false;
  if (chip.card_type >= 0xc0) {
    // gf100_devinit_preinit: 0x2240c bit 1 clear means not POSTed, unless the CRTC says the VBIOS did run (some miss the bit). Only with a
    // display engine (gf100: 0x022500 bit 0, gm107: 0x021c04 bit 0 disables it).
    needed = report.before.state == PostState::NotPosted;
    if (needed && in.vga && !in.dry_run) {
      const uint32_t fuse = r.read32(chip.chipset >= 0x110 ? 0x021c04 : 0x022500);
      if (!(fuse & 1) && (read_crtc(*in.vga, 0x00) || read_crtc(*in.vga, 0x1a))) needed = false;
    }
  } else {
    // nv50_devinit_preinit: without a display engine, assume a secondary GPU that needs it; otherwise CR00 and CR1A both 0 mean the x86
    // VBIOS never ran.
    if (!chip.has_display) {
      needed = true;
    } else if (in.vga && !in.dry_run) {
      needed = !read_crtc(*in.vga, 0x00) && !read_crtc(*in.vga, 0x1a);
    } else if (!in.force && !in.dry_run) {
      report.reason = "cannot tell whether a Tesla GPU was POSTed without byte access to the VGA registers";
      return report;
    } else {
      needed = true;
    }
  }
  report.needed = needed;
  if (!needed && !in.force) {
    report.outcome = PostOutcome::AlreadyPosted;
    report.reason = "the GPU was POSTed";
    return report;
  }

  if (pmu_path) {
    if (!pmu_post(in, report)) {
      report.outcome = PostOutcome::Failed;
      return report;
    }
    report.outcome = in.dry_run ? PostOutcome::DryRun : PostOutcome::Ran;
    report.reason = in.dry_run ? "the PMU devinit upload was planned, nothing was written"
                               : "the VBIOS's DEVINIT application ran on the PMU";
    report.encoder_scripts_pending = true;
    if (!in.dry_run) {
      const uint32_t posted = r.read32(kPostedGf100);
      report.posted_after_known = posted != 0xFFFFFFFFu;
      report.posted_after = report.posted_after_known && (posted & kPostedBit);
    }
    return report;
  }

  NvInitOptions options;
  options.dry_run = in.dry_run;
  options.step_budget = in.step_budget;
  options.board = in.board;
  options.gt215_partition_filter = chip.chipset == 0xa3 || chip.chipset == 0xa5 || chip.chipset == 0xa8;
  NvInitScript script(*in.bios, chip, r, in.vga, in.hooks, in.trace, options);
  const bool ok = script.post();
  report.script = script.result();
  if (!ok) {
    report.outcome = PostOutcome::Failed;
    report.reason = report.script.message;
    return report;
  }
  report.outcome = in.dry_run ? PostOutcome::DryRun : PostOutcome::Ran;
  report.reason = in.dry_run ? "the init scripts were walked, nothing was written" : "the init scripts ran";
  report.encoder_scripts_pending = true;
  if (chip.card_type >= 0xc0 && !in.dry_run) {
    const uint32_t posted = r.read32(kPostedGf100);
    report.posted_after_known = posted != 0xFFFFFFFFu;
    report.posted_after = report.posted_after_known && (posted & kPostedBit);
  }
  return report;
}

const char* NvDevinit::state_name(PostState state) {
  switch (state) {
    case PostState::Unknown: return "unknown";
    case PostState::Posted: return "POSTed";
    case PostState::NotPosted: return "not POSTed";
  }
  return "unknown";
}

}  // namespace nv
}  // namespace hardware
