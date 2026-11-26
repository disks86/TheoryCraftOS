/*
 * fx4_disasm.h - the listing of an fx_4_0 / fx_4_1 effect (D3D10DisassembleEffect), in the shape the effect compiler's listing has: the header comment, the constant
 * buffers with their variables (offsets, sizes, default values), the objects (state objects with their assignments, shaders as their listings, strings), the shared
 * variables of a child effect, then the techniques and passes with every assignment - constants, variables, indexed variables, index and value expressions (their
 * preshader instructions) and anonymous shaders.
 *
 * It reads an effect fx4_parse has read (so everything it follows has been checked) and writes through dxbc_disasm.h's DxdText; the shaders are listed with
 * dxd_disassemble. Plain C with no library underneath (a host test includes it): the includer defines DXD_REALLOC (see dxbc_disasm.h) and DXD_FREE (free-like).
 */
#ifndef TCOS_FX4_DISASM_H
#define TCOS_FX4_DISASM_H

#include "fx4.h"
#include "dxbc_disasm.h"

static void fx4d_indent(DxdText *t, uint32_t depth) { for (uint32_t i = 0; i < depth; ++i) dxd_lit(t, "    "); }
static void fx4d_comment(DxdText *t, const char *s) { dxd_color(t, "#808080"); dxd_lit(t, s); dxd_end_color(t); }

/* ---- types and values ------------------------------------------------------------------------------------------------------------------------------------------ */

static const char *fx4d_base_name(uint32_t base) {
  return base == FX4_BASE_FLOAT ? "float" : base == FX4_BASE_INT ? "int" : base == FX4_BASE_UINT ? "uint" : "bool";
}
static void fx4d_scalar(DxdText *t, uint32_t base, uint32_t bits) {
  if (base == FX4_BASE_FLOAT) dxd_float_bits(t, bits);
  else if (base == FX4_BASE_INT) dxd_int(t, (int32_t)bits);
  else if (base == FX4_BASE_UINT) dxd_uint(t, bits);
  else dxd_lit(t, bits ? "true" : "false");
}
/* The packed values of a numeric or struct type from `src` (at most `available` bytes; *at counts what was read): a value, or braces around several. */
static void fx4d_values(DxdText *t, const Fx4Type *type, const uint8_t *src, uint32_t available, uint32_t *at, uint32_t depth) {
  if (depth > FX4_MAX_DEPTH) return;
  const uint32_t elements = type->elements ? type->elements : 1;
  if (type->kind == FX4_KIND_NUMERIC) {
    const uint32_t count = elements * type->rows * type->columns;      /* (at most 4 x 4 x elements: rows and columns are 1..4) */
    if (count > 1) dxd_lit(t, "{ ");
    for (uint32_t i = 0; i < count; ++i) {
      if (*at > available || available - *at < 4) { dxd_lit(t, "?"); break; }
      if (i) dxd_lit(t, ", ");
      fx4d_scalar(t, type->numeric_base, fx4_load32(src + *at));
      *at += 4;
    }
    if (count > 1) dxd_lit(t, " }");
    return;
  }
  if (type->kind != FX4_KIND_STRUCT) return;
  if (type->elements) dxd_lit(t, "{ ");
  for (uint32_t k = 0; k < elements; ++k) {
    if (k) dxd_lit(t, ", ");
    dxd_lit(t, "{ ");
    for (uint32_t m = 0; m < type->member_count; ++m) {
      if (m) dxd_lit(t, ", ");
      fx4d_values(t, type->members[m].type, src, available, at, depth + 1);
    }
    dxd_lit(t, " }");
  }
  if (type->elements) dxd_lit(t, " }");
}
static void fx4d_elements(DxdText *t, const Fx4Type *type) {
  if (!type->elements) return;
  dxd_lit(t, "[");
  dxd_uint(t, type->elements);
  dxd_lit(t, "]");
}
/* A declaration: the type, the name, the array size and the semantic. */
static void fx4d_declaration(DxdText *t, const Fx4Type *type, const char *name, const char *semantic) {
  dxd_color(t, "#0000ff");
  dxd_str(t, type->name ? type->name : "?");
  dxd_end_color(t);
  dxd_lit(t, " ");
  dxd_str(t, name ? name : "");
  fx4d_elements(t, type);
  if (semantic && *semantic) { dxd_lit(t, " : "); dxd_str(t, semantic); }
}
static void fx4d_string(DxdText *t, const char *s) {
  dxd_lit(t, "\"");
  dxd_str(t, s ? s : "");
  dxd_lit(t, "\"");
}
static void fx4d_annotations(DxdText *t, const Fx4Annotations *a, uint32_t depth) {
  if (!a->count) return;
  dxd_lit(t, "\n");
  fx4d_indent(t, depth);
  dxd_lit(t, "<\n");
  for (uint32_t i = 0; i < a->count; ++i) {
    const Fx4Annotation *item = &a->items[i];
    fx4d_indent(t, depth + 1);
    fx4d_declaration(t, item->type, item->name, NULL);
    dxd_lit(t, " = ");
    if (item->strings) {
      const uint32_t n = item->type->elements ? item->type->elements : 1;
      if (item->type->elements) dxd_lit(t, "{ ");
      for (uint32_t k = 0; k < n; ++k) { if (k) dxd_lit(t, ", "); fx4d_string(t, item->strings[k]); }
      if (item->type->elements) dxd_lit(t, " }");
    } else {
      uint32_t at = 0;
      fx4d_values(t, item->type, item->values, item->type->packed, &at, 0);
    }
    dxd_lit(t, ";\n");
  }
  fx4d_indent(t, depth);
  dxd_lit(t, ">");
}

/* ---- state values -------------------------------------------------------------------------------------------------------------------------------------------------- */

/* The names of the enumerations the properties take (Direct3D 10's values), by property id. */
static const char *fx4d_enum_name(uint32_t id, uint32_t v) {
  static const char *const blend[] = {NULL, "ZERO", "ONE", "SRC_COLOR", "INV_SRC_COLOR", "SRC_ALPHA", "INV_SRC_ALPHA", "DEST_ALPHA", "INV_DEST_ALPHA", "DEST_COLOR",
                                      "INV_DEST_COLOR", "SRC_ALPHA_SAT", NULL, NULL, "BLEND_FACTOR", "INV_BLEND_FACTOR", "SRC1_COLOR", "INV_SRC1_COLOR", "SRC1_ALPHA",
                                      "INV_SRC1_ALPHA"};
  static const char *const blend_op[] = {NULL, "ADD", "SUBTRACT", "REV_SUBTRACT", "MIN", "MAX"};
  static const char *const comparison[] = {NULL, "NEVER", "LESS", "EQUAL", "LESS_EQUAL", "GREATER", "NOT_EQUAL", "GREATER_EQUAL", "ALWAYS"};
  static const char *const stencil_op[] = {NULL, "KEEP", "ZERO", "REPLACE", "INCR_SAT", "DECR_SAT", "INVERT", "INCR", "DECR"};
  static const char *const fill[] = {NULL, NULL, "WIREFRAME", "SOLID"};
  static const char *const cull[] = {NULL, "NONE", "FRONT", "BACK"};
  static const char *const write_mask[] = {"ZERO", "ALL"};
  static const char *const address[] = {NULL, "WRAP", "MIRROR", "CLAMP", "BORDER", "MIRROR_ONCE"};
  static const struct { uint32_t value; const char *name; } filters[] = {
    {0x00, "MIN_MAG_MIP_POINT"}, {0x01, "MIN_MAG_POINT_MIP_LINEAR"}, {0x04, "MIN_POINT_MAG_LINEAR_MIP_POINT"}, {0x05, "MIN_POINT_MAG_MIP_LINEAR"},
    {0x10, "MIN_LINEAR_MAG_MIP_POINT"}, {0x11, "MIN_LINEAR_MAG_POINT_MIP_LINEAR"}, {0x14, "MIN_MAG_LINEAR_MIP_POINT"}, {0x15, "MIN_MAG_MIP_LINEAR"},
    {0x55, "ANISOTROPIC"}, {0x80, "COMPARISON_MIN_MAG_MIP_POINT"}, {0x81, "COMPARISON_MIN_MAG_POINT_MIP_LINEAR"}, {0x84, "COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT"},
    {0x85, "COMPARISON_MIN_POINT_MAG_MIP_LINEAR"}, {0x90, "COMPARISON_MIN_LINEAR_MAG_MIP_POINT"}, {0x91, "COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR"},
    {0x94, "COMPARISON_MIN_MAG_LINEAR_MIP_POINT"}, {0x95, "COMPARISON_MIN_MAG_MIP_LINEAR"}, {0xd5, "COMPARISON_ANISOTROPIC"}, {0x100, "TEXT_1BIT"},
  };
#define FX4D_PICK(table) return v < sizeof(table) / sizeof(table[0]) ? table[v] : NULL
  switch (id) {
    case 12: FX4D_PICK(fill);
    case 13: FX4D_PICK(cull);
    case 23: FX4D_PICK(write_mask);
    case 24: case 31: case 35: case 51: FX4D_PICK(comparison);
    case 28: case 29: case 30: case 32: case 33: case 34: FX4D_PICK(stencil_op);
    case 38: case 39: case 41: case 42: FX4D_PICK(blend);
    case 40: case 43: FX4D_PICK(blend_op);
    case 46: case 47: case 48: FX4D_PICK(address);
    case 45:
      for (uint32_t i = 0; i < sizeof filters / sizeof filters[0]; ++i)
        if (filters[i].value == v) return filters[i].name;
      return NULL;
    default: return NULL;
  }
#undef FX4D_PICK
}
static void fx4d_constant(DxdText *t, const Fx4Assignment *a) {
  /* (the type the property takes: the binary keeps booleans and enumerations as integers) */
  const uint32_t kind = fx4_properties[a->id].type;
  const uint32_t base = kind == FX4_PT_BOOL ? FX4_BASE_BOOL : kind == FX4_PT_FLOAT ? FX4_BASE_FLOAT : kind == FX4_PT_INT ? FX4_BASE_INT : FX4_BASE_UINT;
  dxd_lit(t, fx4d_base_name(base));
  if (a->value_count > 1) dxd_uint(t, a->value_count);
  dxd_lit(t, "(");
  for (uint32_t i = 0; i < a->value_count; ++i) {
    if (i) dxd_lit(t, ", ");
    const char *name = a->values[i].type != FX4_BASE_FLOAT ? fx4d_enum_name(a->id, a->values[i].bits) : NULL;
    if (base == FX4_BASE_BOOL) { dxd_lit(t, a->values[i].bits ? "TRUE" : "FALSE"); continue; }
    if (name) { dxd_lit(t, name); dxd_lit(t, " /* "); }
    fx4d_scalar(t, a->values[i].type, a->values[i].bits);
    if (name) dxd_lit(t, " */");
  }
  dxd_lit(t, ")");
}

/* ---- expressions --------------------------------------------------------------------------------------------------------------------------------------------------- */

static const char *fx4d_op_name(uint32_t op) {
  static const struct { uint16_t op; const char *name; } names[] = {
    {0x100, "mov"}, {0x101, "neg"}, {0x103, "rcp"}, {0x104, "frc"}, {0x105, "exp"}, {0x106, "log"}, {0x107, "rsq"}, {0x108, "sin"}, {0x109, "cos"}, {0x10a, "asin"},
    {0x10b, "acos"}, {0x10c, "atan"}, {0x112, "sqrt"}, {0x120, "ineg"}, {0x121, "not"}, {0x130, "itof"}, {0x131, "utof"}, {0x133, "ftou"}, {0x137, "ftob"},
    {0x139, "floor"}, {0x13a, "ceil"}, {0x200, "min"}, {0x201, "max"}, {0x204, "add"}, {0x205, "mul"}, {0x206, "atan2"}, {0x208, "div"}, {0x210, "ilt"},
    {0x211, "ige"}, {0x212, "ieq"}, {0x213, "ine"}, {0x214, "uge"}, {0x215, "ult"}, {0x216, "iadd"}, {0x219, "imul"}, {0x21a, "udiv"}, {0x21d, "imin"},
    {0x21e, "imax"}, {0x21f, "umin"}, {0x220, "umax"}, {0x230, "and"}, {0x231, "or"}, {0x233, "xor"}, {0x234, "ishl"}, {0x235, "ishr"}, {0x236, "ushr"},
    {0x301, "movc"}, {0x500, "dot"}, {0x70e, "dotswiz"},
  };
  for (uint32_t i = 0; i < sizeof names / sizeof names[0]; ++i)
    if (names[i].op == op) return names[i].name;
  return "?";
}
/* One parameter: a literal's values, or a register of the constant buffer (c), the results (expr) or the temporaries (r) with the components it reaches. */
static void fx4d_operand(DxdText *t, const Fx4Preshader *p, uint32_t table, uint32_t offset, uint32_t comps) {
  if (table == FX4_TABLE_CONSTANTS) {
    if (comps > 1) dxd_lit(t, "(");
    for (uint32_t i = 0; i < comps; ++i) {
      if (i) dxd_lit(t, ", ");
      if (offset > p->literal_count || p->literal_count - offset <= i) { dxd_lit(t, "?"); break; }
      dxd_float_bits(t, fx4_load32(p->literals + 4 * ((size_t)offset + i)));
    }
    if (comps > 1) dxd_lit(t, ")");
    return;
  }
  dxd_lit(t, table == FX4_TABLE_CB ? "c" : table == FX4_TABLE_RESULT ? "expr" : "r");
  dxd_uint(t, offset / 4);
  dxd_lit(t, ".");
  for (uint32_t i = 0; i < comps; ++i) dxd_raw(t, &"xyzw"[(offset + i) % 4], 1);
}
static void fx4d_expression(DxdText *t, const Fx4Preshader *p, uint32_t depth) {
  fx4d_indent(t, depth);
  dxd_color(t, "#408040");
  dxd_lit(t, "asm {\n");
  dxd_end_color(t);
  for (uint32_t i = 0; i < p->var_count; ++i) {
    fx4d_indent(t, depth + 1);
    dxd_color(t, "#808080");
    dxd_lit(t, "// c");
    dxd_uint(t, p->vars[i].reg_index);
    if (p->vars[i].reg_count > 1) { dxd_lit(t, "..c"); dxd_uint(t, p->vars[i].reg_index + p->vars[i].reg_count - 1); }
    dxd_lit(t, ": ");
    dxd_str(t, p->vars[i].name);
    dxd_end_color(t);
    dxd_lit(t, "\n");
  }
  fx4d_indent(t, depth + 1);
  dxd_lit(t, "cli4 ");
  dxd_uint(t, p->literal_count);
  dxd_lit(t, "\n");
  /* (the instructions as fx4_preshader checked them: the count, then each its token, input count and (flags, table, offset) a parameter) */
  const uint32_t bytes = p->code_words * 4;
  uint32_t count = 0, at = 4;
  if (!fx4_word(p->code, bytes, 0, &count)) count = 0;
  for (uint32_t k = 0; k < count; ++k) {
    uint32_t token, inputs;
    if (!fx4_word(p->code, bytes, at, &token) || !fx4_word(p->code, bytes, at + 4, &inputs) || inputs > 8 || !fx4_fits(bytes, at + 8, inputs + 1, 12)) break;
    at += 8;
    const uint32_t comps = FX4_COMPONENTS(token), op = FX4_OPCODE(token);
    fx4d_indent(t, depth + 1);
    dxd_color(t, "#0000ff");
    dxd_lit(t, fx4d_op_name(op));
    dxd_end_color(t);
    /* (the output first, as the compiler lists it) */
    const uint32_t out = at + 12 * inputs;
    const uint32_t out_comps = op == 0x500 || op == 0x70e ? 1 : comps;
    dxd_lit(t, " ");
    fx4d_operand(t, p, fx4_load32(p->code + out + 4), fx4_load32(p->code + out + 8), out_comps);
    for (uint32_t a = 0; a < inputs; ++a) {
      const uint32_t used = (FX4_SCALAR(token) && a == 0 && inputs >= 2) || op == 0x70e || (op == 0x121 && a == 0) ? 1 : comps;
      dxd_lit(t, ", ");
      fx4d_operand(t, p, fx4_load32(p->code + at + 4), fx4_load32(p->code + at + 8), used);
      at += 12;
    }
    at += 12;
    dxd_lit(t, "\n");
  }
  fx4d_indent(t, depth);
  dxd_lit(t, "}");
}

/* ---- shaders ------------------------------------------------------------------------------------------------------------------------------------------------------- */

static int fx4d_same(const char *a, const char *b, size_t n) { for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return 0; return 1; }
/* A shader's listing (dxd_disassemble), every line indented; NULL for none. 0 when the text could not grow. */
static int fx4d_shader(DxdText *t, const Fx4Shader *s, uint32_t depth) {
  if (!s->code) { dxd_lit(t, "NULL"); return 1; }
  DxdText code = {0, 0, 0, 0, t->html};
  const int ok = dxd_disassemble(&code, s->code, s->size, NULL);
  if (code.failed) { DXD_FREE(code.data); t->failed = 1; return 0; }
  if (s->so_decl) dxd_lit(t, "ConstructGSWithSO( ");
  if (!ok) {
    fx4d_comment(t, "/* (a shader that cannot be listed) */");
  } else {
    /* (the listing without the page around it: this listing has one) */
    static const char head[] = "<html><body><pre>\n", tail[] = "</pre></body></html>\n";
    size_t from = 0, to = code.size;
    if (t->html && to >= sizeof head - 1 && fx4d_same(code.data, head, sizeof head - 1)) from = sizeof head - 1;
    if (t->html && to - from >= sizeof tail - 1 && fx4d_same(code.data + to - (sizeof tail - 1), tail, sizeof tail - 1)) to -= sizeof tail - 1;
    dxd_color(t, "#408040");
    dxd_lit(t, "asm {\n");
    dxd_end_color(t);
    size_t line = from;
    for (size_t i = from; i < to; ++i) {
      if (code.data[i] != '\n') continue;
      fx4d_indent(t, depth + 1);
      dxd_raw(t, code.data + line, i - line + 1);
      line = i + 1;
    }
    if (line < to) { fx4d_indent(t, depth + 1); dxd_raw(t, code.data + line, to - line); dxd_lit(t, "\n"); }
    fx4d_indent(t, depth);
    dxd_lit(t, "}");
  }
  DXD_FREE(code.data);
  if (s->so_decl) { dxd_lit(t, ", "); fx4d_string(t, s->so_decl); dxd_lit(t, " )"); }
  return 1;
}

/* ---- assignments --------------------------------------------------------------------------------------------------------------------------------------------------- */

static void fx4d_assignment(DxdText *t, const Fx4Effect *e, const Fx4Assignment *a, uint32_t depth) {
  const Fx4Property *prop = &fx4_properties[a->id];
  const uint32_t count = e->version == FX4_VERSION_4_1 ? prop->count_4_1 : prop->count;
  fx4d_indent(t, depth);
  dxd_lit(t, prop->name);
  if (count > 1) { dxd_lit(t, "["); dxd_uint(t, a->index); dxd_lit(t, "]"); }
  dxd_lit(t, " = ");
  switch (a->op) {
    case FX4_OP_CONST: fx4d_constant(t, a); break;
    case FX4_OP_VAR: dxd_str(t, a->name); break;
    case FX4_OP_CONST_INDEX: dxd_str(t, a->name); dxd_lit(t, "["); dxd_uint(t, a->element); dxd_lit(t, "]"); break;
    case FX4_OP_INDEX_EXPRESSION:
      dxd_str(t, a->name);
      dxd_lit(t, "[\n");
      fx4d_expression(t, &a->expression, depth + 1);
      dxd_lit(t, "\n");
      fx4d_indent(t, depth);
      dxd_lit(t, "]");
      break;
    case FX4_OP_VALUE_EXPRESSION:
      dxd_lit(t, "\n");
      fx4d_expression(t, &a->expression, depth + 1);
      break;
    case FX4_OP_ANONYMOUS_SHADER: fx4d_shader(t, &a->shader, depth); break;
    default: dxd_lit(t, "?"); break;
  }
  dxd_lit(t, ";\n");
}
static void fx4d_assignments(DxdText *t, const Fx4Effect *e, const Fx4Assignments *list, uint32_t depth) {
  for (uint32_t i = 0; i < list->count; ++i) fx4d_assignment(t, e, &list->items[i], depth);
}

/* ---- the sections -------------------------------------------------------------------------------------------------------------------------------------------------- */

static void fx4d_section(DxdText *t, uint32_t count, const char *what) {
  dxd_color(t, "#808080");
  dxd_lit(t, "//\n// ");
  dxd_uint(t, count);
  dxd_lit(t, " ");
  dxd_lit(t, what);
  dxd_lit(t, "\n//\n");
  dxd_end_color(t);
}
static void fx4d_buffer(DxdText *t, const Fx4Buffer *b, int shared) {
  if (shared) dxd_lit(t, "shared ");
  dxd_color(t, "#0000ff");
  dxd_lit(t, b->tbuffer ? "tbuffer" : "cbuffer");
  dxd_end_color(t);
  dxd_lit(t, " ");
  dxd_str(t, b->name ? b->name : "");
  if (b->bind != 0xFFFFFFFFu && !shared) { dxd_lit(t, " : register("); dxd_lit(t, b->tbuffer ? "t" : "b"); dxd_uint(t, b->bind); dxd_lit(t, ")"); }
  fx4d_annotations(t, &b->annotations, 0);
  dxd_lit(t, "\n{\n");
  for (uint32_t i = 0; i < b->count; ++i) {
    const Fx4Numeric *v = &b->vars[i];
    fx4d_indent(t, 1);
    const size_t start = t->size;
    fx4d_declaration(t, v->type, v->name, v->semantic);
    if (v->defaults) {
      dxd_lit(t, " = ");
      uint32_t at = 0;
      const uint64_t packed = fx4_packed_bytes(v->type, 0);
      fx4d_values(t, v->type, v->defaults, packed > 0xFFFFFFFFull ? 0 : (uint32_t)packed, &at, 0);
    }
    fx4d_annotations(t, &v->annotations, 1);
    dxd_lit(t, ";");
    /* (the offset and size in a column, as the compiler's listing has them) */
    const size_t written = t->size >= start ? t->size - start : 0;
    for (size_t k = written; k < 36; ++k) dxd_lit(t, " ");
    dxd_color(t, "#808080");
    dxd_lit(t, " // Offset: ");
    dxd_pad_uint(t, v->offset, 4);
    dxd_lit(t, ", size: ");
    dxd_pad_uint(t, v->type->unpacked, 4);
    dxd_end_color(t);
    dxd_lit(t, "\n");
  }
  dxd_lit(t, "}\n\n");
}
static void fx4d_object(DxdText *t, const Fx4Effect *e, const Fx4Object *o, int shared) {
  const Fx4Type *type = o->type;
  if (shared) dxd_lit(t, "shared ");
  fx4d_declaration(t, type, o->name, o->semantic);
  if (o->bind != 0xFFFFFFFFu && o->bind && !shared) { dxd_lit(t, " : register("); dxd_uint(t, o->bind); dxd_lit(t, ")"); }
  if (!shared) fx4d_annotations(t, &o->annotations, 0);
  if (shared || !o->element_count) { dxd_lit(t, ";\n\n"); return; }
  const int many = type->elements != 0;
  if (type->object == FX4_OBJ_STRING) {
    dxd_lit(t, " = ");
    if (many) dxd_lit(t, "{ ");
    for (uint32_t i = 0; i < o->element_count; ++i) { if (i) dxd_lit(t, ", "); fx4d_string(t, o->elements[i].string); }
    if (many) dxd_lit(t, " }");
    dxd_lit(t, ";\n\n");
    return;
  }
  if (fx4_is_shader(type)) {
    dxd_lit(t, " = ");
    if (many) dxd_lit(t, "{\n");
    for (uint32_t i = 0; i < o->element_count; ++i) {
      if (many) fx4d_indent(t, 1);
      fx4d_shader(t, &o->elements[i].shader, many ? 1 : 0);
      if (many) dxd_lit(t, i + 1 < o->element_count ? ",\n" : "\n");
    }
    if (many) dxd_lit(t, "}");
    dxd_lit(t, ";\n\n");
    return;
  }
  if (fx4_is_state(type)) {
    dxd_lit(t, "\n");
    if (many) dxd_lit(t, "[\n");
    const uint32_t depth = many ? 1 : 0;
    for (uint32_t i = 0; i < o->element_count; ++i) {
      fx4d_indent(t, depth);
      dxd_lit(t, "{\n");
      fx4d_assignments(t, e, &o->elements[i].state, depth + 1);
      fx4d_indent(t, depth);
      dxd_lit(t, !many ? "}" : i + 1 < o->element_count ? "},\n" : "}\n");
    }
    if (many) dxd_lit(t, "]");
    dxd_lit(t, ";\n\n");
    return;
  }
  dxd_lit(t, ";\n\n");      /* (textures and views: the declaration) */
}

/* The listing of an effect: 1, or 0 when the text could not grow (t->failed). */
static int fx4_disassemble(DxdText *t, const Fx4Effect *e, int child) {
  if (t->html) dxd_lit(t, "<html><body><pre>\n");
  dxd_color(t, "#808080");
  dxd_lit(t, "//\n// FX Version: ");
  dxd_lit(t, e->version == FX4_VERSION_4_1 ? "fx_4_1" : "fx_4_0");
  dxd_lit(t, "\n// Child effect (requires effect pool): ");
  dxd_lit(t, child ? "true" : "false");
  dxd_lit(t, "\n//\n");
  dxd_end_color(t);
  if (e->buffer_count) {
    fx4d_section(t, e->buffer_count, "local buffer(s)");
    for (uint32_t i = 0; i < e->buffer_count; ++i) fx4d_buffer(t, &e->buffers[i], 0);
  }
  if (e->object_count) {
    fx4d_section(t, e->object_count, "local object(s)");
    for (uint32_t i = 0; i < e->object_count; ++i) fx4d_object(t, e, &e->objects[i], 0);
  }
  if (e->shared_buffer_count) {
    fx4d_section(t, e->shared_buffer_count, "shared buffer(s)");
    for (uint32_t i = 0; i < e->shared_buffer_count; ++i) fx4d_buffer(t, &e->shared_buffers[i], 1);
  }
  if (e->shared_object_count) {
    fx4d_section(t, e->shared_object_count, "shared object(s)");
    for (uint32_t i = 0; i < e->shared_object_count; ++i) fx4d_object(t, e, &e->shared_objects[i], 1);
  }
  if (e->technique_count) {
    fx4d_section(t, e->technique_count, "technique(s)");
    for (uint32_t i = 0; i < e->technique_count; ++i) {
      const Fx4Technique *tech = &e->techniques[i];
      dxd_color(t, "#0000ff");
      dxd_lit(t, "technique10");
      dxd_end_color(t);
      dxd_lit(t, " ");
      dxd_str(t, tech->name ? tech->name : "");
      fx4d_annotations(t, &tech->annotations, 0);
      dxd_lit(t, "\n{\n");
      for (uint32_t k = 0; k < tech->pass_count; ++k) {
        const Fx4Pass *pass = &tech->passes[k];
        fx4d_indent(t, 1);
        dxd_color(t, "#0000ff");
        dxd_lit(t, "pass");
        dxd_end_color(t);
        dxd_lit(t, " ");
        dxd_str(t, pass->name ? pass->name : "");
        fx4d_annotations(t, &pass->annotations, 1);
        dxd_lit(t, "\n");
        fx4d_indent(t, 1);
        dxd_lit(t, "{\n");
        fx4d_assignments(t, e, &pass->state, 2);
        fx4d_indent(t, 1);
        dxd_lit(t, k + 1 < tech->pass_count ? "}\n\n" : "}\n");
      }
      dxd_lit(t, "}\n\n");
    }
  }
  if (t->html) dxd_lit(t, "</pre></body></html>\n");
  return !t->failed;
}

#endif
