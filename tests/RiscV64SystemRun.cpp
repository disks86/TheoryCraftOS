// Runs a flat RISC-V binary on RiscV64Machine (privileged mode) and prints what it writes to the UART. Usage: RiscV64SystemRun <image.bin> [<instructions>]
// Used by riscv_sys.sh.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "Emulation/RiscV64Machine.hpp"
using namespace emulation;

struct A final : allocators::IAllocator { void* allocate(uint64_t s) override { return malloc(s); } void* allocate_aligned(uint64_t s, uint64_t) override { return malloc(s); } void deallocate(void* p) override { free(p); } };

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 2;
  std::vector<uint8_t> image;
  uint8_t buffer[65536];
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) image.insert(image.end(), buffer, buffer + n);
  fclose(f);
  A allocator;
  process::AddressSpace space(allocator);
  RiscV64Machine machine(allocator, space, 64ull << 20);
  if (!machine.ok() || !machine.load(RiscV64Machine::kRamBase, ConstByteView(image.data(), image.size()))) return 2;
  machine.start(RiscV64Machine::kRamBase);
  const uint64_t limit = argc > 2 ? strtoull(argv[2], nullptr, 0) : 200000000ull;
  const bool done = machine.run(limit);
  ConstByteView out = machine.console();
  fwrite(out.data(), 1, out.size(), stdout);
  if (!done) fprintf(stderr, "[did not finish: pc=%llx]\n", (unsigned long long)machine.cpu().state().pc);
  return done ? 0 : 1;
}
