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

#include "Hardware/GraphicsProcessingUnit/AMD/Gcn/GcnCp.hpp"

namespace gcn {

using namespace pm4;

namespace {

constexpr uint32_t kMaxCodeDwords = 1u << 16;
constexpr uint32_t kEndProgram = 0xBF810000u;
constexpr uint64_t kMaxBufferBytes = 64ull << 20;
constexpr uint64_t kMaxTargetPixels = 1ull << 22;       // the model keeps the target as floats: 64 MiB at most
constexpr uint32_t kMaxConstRows = TgsiShader::kMaxConsts;

float bits_float(uint32_t b) {
  union { uint32_t u; float f; } c;
  c.u = b;
  return c.f;
}

uint32_t field(uint32_t value, uint32_t mask, uint32_t shift) { return (value & mask) >> shift; }

bool fill(List<uint32_t>& list, uint64_t count) {
  if (!list.reserve(count)) return false;
  for (uint64_t i = 0; i < count; ++i)
    if (!list.push_back(0u)) return false;
  return true;
}

uint64_t join(uint32_t lo, uint32_t hi) { return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32); }

}  // namespace

const char* CommandProcessorModel::result_name(Result r) {
  switch (r) {
    case Result::Ok: return "ok";
    case Result::BadPacket: return "bad packet";
    case Result::Unsupported: return "unsupported";
    case Result::MemoryFault: return "memory fault";
    case Result::ShaderFault: return "shader fault";
    case Result::OutOfMemory: return "out of memory";
  }
  return "?";
}

CommandProcessorModel::Result CommandProcessorModel::set_registers(const Pm4Packet& p, uint32_t* file, uint32_t start, uint32_t end) {
  if (p.body.size() < 2) return Result::BadPacket;
  const uint32_t offset = p.body[0];
  const uint64_t count = p.body.size() - 1;
  if (offset >= end - start || count > end - start - offset) return Result::BadPacket;
  for (uint64_t i = 0; i < count; ++i) file[offset + i] = p.body[1 + i];
  return Result::Ok;
}

CommandProcessorModel::Result CommandProcessorModel::read_code(uint64_t address, List<uint32_t>& code) {
  // walks the instructions from `address` up to and including s_endpgm
  for (uint64_t at = address; code.count() < kMaxCodeDwords;) {
    uint32_t w;
    if (!memory_.read(at, ArrayView<uint32_t>(&w, 1))) return Result::MemoryFault;
    const uint32_t words[1] = {w};
    const uint32_t length = instruction_length(ArrayView<const uint32_t>(words, 1));
    if (length == 0) return Result::Unsupported;
    if (!code.push_back(w)) return Result::OutOfMemory;
    for (uint32_t k = 1; k < length; ++k) {
      uint32_t extra;
      if (!memory_.read(at + 4ull * k, ArrayView<uint32_t>(&extra, 1))) return Result::MemoryFault;
      if (!code.push_back(extra)) return Result::OutOfMemory;
    }
    if (w == kEndProgram) return Result::Ok;
    at += 4ull * length;
  }
  return Result::Unsupported;
}

CommandProcessorModel::Result CommandProcessorModel::draw(bool indexed, const Pm4Packet& p) {
  using R = Result;
  const uint32_t expected = indexed ? 5 : 2;
  if (p.body.size() != expected) return R::BadPacket;
  if (indexed ? p.body[4] != kDI_SRC_SEL_DMA : (p.body[1] & 3) != kDI_SRC_SEL_AUTO_INDEX) return R::Unsupported;
  const uint32_t index_count = indexed ? p.body[3] : p.body[0];
  if (index_count == 0 || index_count % 3 != 0 || index_count > (1u << 24)) return R::BadPacket;
  if (num_instances_ != 1) return R::Unsupported;

  // fixed function state this model knows
  if (uconfig_reg(mmVGT_PRIMITIVE_TYPE) != kDI_PT_TRILIST || context_reg(mmVGT_SHADER_STAGES_EN) != 0) return R::Unsupported;
  if ((context_reg(mmCB_BLEND0_CONTROL) >> CB_BLEND0_CONTROL__ENABLE__SHIFT & 1) != 0) return R::Unsupported;
  if (context_reg(mmDB_DEPTH_CONTROL) != 0 || context_reg(mmCB_TARGET_MASK) != 0xF || context_reg(mmCB_SHADER_MASK) != 0xF) return R::Unsupported;
  const uint32_t cb_info = context_reg(mmCB_COLOR0_INFO);
  if (field(cb_info, CB_COLOR0_INFO__FORMAT_MASK, CB_COLOR0_INFO__FORMAT__SHIFT) != kCOLOR_32_32_32_32 ||
      field(cb_info, CB_COLOR0_INFO__NUMBER_TYPE_MASK, CB_COLOR0_INFO__NUMBER_TYPE__SHIFT) != kNUMBER_FLOAT ||
      field(cb_info, CB_COLOR0_INFO__COMP_SWAP_MASK, CB_COLOR0_INFO__COMP_SWAP__SHIFT) != kSWAP_STD ||
      field(cb_info, CB_COLOR0_INFO__ENDIAN_MASK, CB_COLOR0_INFO__ENDIAN__SHIFT) != kENDIAN_NONE)
    return R::Unsupported;
  if (field(context_reg(mmCB_COLOR_CONTROL), CB_COLOR_CONTROL__MODE_MASK, CB_COLOR_CONTROL__MODE__SHIFT) != kCB_NORMAL) return R::Unsupported;
  if (context_reg(mmSPI_PS_INPUT_ENA) != abi::kPsInputEnablePerspCenter || context_reg(mmSPI_PS_INPUT_ADDR) != abi::kPsInputEnablePerspCenter) return R::Unsupported;
  if (field(context_reg(mmSPI_SHADER_POS_FORMAT), SPI_SHADER_POS_FORMAT__POS0_EXPORT_FORMAT_MASK, SPI_SHADER_POS_FORMAT__POS0_EXPORT_FORMAT__SHIFT) != kSPI_SHADER_4COMP ||
      field(context_reg(mmSPI_SHADER_COL_FORMAT), SPI_SHADER_COL_FORMAT__COL0_EXPORT_FORMAT_MASK, SPI_SHADER_COL_FORMAT__COL0_EXPORT_FORMAT__SHIFT) != kSPI_SHADER_32_ABGR)
    return R::Unsupported;

  // the target
  const uint64_t color_base = static_cast<uint64_t>(context_reg(mmCB_COLOR0_BASE)) << 8;
  const uint32_t pitch = (field(context_reg(mmCB_COLOR0_PITCH), CB_COLOR0_PITCH__TILE_MAX_MASK, CB_COLOR0_PITCH__TILE_MAX__SHIFT) + 1) * 8;
  const uint32_t screen_br = context_reg(mmPA_SC_SCREEN_SCISSOR_BR);
  const uint32_t width = field(screen_br, PA_SC_SCREEN_SCISSOR_BR__BR_X_MASK, PA_SC_SCREEN_SCISSOR_BR__BR_X__SHIFT);
  const uint32_t height = field(screen_br, PA_SC_SCREEN_SCISSOR_BR__BR_Y_MASK, PA_SC_SCREEN_SCISSOR_BR__BR_Y__SHIFT);
  if (width == 0 || height == 0 || width > pitch || width > 16384 || height > 16384 || static_cast<uint64_t>(width) * height > kMaxTargetPixels) return R::Unsupported;

  DrawCall call;
  // shaders
  List<uint32_t> vs_code(allocator_), ps_code(allocator_);
  R r = read_code(join(sh_reg(mmSPI_SHADER_PGM_LO_VS), sh_reg(mmSPI_SHADER_PGM_HI_VS)) << 8, vs_code);
  if (r != R::Ok) return r;
  r = read_code(join(sh_reg(mmSPI_SHADER_PGM_LO_PS), sh_reg(mmSPI_SHADER_PGM_HI_PS)) << 8, ps_code);
  if (r != R::Ok) return r;
  call.vertex_code = ArrayView<const uint32_t>(vs_code.view().data(), vs_code.count());
  call.pixel_code = ArrayView<const uint32_t>(ps_code.view().data(), ps_code.count());
  if (field(sh_reg(mmSPI_SHADER_PGM_RSRC2_VS), SPI_SHADER_PGM_RSRC2_VS__USER_SGPR_MASK, SPI_SHADER_PGM_RSRC2_VS__USER_SGPR__SHIFT) != abi::kUserSgprs ||
      field(sh_reg(mmSPI_SHADER_PGM_RSRC2_PS), SPI_SHADER_PGM_RSRC2_PS__USER_SGPR_MASK, SPI_SHADER_PGM_RSRC2_PS__USER_SGPR__SHIFT) != abi::kUserSgprs)
    return R::Unsupported;
  call.vertex_info.fragment = false;
  call.vertex_info.param_exports = field(context_reg(mmSPI_VS_OUT_CONFIG), SPI_VS_OUT_CONFIG__VS_EXPORT_COUNT_MASK, SPI_VS_OUT_CONFIG__VS_EXPORT_COUNT__SHIFT) + 1;
  call.pixel_info.fragment = true;
  call.pixel_info.ps_inputs = field(context_reg(mmSPI_PS_IN_CONTROL), SPI_PS_IN_CONTROL__NUM_INTERP_MASK, SPI_PS_IN_CONTROL__NUM_INTERP__SHIFT);
  if (call.pixel_info.ps_inputs > 16) return R::Unsupported;
  for (uint32_t k = 0; k < call.pixel_info.ps_inputs; ++k) {
    const uint32_t cntl = context_reg(mmSPI_PS_INPUT_CNTL_0 + k);
    if ((cntl >> SPI_PS_INPUT_CNTL_0__FLAT_SHADE__SHIFT & 1) != 0) return R::Unsupported;
    call.ps_param[k] = static_cast<uint8_t>(field(cntl, SPI_PS_INPUT_CNTL_0__OFFSET_MASK, SPI_PS_INPUT_CNTL_0__OFFSET__SHIFT));
  }

  // user data: constants and the descriptor table
  const uint64_t vs_const = join(sh_reg(mmSPI_SHADER_USER_DATA_VS_0), sh_reg(mmSPI_SHADER_USER_DATA_VS_0 + 1));
  const uint64_t ps_const = join(sh_reg(mmSPI_SHADER_USER_DATA_PS_0), sh_reg(mmSPI_SHADER_USER_DATA_PS_0 + 1));
  const uint64_t vs_table = join(sh_reg(mmSPI_SHADER_USER_DATA_VS_0 + 2), sh_reg(mmSPI_SHADER_USER_DATA_VS_0 + 3));
  const uint64_t ps_table = join(sh_reg(mmSPI_SHADER_USER_DATA_PS_0 + 2), sh_reg(mmSPI_SHADER_USER_DATA_PS_0 + 3));
  if (vs_table != ps_table) return R::Unsupported;                       // (the harness has one table)
  List<float> vs_rows(allocator_), ps_rows(allocator_);
  for (int stage = 0; stage < 2; ++stage) {
    const uint64_t addr = stage ? ps_const : vs_const;
    List<float>& rows = stage ? ps_rows : vs_rows;
    if (addr == 0) continue;
    // the number of rows the shader reads is not in the registers: take as many as are mapped
    for (uint32_t n = kMaxConstRows; n > 0; n /= 2) {
      List<uint32_t> words(allocator_);
      if (!fill(words, n * 4ull)) return R::OutOfMemory;
      if (!memory_.read(addr, words.view())) continue;
      for (const uint32_t v : words.view())
        if (!rows.push_back(bits_float(v))) return R::OutOfMemory;
      break;
    }
  }
  call.vertex_constants = ArrayView<const float>(vs_rows.view().data(), vs_rows.count());
  call.pixel_constants = ArrayView<const float>(ps_rows.view().data(), ps_rows.count());
  call.vertex_info.const_rows = static_cast<uint32_t>(vs_rows.count() / 4);
  call.pixel_info.const_rows = static_cast<uint32_t>(ps_rows.count() / 4);

  // the vertex fetch: V# k of the table describes attribute k
  List<uint32_t> table(allocator_);
  if (!fill(table, abi::kTableBytes / 4)) return R::OutOfMemory;
  if (!memory_.read(vs_table, table.view())) return R::MemoryFault;
  uint64_t buffer_base = 0, buffer_end = 0;
  uint32_t stride = 0;
  bool first = true;
  for (uint32_t a = 0; a < abi::kMaxAttributes; ++a) {
    const uint32_t* v = &table[a * 4];
    if (v[2] == 0) continue;
    const uint32_t this_stride = v[1] >> 16 & 0x3FFF;
    const uint32_t dfmt = v[3] >> 15 & 0xF, nfmt = v[3] >> 12 & 7;
    uint32_t comps = 0;
    for (uint32_t c = 0; c < 4; ++c)
      if (dfmt == (c == 0 ? 4u : c == 1 ? 11u : c == 2 ? 13u : 14u)) comps = c + 1;
    if (comps == 0 || nfmt != 7 || this_stride == 0 || (this_stride & 3) != 0 || (v[0] & 3) != 0) return R::Unsupported;
    const uint64_t base = join(v[0], v[1] & 0xFFFF);
    const uint64_t end = base + static_cast<uint64_t>(this_stride) * (v[2] - 1) + comps * 4ull;          // (the last record's attribute ends the range)
    if (first) { stride = this_stride; buffer_base = base; buffer_end = end; first = false; }
    else {
      if (this_stride != stride) return R::Unsupported;
      if (base < buffer_base) buffer_base = base;
      if (end > buffer_end) buffer_end = end;
    }
    call.vertex_info.attributes_used |= 1u << a;
  }
  if (first) return R::Unsupported;
  if (buffer_end - buffer_base > kMaxBufferBytes) return R::Unsupported;
  for (uint32_t a = 0; a < abi::kMaxAttributes; ++a) {
    if (!(call.vertex_info.attributes_used >> a & 1)) continue;
    const uint32_t* v = &table[a * 4];
    const uint32_t dfmt = v[3] >> 15 & 0xF;
    call.attributes[a].components = dfmt == 4 ? 1 : dfmt == 11 ? 2 : dfmt == 13 ? 3 : 4;
    call.attributes[a].offset = static_cast<uint32_t>(join(v[0], v[1] & 0xFFFF) - buffer_base);
  }
  List<uint32_t> vertex_words(allocator_);
  if (!fill(vertex_words, (buffer_end - buffer_base) / 4)) return R::OutOfMemory;
  if (!memory_.read(buffer_base, vertex_words.view())) return R::MemoryFault;
  call.vertex_data = ArrayView<const uint32_t>(vertex_words.view().data(), vertex_words.count());
  call.vertex_stride = stride;

  // the indices
  List<uint32_t> indices(allocator_);
  if (!indices.reserve(index_count)) return R::OutOfMemory;
  if (!indexed) {
    for (uint32_t i = 0; i < index_count; ++i)
      if (!indices.push_back(i)) return R::OutOfMemory;
  } else {
    const uint64_t base = join(p.body[1], p.body[2]);
    const bool wide = index_type_ == kDI_INDEX_SIZE_32_BIT;
    const uint32_t max_size = p.body[0];
    if (index_count > max_size) return R::BadPacket;
    List<uint32_t> raw(allocator_);
    const uint64_t raw_words = wide ? index_count : (static_cast<uint64_t>(index_count) + 1) / 2;
    if (!fill(raw, raw_words)) return R::OutOfMemory;
    if (!memory_.read(base, raw.view())) return R::MemoryFault;
    for (uint32_t i = 0; i < index_count; ++i) {
      const uint32_t v = wide ? raw[i] : (raw[i / 2] >> ((i & 1) * 16)) & 0xFFFF;
      if (!indices.push_back(v)) return R::OutOfMemory;
    }
  }
  call.indices = ArrayView<const uint32_t>(indices.view().data(), indices.count());

  // the viewport and the scissors
  const uint32_t vte = context_reg(mmPA_CL_VTE_CNTL);
  const uint32_t need = (1u << PA_CL_VTE_CNTL__VPORT_X_SCALE_ENA__SHIFT) | (1u << PA_CL_VTE_CNTL__VPORT_X_OFFSET_ENA__SHIFT) | (1u << PA_CL_VTE_CNTL__VPORT_Y_SCALE_ENA__SHIFT) |
                        (1u << PA_CL_VTE_CNTL__VPORT_Y_OFFSET_ENA__SHIFT);
  if ((vte & need) != need || (vte >> PA_CL_VTE_CNTL__VTX_W0_FMT__SHIFT & 1) == 0 || (vte >> PA_CL_VTE_CNTL__VTX_XY_FMT__SHIFT & 1) != 0) return R::Unsupported;
  call.viewport.set = true;
  call.viewport.x_scale = bits_float(context_reg(mmPA_CL_VPORT_XSCALE));
  call.viewport.x_offset = bits_float(context_reg(mmPA_CL_VPORT_XOFFSET));
  call.viewport.y_scale = bits_float(context_reg(mmPA_CL_VPORT_YSCALE));
  call.viewport.y_offset = bits_float(context_reg(mmPA_CL_VPORT_YOFFSET));
  const uint32_t tl_regs[4] = {mmPA_SC_SCREEN_SCISSOR_TL, mmPA_SC_WINDOW_SCISSOR_TL, mmPA_SC_GENERIC_SCISSOR_TL, mmPA_SC_VPORT_SCISSOR_0_TL};
  const uint32_t br_regs[4] = {mmPA_SC_SCREEN_SCISSOR_BR, mmPA_SC_WINDOW_SCISSOR_BR, mmPA_SC_GENERIC_SCISSOR_BR, mmPA_SC_VPORT_SCISSOR_0_BR};
  call.scissor.set = true;
  call.scissor.left = 0;
  call.scissor.top = 0;
  call.scissor.right = width;
  call.scissor.bottom = height;
  for (int k = 0; k < 4; ++k) {
    const uint32_t tl = context_reg(tl_regs[k]), br = context_reg(br_regs[k]);
    const uint32_t l = tl & 0x7FFF, t = (tl >> 16) & 0x7FFF, rr = br & 0x7FFF, b = (br >> 16) & 0x7FFF;
    if (l > call.scissor.left) call.scissor.left = l;
    if (t > call.scissor.top) call.scissor.top = t;
    if (rr < call.scissor.right) call.scissor.right = rr;
    if (b < call.scissor.bottom) call.scissor.bottom = b;
  }

  // the target's current contents, drawn on, and written back
  Framebuffer fb(allocator_, width, height);
  if (!fb.valid()) return R::OutOfMemory;
  List<uint32_t> row(allocator_);
  if (!fill(row, static_cast<uint64_t>(width) * 4)) return R::OutOfMemory;
  for (uint32_t y = 0; y < height; ++y) {
    if (!memory_.read(color_base + static_cast<uint64_t>(y) * pitch * 16, row.view())) return R::MemoryFault;
    for (uint32_t x = 0; x < width; ++x)
      for (uint32_t c = 0; c < 4; ++c) fb.pixel(x, y)[c] = bits_float(row[x * 4 + c]);
  }
  call.textures = textures_;
  const DrawStatus st = gcn::draw(allocator_, call, fb, &last_stats_);
  if (st == DrawStatus::OutOfMemory) return R::OutOfMemory;
  if (st != DrawStatus::Ok) return st == DrawStatus::BadInput ? R::BadPacket : R::ShaderFault;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x)
      for (uint32_t c = 0; c < 4; ++c) {
        union { float f; uint32_t u; } cv;
        cv.f = fb.pixel(x, y)[c];
        row[x * 4 + c] = cv.u;
      }
    if (!memory_.write(color_base + static_cast<uint64_t>(y) * pitch * 16, row.view())) return R::MemoryFault;
  }
  ++draws_;
  return R::Ok;
}

CommandProcessorModel::Result CommandProcessorModel::execute(ArrayView<const uint32_t> stream) {
  Pm4Reader reader(stream);
  Pm4Packet p;
  while (reader.next(&p)) {
    Result r = Result::Ok;
    switch (p.opcode) {
      case kPacket3Nop: case kPacket3ContextControl: case kPacket3EventWrite: case kPacket3AcquireMem: case kPacket3SurfaceSync: case kPacket3IndexBase:
        break;
      case kPacket3SetShReg: r = set_registers(p, sh_, kShRegStart, kShRegEnd); break;
      case kPacket3SetContextReg: r = set_registers(p, context_, kContextRegStart, kContextRegEnd); break;
      case kPacket3SetUconfigReg: r = set_registers(p, uconfig_, kUconfigRegStart, kUconfigRegEnd); break;
      case kPacket3IndexType:
        if (p.body.size() != 1) r = Result::BadPacket; else index_type_ = p.body[0] & 3;
        break;
      case kPacket3NumInstances:
        if (p.body.size() != 1) r = Result::BadPacket; else num_instances_ = p.body[0];
        break;
      case kPacket3DrawIndexAuto: r = draw(false, p); break;
      case kPacket3DrawIndex2: r = draw(true, p); break;
      case kPacket3WriteData: {
        if (p.body.size() < 4) { r = Result::BadPacket; break; }
        const uint32_t dst_sel = p.body[0] >> 8 & 0xF;
        if (dst_sel != 1 && dst_sel != 5) { r = Result::Unsupported; break; }
        if (!memory_.write(join(p.body[1], p.body[2]), p.body.dropped(3))) r = Result::MemoryFault;
        break;
      }
      case kPacket3EventWriteEop: {
        if (p.body.size() != 5) { r = Result::BadPacket; break; }
        const uint32_t data_sel = p.body[2] >> 29 & 7;
        const uint64_t address = join(p.body[1], p.body[2] & 0xFFFF);
        if (data_sel == 0) break;
        if (data_sel != 1 && data_sel != 2) { r = Result::Unsupported; break; }
        const uint32_t data[2] = {p.body[3], p.body[4]};
        if (!memory_.write(address, ArrayView<const uint32_t>(data, data_sel == 2 ? 2 : 1))) r = Result::MemoryFault; else ++fence_writes_;
        break;
      }
      default: r = Result::Unsupported; break;
    }
    if (r != Result::Ok) return r;
  }
  return reader.error() ? Result::BadPacket : Result::Ok;
}

}  // namespace gcn
