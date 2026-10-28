/* The Linux amdgpu driver's AtomBIOS interpreter (atom.c, Copyright 2008 Advanced Micro Devices, Inc.; MIT license), compiled in user space against the stubs in this directory, with a thin
 * C interface for the differential test (atom_diff.cpp): run one command table of an image against a host whose register accesses are called back. */
#include "atom.c"

typedef struct {
  uint32_t (*reg_read)(void*, uint32_t);
  void (*reg_write)(void*, uint32_t, uint32_t);
  uint32_t (*mc_read)(void*, uint32_t);
  void (*mc_write)(void*, uint32_t, uint32_t);
  uint32_t (*pll_read)(void*, uint32_t);
  void (*pll_write)(void*, uint32_t, uint32_t);
  void (*delay_us)(void*, unsigned);
  void (*delay_ms)(void*, unsigned);
  void* user;
} diff_host;

unsigned long diff_jiffies;
static diff_host* g_host;

void diff_udelay(unsigned n) { g_host->delay_us(g_host->user, n); }
void diff_mdelay(unsigned n) { g_host->delay_ms(g_host->user, n); }
static uint32_t cb_reg_read(struct card_info* i, uint32_t reg) { (void)i; return g_host->reg_read(g_host->user, reg); }
static void cb_reg_write(struct card_info* i, uint32_t reg, uint32_t v) { (void)i; g_host->reg_write(g_host->user, reg, v); }
static uint32_t cb_mc_read(struct card_info* i, uint32_t reg) { (void)i; return g_host->mc_read(g_host->user, reg); }
static void cb_mc_write(struct card_info* i, uint32_t reg, uint32_t v) { (void)i; g_host->mc_write(g_host->user, reg, v); }
static uint32_t cb_pll_read(struct card_info* i, uint32_t reg) { (void)i; return g_host->pll_read(g_host->user, reg); }
static void cb_pll_write(struct card_info* i, uint32_t reg, uint32_t v) { (void)i; g_host->pll_write(g_host->user, reg, v); }

/* Returns the driver's result (0: ran to the end); *parsed says whether amdgpu_atom_parse accepted the image. `scratch_out` receives the 20 KB scratch memory. */
int linux_run(unsigned char* image, int table, uint32_t* ps, int ps_size, uint32_t* scratch_out, diff_host* host, int* parsed, uint32_t* version) {
  struct card_info card;
  struct atom_context* ctx;
  int r;
  memset(&card, 0, sizeof card);
  card.reg_read = cb_reg_read;
  card.reg_write = cb_reg_write;
  card.mc_read = cb_mc_read;
  card.mc_write = cb_mc_write;
  card.pll_read = cb_pll_read;
  card.pll_write = cb_pll_write;
  g_host = host;
  diff_jiffies = 0;
  ctx = amdgpu_atom_parse(&card, image);
  if (!ctx) {
    *parsed = 0;
    return -100;
  }
  *parsed = 1;
  *version = ctx->version;
  ctx->scratch = calloc(1, 20 * 1024);
  ctx->scratch_size_bytes = 20 * 1024;
  r = amdgpu_atom_execute_table(ctx, table, ps, ps_size);
  memcpy(scratch_out, ctx->scratch, 20 * 1024);
  free(ctx->scratch);
  amdgpu_atom_destroy(ctx);
  return r;
}
