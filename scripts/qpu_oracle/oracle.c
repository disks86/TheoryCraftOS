#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "broadcom/common/v3d_device_info.h"
#include "qpu_instr.h"
#include "qpu_disasm.h"
/* reads 64-bit instruction words in hex from stdin, prints the disassembly */
int main(void) {
  struct v3d_device_info devinfo = { .ver = 42 };
  char line[64];
  while (fgets(line, sizeof line, stdin)) {
    uint64_t w = strtoull(line, NULL, 16);
    struct v3d_qpu_instr inst;
    if (!v3d_qpu_instr_unpack(&devinfo, w, &inst)) { printf("%016llx UNPACK FAILED\n", (unsigned long long)w); continue; }
    uint64_t back;
    int ok = v3d_qpu_instr_pack(&devinfo, &inst, &back);
    printf("%016llx %s repack=%s%s\n", (unsigned long long)w, v3d_qpu_decode(&devinfo, &inst), ok ? "ok" : "FAIL", (ok && back == w) ? "" : " (differs)");
  }
}
