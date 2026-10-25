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

// Host-side tests for the software WebGPU device: render pipelines, compute pipelines with barriers, copies.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "WebGl/GpuDevice.hpp"

using namespace wgpu;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static void test_render() {
  TestAllocator a;
  {
    Device dev(a);
    const FormatDesc* rgba = find_format("rgba8unorm");
    const uint32_t tex = dev.create_texture(16, 16, 1, 1, 1, 1, rgba, 0x10 | 0x01);
    CHECK(tex != 0);
    TextureView tv;
    tv.texture = tex; tv.format = rgba; tv.dim = 1;
    const uint32_t view = dev.create_view(tv);
    String log(a);
    const char* wgsl =
        "struct VsOut { @builtin(position) pos: vec4<f32>, @location(0) color: vec3<f32>, }\n"
        "@vertex fn vs(@builtin(vertex_index) i: u32, @location(0) p: vec2<f32>, @location(1) c: vec3<f32>) -> VsOut { var o: VsOut; o.pos = vec4<f32>(p, 0.0, 1.0); o.color = c; return o; }\n"
        "@fragment fn fs(in: VsOut) -> @location(0) vec4<f32> { return vec4<f32>(in.color, 1.0); }\n";
    const uint32_t mod = dev.create_shader_module(wgsl, &log);
    CHECK(mod != 0 && log.empty());
    const float verts[] = {-1, -1, 1, 0, 0, 3, -1, 1, 0, 0, -1, 3, 1, 0, 0};   // one big red triangle covering the target
    const uint32_t vb = dev.create_buffer(sizeof(verts), 0x20 | 0x08);
    CHECK(dev.write_buffer(vb, 0, ConstByteView(reinterpret_cast<const uint8_t*>(verts), sizeof(verts))));
    List<UniquePointer<VertexBufferLayout>> buffers(a);
    UniquePointer<VertexBufferLayout> layout = UniquePointer<VertexBufferLayout>::make(a, a);
    layout->stride = 20;
    VertexAttribute a0, a1;
    a0.shader_location = 0; a0.offset = 0; a0.format = vertex_format("float32x2");
    a1.shader_location = 1; a1.offset = 8; a1.format = vertex_format("float32x3");
    layout->attributes.push_back(a0);
    layout->attributes.push_back(a1);
    buffers.push_back(static_cast<UniquePointer<VertexBufferLayout>&&>(layout));
    List<ColorTarget> targets(a);
    ColorTarget ct;
    ct.format = rgba;
    targets.push_back(ct);
    List<UniquePointer<Device::ConstantValue>> none(a);
    const uint32_t rp = dev.create_render_pipeline(0, mod, "vs", mod, "fs", buffers, targets, nullptr, 3, 0, true, 0, false, 1, none, none);
    CHECK(rp != 0);
    if (!rp) printf("error: %s\n", dev.error.c_str());
    RenderPassDesc pd(a);
    ColorAttachment ca;
    ca.view = view;
    ca.clear[2] = 1; ca.clear[3] = 1;
    pd.colors.push_back(ca);
    CHECK(dev.begin_render_pass(pd));
    CHECK(dev.set_pipeline(rp));
    CHECK(dev.set_vertex_buffer(0, vb, 0, 0));
    CHECK(dev.draw(3, 1, 0, 0));
    CHECK(dev.end_render_pass());
    if (!dev.error.empty()) printf("error: %s\n", dev.error.c_str());
    const uint32_t out = dev.create_buffer(16 * 16 * 4, 0x08 | 0x02);
    ImageCopy src;
    src.texture = tex;
    BufferLayout bl;
    bl.bytes_per_row = 64;
    CHECK(dev.copy_texture_to_buffer(src, out, bl, 16, 16, 1));
    uint8_t px[16 * 16 * 4];
    CHECK(dev.read_buffer(out, 0, ByteView(px, sizeof(px))));
    CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255);
    CHECK(px[(8 * 16 + 8) * 4] == 255);
    // a cleared (blue) target where nothing is drawn: draw nothing
    CHECK(dev.begin_render_pass(pd));
    CHECK(dev.end_render_pass());
    CHECK(dev.copy_texture_to_buffer(src, out, bl, 16, 16, 1));
    CHECK(dev.read_buffer(out, 0, ByteView(px, sizeof(px))));
    CHECK(px[0] == 0 && px[2] == 255 && px[3] == 255);
  }
  CHECK(a.live == 0);
}

static void test_compute() {
  TestAllocator a;
  {
    Device dev(a);
    String log(a);
    const char* wgsl =
        "@group(0) @binding(0) var<storage, read_write> data: array<u32>;\n"
        "@group(0) @binding(1) var<uniform> params: vec4<u32>;\n"
        "var<workgroup> tile: array<u32, 8>;\n"
        "@compute @workgroup_size(8) fn main(@builtin(local_invocation_id) lid: vec3<u32>, @builtin(workgroup_id) wid: vec3<u32>, @builtin(global_invocation_id) gid: vec3<u32>) {\n"
        "  tile[lid.x] = gid.x * params.x;\n"
        "  workgroupBarrier();\n"
        "  // after the barrier every invocation can see its neighbour's value\n"
        "  let other = tile[(lid.x + 1u) % 8u];\n"
        "  data[gid.x] = tile[lid.x] + other + arrayLength(&data);\n"
        "}\n";
    const uint32_t mod = dev.create_shader_module(wgsl, &log);
    CHECK(mod != 0);
    if (!log.empty()) printf("wgsl: %s\n", log.c_str());
    const uint32_t data = dev.create_buffer(16 * 4, 0x80 | 0x04 | 0x08);
    const uint32_t params = dev.create_buffer(16, 0x40 | 0x08);
    const uint32_t pv[4] = {10, 0, 0, 0};
    dev.write_buffer(params, 0, ConstByteView(reinterpret_cast<const uint8_t*>(pv), 16));
    List<UniquePointer<Device::ConstantValue>> none(a);
    const uint32_t cp = dev.create_compute_pipeline(0, mod, "main", none);
    CHECK(cp != 0);
    if (!cp) printf("error: %s\n", dev.error.c_str());
    const uint32_t bgl = dev.pipeline_layout_bgl(cp, true, 0);
    CHECK(bgl != 0);
    List<BgEntry> entries(a);
    BgEntry e0, e1;
    e0.binding = 0; e0.kind = 0; e0.id = data;
    e1.binding = 1; e1.kind = 0; e1.id = params;
    entries.push_back(e0);
    entries.push_back(e1);
    const uint32_t bg = dev.create_bind_group(bgl, entries);
    CHECK(bg != 0);
    CHECK(dev.begin_compute_pass());
    CHECK(dev.set_pipeline(cp));
    CHECK(dev.set_bind_group(0, bg, ArrayView<const uint32_t>()));
    CHECK(dev.dispatch(2, 1, 1));
    CHECK(dev.end_compute_pass());
    if (!dev.error.empty()) printf("error: %s\n", dev.error.c_str());
    uint32_t out[16];
    CHECK(dev.read_buffer(data, 0, ByteView(reinterpret_cast<uint8_t*>(out), 64)));
    // invocation i of workgroup 0: tile[i] = i * 10, other = ((i + 1) % 8) * 10, plus the array length (16)
    CHECK(out[0] == 0 + 10 + 16);
    CHECK(out[3] == 30 + 40 + 16);
    CHECK(out[7] == 70 + 0 + 16);
    // workgroup 1: gid = 8 + i
    CHECK(out[8] == 80 + 90 + 16);
    CHECK(out[15] == 150 + 80 + 16);
  }
  CHECK(a.live == 0);
}

static void test_multisample() {
  TestAllocator a;
  {
    Device dev(a);
    const FormatDesc* rgba = find_format("rgba8unorm");
    const uint32_t ms = dev.create_texture(16, 16, 1, 1, 4, 1, rgba, 0x10), one = dev.create_texture(16, 16, 1, 1, 1, 1, rgba, 0x10 | 0x01);
    TextureView mv, ov;
    mv.texture = ms; mv.format = rgba;
    ov.texture = one; ov.format = rgba;
    const uint32_t msv = dev.create_view(mv), ov_id = dev.create_view(ov);
    String log(a);
    const uint32_t mod = dev.create_shader_module(
        "@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> { var p = array<vec2<f32>, 3>(vec2(-1.0,-1.0), vec2(1.0,-1.0), vec2(1.0,1.0)); return vec4(p[i], 0.0, 1.0); }\n"
        "@fragment fn fs() -> @location(0) vec4<f32> { return vec4(1.0); }", &log);
    List<UniquePointer<VertexBufferLayout>> buffers(a);
    List<ColorTarget> targets(a);
    ColorTarget ct;
    ct.format = rgba;
    targets.push_back(ct);
    List<UniquePointer<Device::ConstantValue>> none(a);
    const uint32_t rp = dev.create_render_pipeline(0, mod, "vs", mod, "fs", buffers, targets, nullptr, 3, 0, true, 0, false, 4, none, none);
    CHECK(rp != 0);
    RenderPassDesc pd(a);
    ColorAttachment ca;
    ca.view = msv; ca.resolve = ov_id; ca.clear[3] = 1;
    pd.colors.push_back(ca);
    CHECK(dev.begin_render_pass(pd));
    CHECK(dev.set_pipeline(rp));
    CHECK(dev.draw(3, 1, 0, 0));
    CHECK(dev.end_render_pass());
    const uint32_t out = dev.create_buffer(16 * 16 * 4, 0x08 | 0x02);
    ImageCopy src;
    src.texture = one;
    BufferLayout bl;
    bl.bytes_per_row = 64;
    CHECK(dev.copy_texture_to_buffer(src, out, bl, 16, 16, 1));
    uint8_t px[16 * 16 * 4];
    CHECK(dev.read_buffer(out, 0, ByteView(px, sizeof(px))));
    // the diagonal runs through the pixels on it: those are partly covered
    const uint8_t edge = px[(8 * 16 + 7) * 4];   // row 8 (from the top), column 7: on the diagonal
    CHECK(edge > 60 && edge < 200);
    CHECK(px[(12 * 16 + 12) * 4] == 255 && px[(3 * 16 + 3) * 4] == 0);
    ImageCopy bad;
    bad.texture = ms;
    CHECK(!dev.copy_texture_to_buffer(bad, out, bl, 16, 16, 1));
  }
  CHECK(a.live == 0);
}

static void test_barrier_in_function() {
  TestAllocator a;
  {
    Device dev(a);
    String log(a);
    // a tree reduction whose steps (with the barrier) are in a function
    const char* wgsl =
        "@group(0) @binding(0) var<storage, read_write> data: array<u32>;\n"
        "var<workgroup> tile: array<u32, 8>;\n"
        "fn step(i: u32, stride: u32) { if (i < stride) { tile[i] = tile[i] + tile[i + stride]; } workgroupBarrier(); }\n"
        "fn reduce(i: u32) { step(i, 4u); step(i, 2u); step(i, 1u); }\n"
        "@compute @workgroup_size(8) fn main(@builtin(local_invocation_id) l: vec3<u32>) {\n"
        "  tile[l.x] = data[l.x];\n"
        "  workgroupBarrier();\n"
        "  reduce(l.x);\n"
        "  data[l.x] = tile[0];\n"
        "}\n";
    const uint32_t mod = dev.create_shader_module(wgsl, &log);
    CHECK(mod != 0 && log.empty());
    const uint32_t data = dev.create_buffer(32, 0x80 | 0x04 | 0x08);
    const uint32_t in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    dev.write_buffer(data, 0, ConstByteView(reinterpret_cast<const uint8_t*>(in), 32));
    List<UniquePointer<Device::ConstantValue>> none(a);
    const uint32_t cp = dev.create_compute_pipeline(0, mod, "main", none);
    CHECK(cp != 0);
    List<BgEntry> entries(a);
    BgEntry e0;
    e0.binding = 0; e0.id = data;
    entries.push_back(e0);
    const uint32_t bg = dev.create_bind_group(dev.pipeline_layout_bgl(cp, true, 0), entries);
    CHECK(dev.begin_compute_pass() && dev.set_pipeline(cp) && dev.set_bind_group(0, bg, ArrayView<const uint32_t>()) && dev.dispatch(1, 1, 1) && dev.end_compute_pass());
    uint32_t out[8];
    CHECK(dev.read_buffer(data, 0, ByteView(reinterpret_cast<uint8_t*>(out), 32)));
    for (int i = 0; i < 8; ++i) CHECK(out[i] == 36);
  }
  CHECK(a.live == 0);
}

static void test_compressed() {
  TestAllocator a;
  {
    Device dev(a);
    const FormatDesc* bc1 = find_format("bc1-rgba-unorm");
    const FormatDesc* astc = find_format("astc-6x6-unorm");
    CHECK(bc1 && astc && find_format("bc7-rgba-unorm-srgb") && find_format("astc-12x10-unorm-srgb") && find_format("bc6h-rgb-float"));
    CHECK(dev.create_texture(5, 8, 1, 1, 1, 1, bc1, 0x04) == 0);          // not a multiple of the block size
    CHECK(dev.create_texture(8, 8, 1, 1, 1, 1, bc1, 0x10 | 0x04) == 0);   // not renderable
    CHECK(dev.create_texture(8, 8, 1, 1, 4, 1, bc1, 0x04) == 0);          // not multisampled
    dev.error.clear();
    const uint32_t tex = dev.create_texture(8, 8, 1, 2, 1, 1, bc1, 0x04 | 0x02 | 0x01), at = dev.create_texture(12, 6, 1, 1, 1, 1, astc, 0x04 | 0x02 | 0x01);
    CHECK(tex && at);
    // BC1: red and blue end points; the first row of indices is 0 1 2 3
    const uint8_t blk[8] = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0x00, 0x00, 0x00};
    uint8_t two[16];
    memcpy(two, blk, 8); memcpy(two + 8, blk, 8);
    ImageCopy dst;
    dst.texture = tex; dst.x = 4;
    BufferLayout bl;
    CHECK(dev.write_texture(dst, ConstByteView(two, 8), bl, 4, 4, 1));
    const Texture* t = dev.texture(tex);
    uint32_t px[4];
    webgl::read_texel(*t->mips[0], 4, 0, 0, px); CHECK(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255);
    webgl::read_texel(*t->mips[0], 5, 0, 0, px); CHECK(px[0] == 0 && px[2] == 255);
    webgl::read_texel(*t->mips[0], 0, 0, 0, px); CHECK(px[0] == 0 && px[2] == 0);   // not written
    // a row of two blocks with padding, and its raw read back
    dst.x = 0;
    bl.bytes_per_row = 20;
    CHECK(!dev.write_texture(dst, ConstByteView(two, 16), bl, 8, 8, 1));   // data too small
    uint8_t rows[36] = {};
    memcpy(rows, two, 16);
    memcpy(rows + 20, two, 16);
    CHECK(dev.write_texture(dst, ConstByteView(rows, 36), bl, 8, 8, 1));
    const uint32_t out = dev.create_buffer(32, 0x08 | 0x02);
    BufferLayout rb;
    rb.bytes_per_row = 16;
    ImageCopy src;
    src.texture = tex;
    CHECK(dev.copy_texture_to_buffer(src, out, rb, 8, 8, 1));
    uint8_t back[32];
    CHECK(dev.read_buffer(out, 0, ByteView(back, 32)));
    CHECK(memcmp(back, two, 16) == 0 && memcmp(back + 16, rows + 20, 16) == 0);
    // alignment and bounds
    dst.x = 2;
    CHECK(!dev.write_texture(dst, ConstByteView(two, 16), BufferLayout(), 4, 4, 1));
    dst.x = 0;
    CHECK(!dev.write_texture(dst, ConstByteView(two, 16), BufferLayout(), 3, 4, 1));
    // texture to texture copies the blocks (and decodes them)
    const uint32_t other = dev.create_texture(8, 8, 1, 1, 1, 1, bc1, 0x04 | 0x02);
    ImageCopy to;
    to.texture = other; to.x = 4; to.y = 4;
    src.x = 4; src.y = 0;
    CHECK(dev.copy_texture_to_texture(src, to, 4, 4, 1));
    webgl::read_texel(*dev.texture(other)->mips[0], 5, 4, 0, px); CHECK(px[2] == 255);
    CHECK(!dev.copy_texture_to_texture(src, ImageCopy{at, 0, 0, 0, 0, 0}, 4, 4, 1));   // formats differ
    // ASTC 6x6: a void-extent block, 12 x 6 texels = two blocks, the second one with another colour
    uint8_t ab[32] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff};
    memcpy(ab + 16, ab, 16);
    ab[16 + 9] = 0x00; ab[16 + 11] = 0xff;
    ImageCopy ad;
    ad.texture = at;
    CHECK(dev.write_texture(ad, ConstByteView(ab, 32), BufferLayout(), 12, 6, 1));
    webgl::read_texel(*dev.texture(at)->mips[0], 3, 3, 0, px); CHECK(px[0] == 255 && px[1] == 0 && px[3] == 255);
    webgl::read_texel(*dev.texture(at)->mips[0], 9, 3, 0, px); CHECK(px[0] == 0 && px[1] == 255);
    // mip 1 of the BC1 texture is 4 x 4 texels (one block)
    ImageCopy m1;
    m1.texture = tex; m1.mip = 1;
    CHECK(dev.write_texture(m1, ConstByteView(blk, 8), BufferLayout(), 4, 4, 1));
  }
  CHECK(a.live == 0);
}

static void test_rg11b10() {
  TestAllocator a;
  {
    Device dev(a);
    const FormatDesc* f = find_format("rg11b10ufloat");
    CHECK(f != nullptr);
    const uint32_t tex = dev.create_texture(4, 1, 1, 1, 1, 1, f, 0x04 | 0x02 | 0x01);
    CHECK(tex != 0);
    auto pack = [](uint32_t r, uint32_t g, uint32_t b) { return r | (g << 11) | (b << 22); };
    // 1.0, 0.5, 2.0 / the smallest denormals / infinity and NaN / the largest finite values
    const uint32_t texels[4] = {pack(15 << 6, 14 << 6, 16 << 5), pack(1, 1, 1), pack(31 << 6, 31 << 6 | 1, 31 << 5), pack(1983, 1983, 991)};
    ImageCopy c;
    c.texture = tex;
    CHECK(dev.write_texture(c, ConstByteView(reinterpret_cast<const uint8_t*>(texels), 16), BufferLayout(), 4, 1, 1));
    const Image& im = *dev.texture(tex)->mips[0];
    uint32_t px[4];
    auto fl = [](uint32_t u) { return __builtin_bit_cast(float, u); };
    webgl::read_texel(im, 0, 0, 0, px); CHECK(fl(px[0]) == 1.0f && fl(px[1]) == 0.5f && fl(px[2]) == 2.0f);
    webgl::read_texel(im, 1, 0, 0, px); CHECK(fl(px[0]) == 1.0f / 64 / 16384 && fl(px[2]) == 1.0f / 32 / 16384);
    webgl::read_texel(im, 2, 0, 0, px); CHECK(fl(px[0]) > 1e38f && fl(px[1]) != fl(px[1]) && fl(px[2]) > 1e38f);
    webgl::read_texel(im, 3, 0, 0, px); CHECK(fl(px[0]) == 65024.0f && fl(px[2]) == 64512.0f);
    // reading back encodes again: exact values round-trip; others round to nearest even, negatives to zero, overflow to the largest finite value
    const uint32_t out = dev.create_buffer(16, 0x08 | 0x02);
    CHECK(dev.copy_texture_to_buffer(c, out, BufferLayout(), 4, 1, 1));
    uint32_t back[4];
    CHECK(dev.read_buffer(out, 0, ByteView(reinterpret_cast<uint8_t*>(back), 16)));
    CHECK(back[0] == texels[0] && back[1] == texels[1] && back[3] == texels[3] && back[2] >> 22 == 31 << 5);
    Image& wm = *dev.texture_mut(tex)->mips[0];
    const float vals[4] = {1.0f + 1.0f / 128, -3.0f, 1e9f, 3.14159f};   // 1 + 2^-7 is a tie between 1 and 1 + 2^-6 for an 11-bit float: rounds to even (1.0)
    uint32_t lanes[4] = {__builtin_bit_cast(uint32_t, vals[0]), __builtin_bit_cast(uint32_t, vals[1]), __builtin_bit_cast(uint32_t, vals[2]), __builtin_bit_cast(uint32_t, 1.0f)};
    webgl::write_texel(wm, 0, 0, 0, lanes);
    lanes[0] = __builtin_bit_cast(uint32_t, vals[3]); lanes[1] = lanes[0]; lanes[2] = lanes[0];
    webgl::write_texel(wm, 1, 0, 0, lanes);
    CHECK(dev.copy_texture_to_buffer(c, out, BufferLayout(), 4, 1, 1));
    CHECK(dev.read_buffer(out, 0, ByteView(reinterpret_cast<uint8_t*>(back), 16)));
    CHECK((back[0] & 2047) == 15 << 6 && ((back[0] >> 11) & 2047) == 0 && (back[0] >> 22) == 991);
    // pi: 1.5707963 * 2^1 -> mantissa 0.5707963 * 64 = 36.53 -> 37 (11 bit), * 32 = 18.26 -> 18 (10 bit)
    CHECK((back[1] & 2047) == ((16 << 6) | 37) && (back[1] >> 22) == ((16 << 5) | 18));
  }
  CHECK(a.live == 0);
}

int main() {
  test_rg11b10();
  test_compressed();
  test_barrier_in_function();
  test_multisample();
  test_render();
  test_compute();
  if (g_failed) { printf("%d checks failed\n", g_failed); return 1; }
  printf("gpu tests passed\n");
  return 0;
}
