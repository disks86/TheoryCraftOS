#include "Emulation/AArch64SveCommon.hpp"
namespace emulation {
bool AArch64Cpu::sve2(uint32_t) { return false; }
bool AArch64Cpu::sve_float(uint32_t) { return false; }
}
