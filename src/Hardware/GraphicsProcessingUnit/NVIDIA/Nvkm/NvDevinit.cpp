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
// gt215.c (its register filter), engine/disp/vga.c (nvkm_lockvgac).
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
  if (chip.card_type >= 0x160) {
    report.reason = "Turing and later: devinit is NVIDIA's signed FWSEC firmware (tu102_devinit_post), not done here";
    return report;
  }
  if (chip.chipset >= 0x120) {
    report.reason = "GM20x to Volta: devinit is the VBIOS's DEVINIT application on the PMU falcon (gm200_devinit_post), not done here";
    return report;
  }
  if (report.before.state == PostState::Unknown && chip.card_type >= 0xc0) {
    // (0x2240c read all ones: the GPU is not answering)
    report.reason = report.before.reason;
    return report;
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
