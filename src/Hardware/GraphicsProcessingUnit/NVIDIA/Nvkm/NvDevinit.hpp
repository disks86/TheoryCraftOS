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
// after a D3cold power cycle), running the video BIOS's init scripts as nouveau's devinit post does (post, over NvInitScript; from GM20x on,
// the VBIOS's devinit application on the PMU falcon instead).
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

// --- GM20x to Volta: the VBIOS's devinit application on the PMU falcon (gm200_devinit_post) ------------------------------------------------

// The application types of the BIT 'p' falcon ucode table (nvbios_pmuE.type) that devinit uses.
constexpr uint8_t kPmuAppPreOs = 0x01, kPmuAppDevinit = 0x04;

// nvbios_pmuR: one application of the BIT 'p' table, with the image ranges checked (boot, code and data sections inside the image,
// rounded up to whole words, as the upload reads them).
struct NvPmuApp {
  uint32_t descriptor = 0;          // the image offset of the application's descriptor (nvbios_pmuE.data)
  uint32_t init_addr_pmu = 0;       // the falcon's boot vector
  uint32_t args_addr_pmu = 0;       // DMEM address of the argument block
  uint32_t boot_addr = 0, boot_addr_pmu = 0, boot_size = 0;   // image offset, IMEM address, bytes (non-secure)
  uint32_t code_addr = 0, code_addr_pmu = 0, code_size = 0;   // (secure)
  uint32_t data_addr = 0, data_addr_pmu = 0, data_size = 0;   // image offset, DMEM address, bytes
};

enum class NvPmuAppStatus : uint8_t {
  Found,
  NoTable,       // no BIT 'p' version 2 entry, or its table pointer is 0
  NotFound,      // the table has no application of this type
  Truncated,     // the table, an entry or a descriptor runs out of the image
  BadSections,   // a section lies outside the image (or its size wraps)
};
const char* pmu_app_status_name(NvPmuAppStatus status);

// nvbios_pmuTe / nvbios_pmuEp / nvbios_pmuRm: the first application of `type` with a non-zero descriptor.
[[nodiscard]] NvPmuAppStatus find_pmu_app(const NvBios& bios, uint8_t type, NvPmuApp* out);

// What the PMU path did (or, in a dry run, would do).
struct PmuPostReport {
  bool used = false;                  // post() took the PMU path (GM20x .. GV100)
  bool engine_reset = false;          // the falcon is reset through its own 0x3c0 (gp102_flcn_reset_eng); otherwise through PMC_ENABLE
  NvPmuAppStatus devinit_status = NvPmuAppStatus::NotFound;
  NvPmuApp devinit;
  uint32_t init_table = 0, init_table_length = 0;     // BIT 'I' +0x14/+0x16: the tables the boot scripts' opcodes use
  uint32_t boot_scripts = 0, boot_scripts_length = 0; // BIT 'I' +0x18/+0x1a: the boot scripts
  bool dmem_targets_known = false;    // (a dry run takes them from the image's data section; unknown if they point elsewhere)
  uint32_t init_table_dmem = 0, boot_scripts_dmem = 0;
  uint32_t imem_words = 0, dmem_words = 0;   // words written (or that would be) to the falcon's code and data ports
  bool started = false;               // the DEVINIT application was started
  bool completed = false;             // ... and set mailbox 0x10a040 bit 13 within the timeout
  uint32_t waited_us = 0;
  NvPmuAppStatus preos_status = NvPmuAppStatus::NotFound;
  bool preos_started = false;         // the optional PRE_OS application (fan control until a driver PMU loads) was started
};

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
  // GM20x .. GV100 (the PMU path): how long the DEVINIT application may run (nouveau: 2 s), the poll interval, and whether the PRE_OS
  // application is started afterwards (gm200_devinit_preos).
  uint32_t pmu_timeout_us = 2000000;
  uint32_t pmu_poll_us = 10;
  bool run_preos = true;
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
  PmuPostReport pmu;                     // GM20x .. GV100
};

class NvDevinit {
 public:
  // gf100_devinit_preinit: bit 1 of 0x2240c is set by devinit and goes back to 0 on suspend.
  static constexpr uint32_t kPostedGf100 = 0x02240c, kPostedBit = 0x2;
  // tu102_devinit_wait: 0x118128 bit 0 and 0x118234 bits 7:0 == 0xff once devinit has finished.
  static constexpr uint32_t kDevinitStatusTu102 = 0x118128, kDevinitDoneTu102 = 0x118234;

  [[nodiscard]] static PostStatus detect(INvRegisters& registers, const Chip& chip);
  // The PMU falcon (gm200_devinit.c's registers, BAR0 offsets).
  static constexpr uint32_t kPmuFalcon = 0x10a000;
  static constexpr uint32_t kPmcEnable = 0x000200, kPmcEnablePmu = 0x00002000;

  // nvkm_devinit_preinit + nvkm_devinit_post for Tesla (G80) to Volta: unlock the extended VGA CRTC registers, decide (as
  // nv50/gf100_devinit_preinit do) whether a POST is needed, and if so (or forced):
  //   - Tesla to Maxwell GM10x: run the init scripts in nvbios_post's order (the script table's entries, then the script at BIT 'I' +
  //     0x0e);
  //   - GM20x, Pascal and Volta (gm200_devinit_post): reset the PMU falcon, upload the VBIOS's DEVINIT application (BIT 'p' type 0x04),
  //     the init tables and boot scripts (BIT 'I' +0x14 .. +0x1a) into its DMEM where the application's argument block says, start it and
  //     wait (bounded) for it to report completion; then start the PRE_OS application (type 0x01) if the image has one.
  // Refused: pre-Tesla (VGA owner and memory init not done), Turing and later (FWSEC, under GSP control), Tegra (the SoC's firmware),
  // and on the PMU path an image without BIT 'I' (version 1, 0x1c bytes) or a usable DEVINIT application (checked before anything is
  // written).
  [[nodiscard]] static PostReport post(const PostInputs& inputs);
  static const char* state_name(PostState state);
};

}  // namespace nv
}  // namespace hardware
