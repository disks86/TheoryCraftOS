// Reads instruction words from standard input (4 bytes each, a compressed one in the low half) and writes one byte per word: 0 the RISC-V emulator executes it, 1 it is an invalid instruction,
// 2 another fault. Used by riscv_diff.sh to tell the instructions the emulator lacks from the ones it runs.
#include <cstdio>
#include <cstdlib>
#include "Process/AddressSpace.hpp"
#include "Emulation/RiscV64Cpu.hpp"
using namespace emulation;
struct A final : allocators::IAllocator { void* allocate(uint64_t s) override { return malloc(s); } void* allocate_aligned(uint64_t s, uint64_t) override { return malloc(s); } void deallocate(void* p) override { free(p); } };
// Reads instruction words from stdin (4 bytes each) and prints one byte per word: 0 executed, 1 invalid instruction here, 2 another fault.
int main() {
  A alloc;
  process::AddressSpace space{alloc};
  space.map(0x10000, 0x1000, process::kProtRead | process::kProtWrite | process::kProtExecute);
  space.map(0x20000, 0x2000, process::kProtRead | process::kProtWrite);
  uint32_t w;
  RiscV64Cpu cpu;
  while (fread(&w, 4, 1, stdin) == 1) {
    const bool wide = (w & 3) == 3;
    uint8_t raw[8] = {uint8_t(w), uint8_t(w >> 8), uint8_t(w >> 16), uint8_t(w >> 24), 0x13, 0x00, 0x00, 0x00};
    if (!wide) { raw[2] = 0x01; raw[3] = 0x00; }      // c.nop after a compressed instruction
    space.write_privileged(0x10000, ConstByteView(raw, 8));
    process::ProgramStart start;
    start.arch = process::UserArch::RiscV64; start.entry = 0x10000; start.stack_pointer = 0x21000;
    cpu.reset(start);
    for (int r = 1; r < 32; ++r) cpu.state().x[r] = 0x20800;
    cpu.state().vtype = 0; cpu.state().vl = 4;
    GuestMemory m(space);
    const StopReason r = cpu.run(m, 1);
    uint8_t out = 0;
    if (r == StopReason::Fault) out = cpu.fault().vector == 6 ? 1 : 2;
    fwrite(&out, 1, 1, stdout);
  }
}
