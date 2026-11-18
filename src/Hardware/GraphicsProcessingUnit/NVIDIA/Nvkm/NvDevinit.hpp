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

// Was the GPU POSTed (did the video BIOS's x86 code, or the GPU's own firmware, run the devinit scripts that bring up its memory and
// clocks)? nouveau's devinit preinit checks (detect). And, for a GPU that was not (a secondary card, or the discrete GPU of a hybrid laptop
// after a D3cold power cycle), running the video BIOS's init scripts as nouveau's devinit post does (post, over NvInitScript).
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBios.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvInitScript.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"

namespace hardware {
namespace nv {

enum class PostState : uint8_t { Unknown, Posted, NotPosted };

struct PostStatus {
  PostState state = PostState::Unknown;
  const char* reason = "";
  // Turing and later: the FWSEC/devinit completion flags (as tu102_devinit_wait polls them), read once.
  bool devinit_complete_known = false;
  bool devinit_complete = false;
};

enum class PostOutcome : uint8_t {
  Ran,             // the scripts ran to the end
  DryRun,          // the scripts were walked, nothing written (PostInputs::dry_run)
  AlreadyPosted,   // nothing to do (and not forced)
  Refused,         // this generation is not brought up this way here (reason), or an input is missing; nothing was written
  Failed,          // a script stopped with an error (PostReport::script says where); some writes may have been made
};
const char* post_outcome_name(PostOutcome outcome);

struct PostInputs {
  const NvBios* bios = nullptr;          // the image (NvBiosShadow::fetch)
  INvRegisters* registers = nullptr;     // BAR0
  INvVgaIo* vga = nullptr;               // byte access to BAR0's VGA mirrors (MmioNvVgaIo on hardware); needed before Fermi (detection)
  Chip chip;
  const BoardInfo* board = nullptr;      // parse_board's tables (INIT_GPIO_NE), may be nullptr
  INvInitHooks* hooks = nullptr;         // PLL / I2C / AUX / GPIO; nullptr: none (PLLs are then not programmed, see NvInitResult)
  INvInitTrace* trace = nullptr;         // every write, in order
  bool dry_run = false;                  // walk the scripts and report what would be written; nothing is
  bool force = false;                    // run even when the GPU looks POSTed (nouveau's NvForcePost)
  uint32_t step_budget = NvInitOptions::kDefaultStepBudget;
};

struct PostReport {
  PostOutcome outcome = PostOutcome::Refused;
  const char* reason = "";
  PostStatus before;                     // detect() before anything was done
  bool needed = false;                   // nouveau's preinit decided the GPU needs a POST (the VGA heuristics included)
  NvInitResult script;                   // the interpreter's result (Ran / DryRun / Failed)
  bool posted_after_known = false;       // Fermi+: 0x2240c bit 1 read again afterwards
  bool posted_after = false;
  // nv50_devinit_init then runs the first script of every DCB output's display encoder table (nvbios_outp_match): not done here, the
  // display code has to (it knows the outputs).
  bool encoder_scripts_pending = false;
};

class NvDevinit {
 public:
  // gf100_devinit_preinit: bit 1 of 0x2240c is set by devinit and goes back to 0 on suspend.
  static constexpr uint32_t kPostedGf100 = 0x02240c, kPostedBit = 0x2;
  // tu102_devinit_wait: 0x118128 bit 0 and 0x118234 bits 7:0 == 0xff once devinit has finished.
  static constexpr uint32_t kDevinitStatusTu102 = 0x118128, kDevinitDoneTu102 = 0x118234;

  [[nodiscard]] static PostStatus detect(INvRegisters& registers, const Chip& chip);
  // nvkm_devinit_preinit + nvkm_devinit_post for Tesla (G80) to Maxwell GM10x: unlock the extended VGA CRTC registers, decide (as
  // nv50/gf100_devinit_preinit do) whether a POST is needed, and if so (or forced) run the init scripts in nvbios_post's order (the
  // script table's entries, then the script at BIT 'I' + 0x0e). Refused: pre-Tesla (VGA owner and memory init not done), GM20x to Volta
  // (gm200_devinit_post runs the VBIOS's DEVINIT application on the PMU falcon instead: not done), Turing and later (FWSEC, under GSP
  // control), Tegra (the SoC's firmware).
  [[nodiscard]] static PostReport post(const PostInputs& inputs);
  static const char* state_name(PostState state);
};

}  // namespace nv
}  // namespace hardware
