#include <stdio.h>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dShaders.hpp"
using namespace hardware;
int main(){
  std::vector<uint64_t> code(512);
  v3d_shader::ShaderCode sc;
  bool ok = v3d_shader::build_colored_pipeline(ArrayView<uint64_t>(code.data(), code.size()), &sc);
  fprintf(stderr, "ok=%d cs=%u+%u vs=%u+%u fs=%u+%u fsz=%u+%u\n", ok, sc.coordinate.first, sc.coordinate.instructions, sc.vertex.first, sc.vertex.instructions, sc.fragment.first, sc.fragment.instructions, sc.fragment_z.first, sc.fragment_z.instructions);
  const v3d_shader::Program* ps[4] = {&sc.coordinate, &sc.vertex, &sc.fragment, &sc.fragment_z};
  for (auto* p : ps) { printf("#\n"); for (uint32_t i = 0; i < p->instructions; ++i) printf("%016llx\n", (unsigned long long)code[p->first + i]); }
}
