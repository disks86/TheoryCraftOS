// Reads 32-bit instruction words from standard input and writes one byte per word: 0 the AArch64 emulator executes it, 1 it is an undefined instruction to it (vector 6), 2 it faulted
// otherwise (a memory access, ...). Used by aarch64_diff.sh to tell the instructions the emulator lacks from the ones it runs, and as the decode-coverage measure.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "Emulation/AArch64Cpu.hpp"
#include "Process/AddressSpace.hpp"
using namespace emulation;
struct A final : allocators::IAllocator { void* allocate(uint64_t s) override { return malloc(s); } void* allocate_aligned(uint64_t s, uint64_t) override { return malloc(s); } void deallocate(void* p) override { free(p); } };
int main() {
  A alloc;
  process::AddressSpace space{alloc};
  space.map(0x10000, 0x1000, process::kProtRead | process::kProtWrite | process::kProtExecute);
  space.map(0x20000, 0x2000, process::kProtRead | process::kProtWrite);
  uint32_t w;
  AArch64Cpu cpu;
  while (fread(&w, 4, 1, stdin) == 1) {
    uint8_t raw[8] = {uint8_t(w), uint8_t(w >> 8), uint8_t(w >> 16), uint8_t(w >> 24), 0x1F, 0x20, 0x03, 0xD5};
    space.write_privileged(0x10000, ConstByteView(raw, 8));
    process::ProgramStart start;
    start.arch = process::UserArch::AArch64; start.entry = 0x10000; start.stack_pointer = 0x21000;
    cpu.reset(start);
    cpu.state().streaming = cpu.state().za_enabled = true;      // (so that SME instructions are not rejected for the mode they run in)
    GuestMemory m(space);
    const StopReason r = cpu.run(m, 1);
    uint8_t out = 0;
    if (r == StopReason::Fault) out = cpu.fault().vector == 6 ? 1 : 2;
    fwrite(&out, 1, 1, stdout);
  }
}
