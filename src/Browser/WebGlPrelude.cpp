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

// The JavaScript side of WebGL: WebGLRenderingContext, WebGL2RenderingContext, the object types and the extensions, over the natives in DomBindingsGl.cpp.
#include "Browser/WebPrelude.hpp"

namespace browser {

namespace {

const char kWebGlPrelude[] = R"JSPRELUDE((function (g) {
  if (typeof __gl_create !== 'function' || g.WebGLRenderingContext !== undefined) return;
  var OPS = __gl_ops(), OP = {};
  for (var oi = 0; oi < OPS.length; oi++) OP[OPS[oi]] = oi;
  var call = __gl;
  var hide = function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true, configurable: true }); };
  var F = 102, I = 105, U = 117;   // 'f' 'i' 'u'

  // ------------------------------------------------------------------------------------------------------------------------------------------ constants
  var C1 = {
    DEPTH_BUFFER_BIT: 0x100, STENCIL_BUFFER_BIT: 0x400, COLOR_BUFFER_BIT: 0x4000, POINTS: 0, LINES: 1, LINE_LOOP: 2, LINE_STRIP: 3, TRIANGLES: 4, TRIANGLE_STRIP: 5, TRIANGLE_FAN: 6,
    ZERO: 0, ONE: 1, SRC_COLOR: 0x300, ONE_MINUS_SRC_COLOR: 0x301, SRC_ALPHA: 0x302, ONE_MINUS_SRC_ALPHA: 0x303, DST_ALPHA: 0x304, ONE_MINUS_DST_ALPHA: 0x305, DST_COLOR: 0x306,
    ONE_MINUS_DST_COLOR: 0x307, SRC_ALPHA_SATURATE: 0x308, FUNC_ADD: 0x8006, BLEND_EQUATION: 0x8009, BLEND_EQUATION_RGB: 0x8009, BLEND_EQUATION_ALPHA: 0x883D, FUNC_SUBTRACT: 0x800A,
    FUNC_REVERSE_SUBTRACT: 0x800B, BLEND_DST_RGB: 0x80C8, BLEND_SRC_RGB: 0x80C9, BLEND_DST_ALPHA: 0x80CA, BLEND_SRC_ALPHA: 0x80CB, CONSTANT_COLOR: 0x8001, ONE_MINUS_CONSTANT_COLOR: 0x8002,
    CONSTANT_ALPHA: 0x8003, ONE_MINUS_CONSTANT_ALPHA: 0x8004, BLEND_COLOR: 0x8005, ARRAY_BUFFER: 0x8892, ELEMENT_ARRAY_BUFFER: 0x8893, ARRAY_BUFFER_BINDING: 0x8894,
    ELEMENT_ARRAY_BUFFER_BINDING: 0x8895, STREAM_DRAW: 0x88E0, STATIC_DRAW: 0x88E4, DYNAMIC_DRAW: 0x88E8, BUFFER_SIZE: 0x8764, BUFFER_USAGE: 0x8765, CURRENT_VERTEX_ATTRIB: 0x8626,
    FRONT: 0x404, BACK: 0x405, FRONT_AND_BACK: 0x408, TEXTURE_2D: 0x0DE1, CULL_FACE: 0x0B44, BLEND: 0x0BE2, DITHER: 0x0BD0, STENCIL_TEST: 0x0B90, DEPTH_TEST: 0x0B71, SCISSOR_TEST: 0x0C11,
    POLYGON_OFFSET_FILL: 0x8037, SAMPLE_ALPHA_TO_COVERAGE: 0x809E, SAMPLE_COVERAGE: 0x80A0, NO_ERROR: 0, INVALID_ENUM: 0x500, INVALID_VALUE: 0x501, INVALID_OPERATION: 0x502,
    OUT_OF_MEMORY: 0x505, CW: 0x900, CCW: 0x901, LINE_WIDTH: 0x0B21, ALIASED_POINT_SIZE_RANGE: 0x846D, ALIASED_LINE_WIDTH_RANGE: 0x846E, CULL_FACE_MODE: 0x0B45, FRONT_FACE: 0x0B46,
    DEPTH_RANGE: 0x0B70, DEPTH_WRITEMASK: 0x0B72, DEPTH_CLEAR_VALUE: 0x0B73, DEPTH_FUNC: 0x0B74, STENCIL_CLEAR_VALUE: 0x0B91, STENCIL_FUNC: 0x0B92, STENCIL_FAIL: 0x0B94,
    STENCIL_PASS_DEPTH_FAIL: 0x0B95, STENCIL_PASS_DEPTH_PASS: 0x0B96, STENCIL_REF: 0x0B97, STENCIL_VALUE_MASK: 0x0B93, STENCIL_WRITEMASK: 0x0B98, STENCIL_BACK_FUNC: 0x8800,
    STENCIL_BACK_FAIL: 0x8801, STENCIL_BACK_PASS_DEPTH_FAIL: 0x8802, STENCIL_BACK_PASS_DEPTH_PASS: 0x8803, STENCIL_BACK_REF: 0x8CA3, STENCIL_BACK_VALUE_MASK: 0x8CA4,
    STENCIL_BACK_WRITEMASK: 0x8CA5, VIEWPORT: 0x0BA2, SCISSOR_BOX: 0x0C10, COLOR_CLEAR_VALUE: 0x0C22, COLOR_WRITEMASK: 0x0C23, UNPACK_ALIGNMENT: 0x0CF5, PACK_ALIGNMENT: 0x0D05,
    MAX_TEXTURE_SIZE: 0x0D33, MAX_VIEWPORT_DIMS: 0x0D3A, SUBPIXEL_BITS: 0x0D50, RED_BITS: 0x0D52, GREEN_BITS: 0x0D53, BLUE_BITS: 0x0D54, ALPHA_BITS: 0x0D55, DEPTH_BITS: 0x0D56,
    STENCIL_BITS: 0x0D57, POLYGON_OFFSET_UNITS: 0x2A00, POLYGON_OFFSET_FACTOR: 0x8038, TEXTURE_BINDING_2D: 0x8069, SAMPLE_BUFFERS: 0x80A8, SAMPLES: 0x80A9, SAMPLE_COVERAGE_VALUE: 0x80AA,
    SAMPLE_COVERAGE_INVERT: 0x80AB, COMPRESSED_TEXTURE_FORMATS: 0x86A3, DONT_CARE: 0x1100, FASTEST: 0x1101, NICEST: 0x1102, GENERATE_MIPMAP_HINT: 0x8192, BYTE: 0x1400,
    UNSIGNED_BYTE: 0x1401, SHORT: 0x1402, UNSIGNED_SHORT: 0x1403, INT: 0x1404, UNSIGNED_INT: 0x1405, FLOAT: 0x1406, DEPTH_COMPONENT: 0x1902, ALPHA: 0x1906, RGB: 0x1907, RGBA: 0x1908,
    LUMINANCE: 0x1909, LUMINANCE_ALPHA: 0x190A, UNSIGNED_SHORT_4_4_4_4: 0x8033, UNSIGNED_SHORT_5_5_5_1: 0x8034, UNSIGNED_SHORT_5_6_5: 0x8363, FRAGMENT_SHADER: 0x8B30,
    VERTEX_SHADER: 0x8B31, MAX_VERTEX_ATTRIBS: 0x8869, MAX_VERTEX_UNIFORM_VECTORS: 0x8DFB, MAX_VARYING_VECTORS: 0x8DFC, MAX_COMBINED_TEXTURE_IMAGE_UNITS: 0x8B4D,
    MAX_VERTEX_TEXTURE_IMAGE_UNITS: 0x8B4C, MAX_TEXTURE_IMAGE_UNITS: 0x8872, MAX_FRAGMENT_UNIFORM_VECTORS: 0x8DFD, SHADER_TYPE: 0x8B4F, DELETE_STATUS: 0x8B80, LINK_STATUS: 0x8B82,
    VALIDATE_STATUS: 0x8B83, ATTACHED_SHADERS: 0x8B85, ACTIVE_UNIFORMS: 0x8B86, ACTIVE_ATTRIBUTES: 0x8B89, SHADING_LANGUAGE_VERSION: 0x8B8C, CURRENT_PROGRAM: 0x8B8D, NEVER: 0x200,
    LESS: 0x201, EQUAL: 0x202, LEQUAL: 0x203, GREATER: 0x204, NOTEQUAL: 0x205, GEQUAL: 0x206, ALWAYS: 0x207, KEEP: 0x1E00, REPLACE: 0x1E01, INCR: 0x1E02, DECR: 0x1E03, INVERT: 0x150A,
    INCR_WRAP: 0x8507, DECR_WRAP: 0x8508, VENDOR: 0x1F00, RENDERER: 0x1F01, VERSION: 0x1F02, NEAREST: 0x2600, LINEAR: 0x2601, NEAREST_MIPMAP_NEAREST: 0x2700,
    LINEAR_MIPMAP_NEAREST: 0x2701, NEAREST_MIPMAP_LINEAR: 0x2702, LINEAR_MIPMAP_LINEAR: 0x2703, TEXTURE_MAG_FILTER: 0x2800, TEXTURE_MIN_FILTER: 0x2801, TEXTURE_WRAP_S: 0x2802,
    TEXTURE_WRAP_T: 0x2803, TEXTURE: 0x1702, TEXTURE_CUBE_MAP: 0x8513, TEXTURE_BINDING_CUBE_MAP: 0x8514, TEXTURE_CUBE_MAP_POSITIVE_X: 0x8515, TEXTURE_CUBE_MAP_NEGATIVE_X: 0x8516,
    TEXTURE_CUBE_MAP_POSITIVE_Y: 0x8517, TEXTURE_CUBE_MAP_NEGATIVE_Y: 0x8518, TEXTURE_CUBE_MAP_POSITIVE_Z: 0x8519, TEXTURE_CUBE_MAP_NEGATIVE_Z: 0x851A, MAX_CUBE_MAP_TEXTURE_SIZE: 0x851C,
    ACTIVE_TEXTURE: 0x84E0, REPEAT: 0x2901, CLAMP_TO_EDGE: 0x812F, MIRRORED_REPEAT: 0x8370, FLOAT_VEC2: 0x8B50, FLOAT_VEC3: 0x8B51, FLOAT_VEC4: 0x8B52, INT_VEC2: 0x8B53,
    INT_VEC3: 0x8B54, INT_VEC4: 0x8B55, BOOL: 0x8B56, BOOL_VEC2: 0x8B57, BOOL_VEC3: 0x8B58, BOOL_VEC4: 0x8B59, FLOAT_MAT2: 0x8B5A, FLOAT_MAT3: 0x8B5B, FLOAT_MAT4: 0x8B5C,
    SAMPLER_2D: 0x8B5E, SAMPLER_CUBE: 0x8B60, VERTEX_ATTRIB_ARRAY_ENABLED: 0x8622, VERTEX_ATTRIB_ARRAY_SIZE: 0x8623, VERTEX_ATTRIB_ARRAY_STRIDE: 0x8624,
    VERTEX_ATTRIB_ARRAY_TYPE: 0x8625, VERTEX_ATTRIB_ARRAY_NORMALIZED: 0x886A, VERTEX_ATTRIB_ARRAY_POINTER: 0x8645, VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: 0x889F,
    IMPLEMENTATION_COLOR_READ_TYPE: 0x8B9A, IMPLEMENTATION_COLOR_READ_FORMAT: 0x8B9B, COMPILE_STATUS: 0x8B81, LOW_FLOAT: 0x8DF0, MEDIUM_FLOAT: 0x8DF1, HIGH_FLOAT: 0x8DF2,
    LOW_INT: 0x8DF3, MEDIUM_INT: 0x8DF4, HIGH_INT: 0x8DF5, FRAMEBUFFER: 0x8D40, RENDERBUFFER: 0x8D41, RGBA4: 0x8056, RGB5_A1: 0x8057, RGB565: 0x8D62, DEPTH_COMPONENT16: 0x81A5,
    STENCIL_INDEX8: 0x8D48, DEPTH_STENCIL: 0x84F9, RENDERBUFFER_WIDTH: 0x8D42, RENDERBUFFER_HEIGHT: 0x8D43, RENDERBUFFER_INTERNAL_FORMAT: 0x8D44, RENDERBUFFER_RED_SIZE: 0x8D50,
    RENDERBUFFER_GREEN_SIZE: 0x8D51, RENDERBUFFER_BLUE_SIZE: 0x8D52, RENDERBUFFER_ALPHA_SIZE: 0x8D53, RENDERBUFFER_DEPTH_SIZE: 0x8D54, RENDERBUFFER_STENCIL_SIZE: 0x8D55,
    FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE: 0x8CD0, FRAMEBUFFER_ATTACHMENT_OBJECT_NAME: 0x8CD1, FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL: 0x8CD2,
    FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE: 0x8CD3, COLOR_ATTACHMENT0: 0x8CE0, DEPTH_ATTACHMENT: 0x8D00, STENCIL_ATTACHMENT: 0x8D20, DEPTH_STENCIL_ATTACHMENT: 0x821A, NONE: 0,
    FRAMEBUFFER_COMPLETE: 0x8CD5, FRAMEBUFFER_INCOMPLETE_ATTACHMENT: 0x8CD6, FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: 0x8CD7, FRAMEBUFFER_INCOMPLETE_DIMENSIONS: 0x8CD9,
    FRAMEBUFFER_UNSUPPORTED: 0x8CDD, FRAMEBUFFER_BINDING: 0x8CA6, RENDERBUFFER_BINDING: 0x8CA7, MAX_RENDERBUFFER_SIZE: 0x84E8, INVALID_FRAMEBUFFER_OPERATION: 0x506,
    UNPACK_FLIP_Y_WEBGL: 0x9240, UNPACK_PREMULTIPLY_ALPHA_WEBGL: 0x9241, CONTEXT_LOST_WEBGL: 0x9242, UNPACK_COLORSPACE_CONVERSION_WEBGL: 0x9243, BROWSER_DEFAULT_WEBGL: 0x9244
  };
  var C2 = {
    READ_BUFFER: 0x0C02, UNPACK_ROW_LENGTH: 0x0CF2, UNPACK_SKIP_ROWS: 0x0CF3, UNPACK_SKIP_PIXELS: 0x0CF4, PACK_ROW_LENGTH: 0x0D02, PACK_SKIP_ROWS: 0x0D03, PACK_SKIP_PIXELS: 0x0D04,
    COLOR: 0x1800, DEPTH: 0x1801, STENCIL: 0x1802, RED: 0x1903, RGB8: 0x8051, RGBA8: 0x8058, RGB10_A2: 0x8059, TEXTURE_BINDING_3D: 0x806A, UNPACK_SKIP_IMAGES: 0x806D,
    UNPACK_IMAGE_HEIGHT: 0x806E, TEXTURE_3D: 0x806F, TEXTURE_WRAP_R: 0x8072, MAX_3D_TEXTURE_SIZE: 0x8073, UNSIGNED_INT_2_10_10_10_REV: 0x8368, MAX_ELEMENTS_VERTICES: 0x80E8,
    MAX_ELEMENTS_INDICES: 0x80E9, TEXTURE_MIN_LOD: 0x813A, TEXTURE_MAX_LOD: 0x813B, TEXTURE_BASE_LEVEL: 0x813C, TEXTURE_MAX_LEVEL: 0x813D, MIN: 0x8007, MAX: 0x8008,
    DEPTH_COMPONENT24: 0x81A6, MAX_TEXTURE_LOD_BIAS: 0x84FD, TEXTURE_COMPARE_MODE: 0x884C, TEXTURE_COMPARE_FUNC: 0x884D, CURRENT_QUERY: 0x8865, QUERY_RESULT: 0x8866,
    QUERY_RESULT_AVAILABLE: 0x8867, STREAM_READ: 0x88E1, STREAM_COPY: 0x88E2, STATIC_READ: 0x88E5, STATIC_COPY: 0x88E6, DYNAMIC_READ: 0x88E9, DYNAMIC_COPY: 0x88EA, MAX_DRAW_BUFFERS: 0x8824,
    MAX_FRAGMENT_UNIFORM_COMPONENTS: 0x8B49, MAX_VERTEX_UNIFORM_COMPONENTS: 0x8B4A, SAMPLER_3D: 0x8B5F, SAMPLER_2D_SHADOW: 0x8B62, FRAGMENT_SHADER_DERIVATIVE_HINT: 0x8B8B,
    PIXEL_PACK_BUFFER: 0x88EB, PIXEL_UNPACK_BUFFER: 0x88EC, PIXEL_PACK_BUFFER_BINDING: 0x88ED, PIXEL_UNPACK_BUFFER_BINDING: 0x88EF, FLOAT_MAT2x3: 0x8B65, FLOAT_MAT2x4: 0x8B66,
    FLOAT_MAT3x2: 0x8B67, FLOAT_MAT3x4: 0x8B68, FLOAT_MAT4x2: 0x8B69, FLOAT_MAT4x3: 0x8B6A, SRGB: 0x8C40, SRGB8: 0x8C41, SRGB8_ALPHA8: 0x8C43, COMPARE_REF_TO_TEXTURE: 0x884E,
    RGBA32F: 0x8814, RGB32F: 0x8815, RGBA16F: 0x881A, RGB16F: 0x881B, VERTEX_ATTRIB_ARRAY_INTEGER: 0x88FD, MAX_ARRAY_TEXTURE_LAYERS: 0x88FF, MIN_PROGRAM_TEXEL_OFFSET: 0x8904,
    MAX_PROGRAM_TEXEL_OFFSET: 0x8905, MAX_VARYING_COMPONENTS: 0x8B4B, TEXTURE_2D_ARRAY: 0x8C1A, TEXTURE_BINDING_2D_ARRAY: 0x8C1D, R11F_G11F_B10F: 0x8C3A,
    UNSIGNED_INT_10F_11F_11F_REV: 0x8C3B, RGB9_E5: 0x8C3D, UNSIGNED_INT_5_9_9_9_REV: 0x8C3E, TRANSFORM_FEEDBACK_BUFFER_MODE: 0x8C7F, MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS: 0x8C80,
    TRANSFORM_FEEDBACK_VARYINGS: 0x8C83, TRANSFORM_FEEDBACK_BUFFER_START: 0x8C84, TRANSFORM_FEEDBACK_BUFFER_SIZE: 0x8C85, TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN: 0x8C88,
    RASTERIZER_DISCARD: 0x8C89, MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS: 0x8C8A, MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS: 0x8C8B, INTERLEAVED_ATTRIBS: 0x8C8C,
    SEPARATE_ATTRIBS: 0x8C8D, TRANSFORM_FEEDBACK_BUFFER: 0x8C8E, TRANSFORM_FEEDBACK_BUFFER_BINDING: 0x8C8F, RGBA32UI: 0x8D70, RGB32UI: 0x8D71, RGBA16UI: 0x8D76, RGB16UI: 0x8D77,
    RGBA8UI: 0x8D7C, RGB8UI: 0x8D7D, RGBA32I: 0x8D82, RGB32I: 0x8D83, RGBA16I: 0x8D88, RGB16I: 0x8D89, RGBA8I: 0x8D8E, RGB8I: 0x8D8F, RED_INTEGER: 0x8D94, RGB_INTEGER: 0x8D98,
    RGBA_INTEGER: 0x8D99, SAMPLER_2D_ARRAY: 0x8DC1, SAMPLER_2D_ARRAY_SHADOW: 0x8DC4, SAMPLER_CUBE_SHADOW: 0x8DC5, UNSIGNED_INT_VEC2: 0x8DC6, UNSIGNED_INT_VEC3: 0x8DC7,
    UNSIGNED_INT_VEC4: 0x8DC8, INT_SAMPLER_2D: 0x8DCA, INT_SAMPLER_3D: 0x8DCB, INT_SAMPLER_CUBE: 0x8DCC, INT_SAMPLER_2D_ARRAY: 0x8DCF, UNSIGNED_INT_SAMPLER_2D: 0x8DD2,
    UNSIGNED_INT_SAMPLER_3D: 0x8DD3, UNSIGNED_INT_SAMPLER_CUBE: 0x8DD4, UNSIGNED_INT_SAMPLER_2D_ARRAY: 0x8DD7, DEPTH_COMPONENT32F: 0x8CAC, DEPTH32F_STENCIL8: 0x8CAD,
    FLOAT_32_UNSIGNED_INT_24_8_REV: 0x8DAD, FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING: 0x8210, FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE: 0x8211, FRAMEBUFFER_ATTACHMENT_RED_SIZE: 0x8212,
    FRAMEBUFFER_ATTACHMENT_GREEN_SIZE: 0x8213, FRAMEBUFFER_ATTACHMENT_BLUE_SIZE: 0x8214, FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE: 0x8215, FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE: 0x8216,
    FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE: 0x8217, FRAMEBUFFER_DEFAULT: 0x8218, UNSIGNED_INT_24_8: 0x84FA, DEPTH24_STENCIL8: 0x88F0, UNSIGNED_NORMALIZED: 0x8C17,
    DRAW_FRAMEBUFFER_BINDING: 0x8CA6, READ_FRAMEBUFFER: 0x8CA8, DRAW_FRAMEBUFFER: 0x8CA9, READ_FRAMEBUFFER_BINDING: 0x8CAA, RENDERBUFFER_SAMPLES: 0x8CAB,
    FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER: 0x8CD4, MAX_COLOR_ATTACHMENTS: 0x8CDF, FRAMEBUFFER_INCOMPLETE_MULTISAMPLE: 0x8D56, MAX_SAMPLES: 0x8D57, HALF_FLOAT: 0x140B, RG: 0x8227,
    RG_INTEGER: 0x8228, R8: 0x8229, RG8: 0x822B, R16F: 0x822D, R32F: 0x822E, RG16F: 0x822F, RG32F: 0x8230, R8I: 0x8231, R8UI: 0x8232, R16I: 0x8233, R16UI: 0x8234, R32I: 0x8235,
    R32UI: 0x8236, RG8I: 0x8237, RG8UI: 0x8238, RG16I: 0x8239, RG16UI: 0x823A, RG32I: 0x823B, RG32UI: 0x823C, VERTEX_ARRAY_BINDING: 0x85B5, R8_SNORM: 0x8F94, RG8_SNORM: 0x8F95,
    RGB8_SNORM: 0x8F96, RGBA8_SNORM: 0x8F97, SIGNED_NORMALIZED: 0x8F9C, COPY_READ_BUFFER: 0x8F36, COPY_WRITE_BUFFER: 0x8F37, COPY_READ_BUFFER_BINDING: 0x8F36,
    COPY_WRITE_BUFFER_BINDING: 0x8F37, UNIFORM_BUFFER: 0x8A11, UNIFORM_BUFFER_BINDING: 0x8A28, UNIFORM_BUFFER_START: 0x8A29, UNIFORM_BUFFER_SIZE: 0x8A2A,
    MAX_VERTEX_UNIFORM_BLOCKS: 0x8A2B, MAX_FRAGMENT_UNIFORM_BLOCKS: 0x8A2D, MAX_COMBINED_UNIFORM_BLOCKS: 0x8A2E, MAX_UNIFORM_BUFFER_BINDINGS: 0x8A2F, MAX_UNIFORM_BLOCK_SIZE: 0x8A30,
    MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS: 0x8A31, MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS: 0x8A33, UNIFORM_BUFFER_OFFSET_ALIGNMENT: 0x8A34, ACTIVE_UNIFORM_BLOCKS: 0x8A36,
    UNIFORM_TYPE: 0x8A37, UNIFORM_SIZE: 0x8A38, UNIFORM_BLOCK_INDEX: 0x8A3A, UNIFORM_OFFSET: 0x8A3B, UNIFORM_ARRAY_STRIDE: 0x8A3C, UNIFORM_MATRIX_STRIDE: 0x8A3D,
    UNIFORM_IS_ROW_MAJOR: 0x8A3E, UNIFORM_BLOCK_BINDING: 0x8A3F, UNIFORM_BLOCK_DATA_SIZE: 0x8A40, UNIFORM_BLOCK_ACTIVE_UNIFORMS: 0x8A42, UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES: 0x8A43,
    UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER: 0x8A44, UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER: 0x8A46, INVALID_INDEX: 0xFFFFFFFF, MAX_VERTEX_OUTPUT_COMPONENTS: 0x9122,
    MAX_FRAGMENT_INPUT_COMPONENTS: 0x9125, MAX_SERVER_WAIT_TIMEOUT: 0x9111, OBJECT_TYPE: 0x9112, SYNC_CONDITION: 0x9113, SYNC_STATUS: 0x9114, SYNC_FLAGS: 0x9115, SYNC_FENCE: 0x9116,
    SYNC_GPU_COMMANDS_COMPLETE: 0x9117, UNSIGNALED: 0x9118, SIGNALED: 0x9119, ALREADY_SIGNALED: 0x911A, TIMEOUT_EXPIRED: 0x911B, CONDITION_SATISFIED: 0x911C, WAIT_FAILED: 0x911D,
    SYNC_FLUSH_COMMANDS_BIT: 1, VERTEX_ATTRIB_ARRAY_DIVISOR: 0x88FE, ANY_SAMPLES_PASSED: 0x8C2F, ANY_SAMPLES_PASSED_CONSERVATIVE: 0x8D6A, SAMPLER_BINDING: 0x8919,
    RGB10_A2UI: 0x906F, TEXTURE_IMMUTABLE_FORMAT: 0x912F, TEXTURE_IMMUTABLE_LEVELS: 0x82DF, TRANSFORM_FEEDBACK: 0x8E22, TRANSFORM_FEEDBACK_PAUSED: 0x8E23,
    TRANSFORM_FEEDBACK_ACTIVE: 0x8E24, TRANSFORM_FEEDBACK_BINDING: 0x8E25, TIMEOUT_IGNORED: -1, MAX_CLIENT_WAIT_TIMEOUT_WEBGL: 0x9247
  };
  var addSeries = function (o, name, base, n) { for (var i = 0; i < n; i++) o[name + i] = base + i; };
  addSeries(C1, 'TEXTURE', 0x84C0, 32);
  addSeries(C2, 'COLOR_ATTACHMENT', 0x8CE0, 16);
  addSeries(C2, 'DRAW_BUFFER', 0x8825, 16);
  var defineConstants = function (target, tables) {
    for (var t = 0; t < tables.length; t++)
      for (var k in tables[t]) Object.defineProperty(target, k, { value: tables[t][k], enumerable: true });
  };

  // ------------------------------------------------------------------------------------------------------------------------------------------ objects
  var kinds = { Buffer: 98, Texture: 116, Framebuffer: 102, Renderbuffer: 114, Shader: 115, Program: 112, VertexArrayObject: 118, Sampler: 97, Query: 113, TransformFeedback: 120 };
  var makeClass = function (name) {
    var C = function () { throw new TypeError('Illegal constructor'); };
    Object.defineProperty(C, 'name', { value: name });
    Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
    g[name] = C;
    return C;
  };
  var Classes = {};
  for (var kn in kinds) Classes[kn] = makeClass('WebGL' + kn);
  var WebGLUniformLocation = makeClass('WebGLUniformLocation'), WebGLActiveInfo = makeClass('WebGLActiveInfo'), WebGLShaderPrecisionFormat = makeClass('WebGLShaderPrecisionFormat'),
      WebGLSync = makeClass('WebGLSync'), WebGLContextEvent = null;
  var newObj = function (cls, ctx, h, kind) {
    var o = Object.create(cls.prototype);
    Object.defineProperty(o, '__h', { value: h });
    Object.defineProperty(o, '__ctx', { value: ctx });
    Object.defineProperty(o, '__kind', { value: kind });
    Object.defineProperty(o, '__deleted', { value: false, writable: true });
    ctx.__objs[kind + ':' + h] = o;
    return o;
  };
  var handleOf = function (o) { return o === null || o === undefined ? 0 : o.__h; };

  // ------------------------------------------------------------------------------------------------------------------------------------------ contexts
  var WebGLRenderingContext = function () { throw new TypeError('Illegal constructor'); };
  var WebGL2RenderingContext = function () { throw new TypeError('Illegal constructor'); };
  g.WebGLRenderingContext = WebGLRenderingContext;
  g.WebGL2RenderingContext = WebGL2RenderingContext;
  var P1 = WebGLRenderingContext.prototype, P2 = WebGL2RenderingContext.prototype;
  Object.defineProperty(P1, Symbol.toStringTag, { value: 'WebGLRenderingContext', configurable: true });
  Object.defineProperty(P2, Symbol.toStringTag, { value: 'WebGL2RenderingContext', configurable: true });
  defineConstants(P1, [C1]);
  defineConstants(WebGLRenderingContext, [C1]);
  defineConstants(P2, [C1, C2]);
  defineConstants(WebGL2RenderingContext, [C1, C2]);
  // methods shared by both: defined on P1 and inherited by P2 (the prototype chain is P2 -> P1)
  Object.setPrototypeOf(P2, P1);

  var method = function (proto, name, fn) { Object.defineProperty(proto, name, { value: fn, writable: true, configurable: true, enumerable: true }); };
  var m1 = function (name, fn) { method(P1, name, fn); };
  var m2 = function (name, fn) { method(P2, name, fn); };

  var pending = [];
  var flushing = false;
  var flushAll = function () {
    flushing = false;
    var list = pending; pending = [];
    for (var i = 0; i < list.length; i++) { list[i].__queued = false; __gl_present(list[i].__id); }
  };
  var dirty = function (ctx) {
    if (ctx.__queued) return;
    ctx.__queued = true;
    pending.push(ctx);
    if (!flushing) { flushing = true; Promise.resolve().then(flushAll); }
  };
  var syncSize = function (ctx) {
    var cv = ctx.canvas;
    if (cv.width !== ctx.__w || cv.height !== ctx.__h) {
      ctx.__w = cv.width; ctx.__h = cv.height;
      __gl_resize(ctx.__id, ctx.__w, ctx.__h);
      ctx.__w = Math.min(ctx.__w, 8192); ctx.__h = Math.min(ctx.__h, 8192);
    }
  };
  var lost = function (ctx) { return ctx.__lost; };

  g.__make_gl_context = function (kind, attrs) {
    var canvas = this;
    var es3 = kind === 'webgl2';
    var a = {};
    if (attrs && typeof attrs === 'object') for (var k in attrs) a[k] = attrs[k];
    var id = __gl_create(canvas, es3, a);
    if (!id) return null;
    var ctx = Object.create(es3 ? P2 : P1);
    hide(ctx, '__id', id);
    hide(ctx, '__objs', {});
    hide(ctx, '__lost', false);
    hide(ctx, '__queued', false);
    hide(ctx, '__w', canvas.width);
    hide(ctx, '__h', canvas.height);
    hide(ctx, '__es3', es3);
    hide(ctx, '__ext', {});
    hide(ctx, '__attrs', a);
    hide(ctx, '__loseExt', null);
    Object.defineProperty(ctx, 'canvas', { value: canvas, enumerable: true });
    // the context has the default framebuffer-sized viewport (the native side did that)
    return ctx;
  };

  var C = function (ctx, op) { return call.apply(null, [ctx.__id, op].concat(Array.prototype.slice.call(arguments, 2))); };
  var CALL = function (ctx, name, a, b, c, d, e, f, gg, h, i, j, k) { return call(ctx.__id, OP[name], a, b, c, d, e, f, gg, h, i, j, k); };

  // a method that goes straight to a native op, with its arguments as they are
  var simple = function (proto, name, op, argc) {
    method(proto, name, function (a, b, c, d, e, f, gg, h, i, j) {
      if (arguments.length < argc) throw new TypeError("Failed to execute '" + name + "' on '" + (this.__es3 ? 'WebGL2RenderingContext' : 'WebGLRenderingContext') + "': " + argc + ' arguments required, but only ' + arguments.length + ' present.');
      if (this.__lost) return undefined;
      return call(this.__id, OP[op], a, b, c, d, e, f, gg, h, i, j);
    });
  };
  var withObj = function (proto, name, op, argc) {   // arguments that are objects pass as their handles (the native side reads __h)
    simple(proto, name, op, argc);
  };

  Object.defineProperty(P1, 'drawingBufferWidth', { get: function () { syncSize(this); return this.__w; }, configurable: true, enumerable: true });
  Object.defineProperty(P1, 'drawingBufferHeight', { get: function () { syncSize(this); return this.__h; }, configurable: true, enumerable: true });
  Object.defineProperty(P1, 'drawingBufferColorSpace', { value: 'srgb', writable: true, configurable: true, enumerable: true });
  m1('getContextAttributes', function () { return this.__lost ? null : call(this.__id, OP.attributes); });
  m1('isContextLost', function () { return !!this.__lost; });
  m1('getError', function () {
    if (this.__lost) return 0;
    return call(this.__id, OP.getError);
  });
  m1('flush', function () {});
  m1('finish', function () {});
  m1('makeXRCompatible', function () { return Promise.resolve(); });

  // ------------------------------------------------------------------------------------------------------------------------------------------ object creation
  var creators = [['createBuffer', 'Buffer', 'deleteBuffer', 'isBuffer'], ['createTexture', 'Texture', 'deleteTexture', 'isTexture'], ['createFramebuffer', 'Framebuffer', 'deleteFramebuffer', 'isFramebuffer'],
                  ['createRenderbuffer', 'Renderbuffer', 'deleteRenderbuffer', 'isRenderbuffer'], ['createProgram', 'Program', 'deleteProgram', 'isProgram']];
  creators.forEach(function (c) {
    m1(c[0], function () {
      if (this.__lost) return null;
      var h = call(this.__id, OP.create, kinds[c[1]]);
      return h ? newObj(Classes[c[1]], this, h, c[1]) : null;
    });
    m1(c[2], function (o) {
      if (o === null || o === undefined) return;
      if (!(o instanceof Classes[c[1]]) || o.__ctx !== this) throw new TypeError("Failed to execute '" + c[2] + "' on 'WebGLRenderingContext': parameter 1 is not of type 'WebGL" + c[1] + "'.");
      if (o.__deleted || this.__lost) return;
      call(this.__id, OP.destroy, kinds[c[1]], o.__h);
      o.__deleted = true;
    });
    m1(c[3], function (o) { return !!o && o instanceof Classes[c[1]] && o.__ctx === this && !o.__deleted && call(this.__id, OP.isObject, kinds[c[1]], o.__h); });
  });
  m1('createShader', function (type) {
    if (this.__lost) return null;
    var h = call(this.__id, OP.create, 115, type);
    return h ? newObj(Classes.Shader, this, h, 'Shader') : null;
  });
  m1('deleteShader', function (o) { if (o && !o.__deleted && !this.__lost) { call(this.__id, OP.destroy, 115, o.__h); o.__deleted = true; } });
  m1('isShader', function (o) { return !!o && o instanceof Classes.Shader && !o.__deleted; });

  // ------------------------------------------------------------------------------------------------------------------------------------------ buffers
  var slicedView = function (data, srcOffset, length) {
    if (!srcOffset && !length) return data;
    var bytes = data.BYTES_PER_ELEMENT || 1;
    var start = (data.byteOffset || 0) + (srcOffset || 0) * bytes;
    var n = length ? length : (data.byteLength / bytes) - (srcOffset || 0);
    return new Uint8Array(data.buffer || data, start, n * bytes);
  };
  m1('bindBuffer', function (target, o) { if (!this.__lost) CALL(this, 'bindBuffer', target, handleOf(o)); });
  m1('bufferData', function (target, data, usage, srcOffset, length) {
    if (this.__lost) return;
    if (data === null) { this.__err(0x501); return; }
    if (typeof data !== 'number' && srcOffset !== undefined) data = slicedView(data, srcOffset, length);
    CALL(this, 'bufferData', target, data, usage);
  });
  m1('bufferSubData', function (target, offset, data, srcOffset, length) {
    if (this.__lost) return;
    if (srcOffset !== undefined) data = slicedView(data, srcOffset, length);
    CALL(this, 'bufferSubData', target, offset, data);
  });
  m1('getBufferParameter', function (target, pname) { return this.__lost ? null : CALL(this, 'getBufferParameter', target, pname); });
  m1('__err', function (e) { /* an error raised by this layer: the native side does not know about it */ if (!this.__pendingErr) this.__pendingErr = e; });
  P1.getError = (function (orig) {
    return function () {
      if (this.__lost) return 0;
      if (this.__pendingErr) { var e = this.__pendingErr; this.__pendingErr = 0; return e; }
      return call(this.__id, OP.getError);
    };
  })(P1.getError);
  m2('getBufferSubData', function (target, srcByteOffset, dst, dstOffset, length) {
    if (dstOffset || length) dst = slicedView(dst, dstOffset, length);
    CALL(this, 'getBufferSubData', target, srcByteOffset, dst);
  });
  m2('copyBufferSubData', function (rt, wt, ro, wo, size) { CALL(this, 'copyBufferSubData', rt, wt, ro, wo, size); });
  m2('bindBufferBase', function (target, index, buf) { CALL(this, 'bindBufferRange', target, index, handleOf(buf), 0, 0); });
  m2('bindBufferRange', function (target, index, buf, offset, size) { CALL(this, 'bindBufferRange', target, index, handleOf(buf), offset, size); });

  // ------------------------------------------------------------------------------------------------------------------------------------------ textures
  m1('activeTexture', function (u) { CALL(this, 'activeTexture', u); });
  m1('bindTexture', function (target, o) { if (!this.__lost) CALL(this, 'bindTexture', target, handleOf(o)); });
  m1('texParameterf', function (t, p, v) { CALL(this, 'texParameter', t, p, v); });
  m1('texParameteri', function (t, p, v) { CALL(this, 'texParameter', t, p, v); });
  m1('getTexParameter', function (t, p) { return this.__lost ? null : CALL(this, 'getTexParameter', t, p); });
  m1('generateMipmap', function (t) { CALL(this, 'generateMipmap', t); });
  m1('pixelStorei', function (p, v) { CALL(this, 'pixelStorei', p, typeof v === 'boolean' ? (v ? 1 : 0) : v); });
  var viewAt = function (pixels, off) {
    if (!off) return pixels;
    var bytes = pixels.BYTES_PER_ELEMENT || 1;
    return new pixels.constructor(pixels.buffer, pixels.byteOffset + off * bytes, pixels.length - off);
  };
  m1('texImage2D', function (target, level, internal, a, b, c, d, e, f, gg) {
    if (this.__lost) return;
    var n = arguments.length;
    if (n === 6) { CALL(this, 'texImage', target, level, internal, 0, 0, 1, 0, a, b, c, false); return; }   // (format, type, source)
    if (n < 9) throw new TypeError("Failed to execute 'texImage2D' on 'WebGLRenderingContext': Overload resolution failed.");
    var pixels = f === undefined ? null : f;   // (a number is an offset into the pixel unpack buffer)
    if (gg !== undefined && pixels && pixels.buffer) pixels = viewAt(pixels, gg);
    CALL(this, 'texImage', target, level, internal, a, b, 1, c, d, e, pixels, false);   // (width, height, border, format, type, pixels)
  });
  m1('texSubImage2D', function (target, level, xo, yo, a, b, c, d, e, f) {
    if (this.__lost) return;
    var n = arguments.length;
    if (n === 7) { CALL(this, 'texSubImage', target, level, xo, yo, 0, 0, 0, 1, a, b, c, false); return; }   // (format, type, source)
    var pixels = e;
    if (f !== undefined && pixels && pixels.buffer) pixels = viewAt(pixels, f);
    CALL(this, 'texSubImage', target, level, xo, yo, 0, a, b, 1, c, d, pixels, false);
  });
  m1('copyTexImage2D', function (t, l, f, x, y, w, h, b) { CALL(this, 'copyTexImage2D', t, l, f, x, y, w, h, b); });
  m1('copyTexSubImage2D', function (t, l, xo, yo, x, y, w, h) { CALL(this, 'copyTexSubImage', t, l, xo, yo, 0, x, y, w, h); });
  m1('compressedTexImage2D', function () { this.__err(0x500); });
  m1('compressedTexSubImage2D', function () { this.__err(0x500); });
  m2('texImage3D', function (target, level, internal, w, h, d, border, format, type, pixels, srcOffset) {
    if (this.__lost) return;
    if (pixels && pixels.buffer && srcOffset) pixels = viewAt(pixels, srcOffset);
    CALL(this, 'texImage', target, level, internal, w, h, d, border, format, type, pixels === undefined ? null : pixels, true);
  });
  m2('texSubImage3D', function (target, level, xo, yo, zo, w, h, d, format, type, pixels, srcOffset) {
    if (this.__lost) return;
    if (pixels && pixels.buffer && srcOffset) pixels = viewAt(pixels, srcOffset);
    CALL(this, 'texSubImage', target, level, xo, yo, zo, w, h, d, format, type, pixels, true);
  });
  m2('texStorage2D', function (t, levels, f, w, h) { CALL(this, 'texStorage', t, levels, f, w, h, 1); });
  m2('texStorage3D', function (t, levels, f, w, h, d) { CALL(this, 'texStorage', t, levels, f, w, h, d); });
  m2('copyTexSubImage3D', function (t, l, xo, yo, zo, x, y, w, h) { CALL(this, 'copyTexSubImage', t, l, xo, yo, zo, x, y, w, h); });
  m2('compressedTexImage3D', function () { this.__err(0x500); });
  m2('compressedTexSubImage3D', function () { this.__err(0x500); });
  // texImage2D / texSubImage2D of WebGL 2 accept the same forms; the context's prototype chain already provides them.

  // samplers
  m2('createSampler', function () { var h = call(this.__id, OP.create, 97); return h ? newObj(Classes.Sampler, this, h, 'Sampler') : null; });
  m2('deleteSampler', function (o) { if (o && !o.__deleted) { call(this.__id, OP.destroy, 97, o.__h); o.__deleted = true; } });
  m2('isSampler', function (o) { return !!o && o instanceof Classes.Sampler && !o.__deleted; });
  m2('bindSampler', function (unit, o) { CALL(this, 'bindSampler', unit, handleOf(o)); });
  m2('samplerParameteri', function (s, p, v) { CALL(this, 'samplerParameter', handleOf(s), p, v); });
  m2('samplerParameterf', function (s, p, v) { CALL(this, 'samplerParameter', handleOf(s), p, v); });
  m2('getSamplerParameter', function () { return null; });

  // ------------------------------------------------------------------------------------------------------------------------------------------ renderbuffers and framebuffers
  m1('bindRenderbuffer', function (target, o) { if (!this.__lost) CALL(this, 'bindRenderbuffer', target, handleOf(o)); });
  m1('renderbufferStorage', function (t, f, w, h) { CALL(this, 'renderbufferStorage', f, w, h, 0); });
  m1('getRenderbufferParameter', function (t, p) { return this.__lost ? null : CALL(this, 'getRenderbufferParameter', p); });
  m1('bindFramebuffer', function (target, o) { if (!this.__lost) CALL(this, 'bindFramebuffer', target, handleOf(o)); });
  m1('framebufferTexture2D', function (target, att, textarget, tex, level) { CALL(this, 'framebufferTexture', target, att, textarget, handleOf(tex), level, 0); });
  m1('framebufferRenderbuffer', function (target, att, rbt, rb) { CALL(this, 'framebufferRenderbuffer', target, att, rbt, handleOf(rb)); });
  m1('checkFramebufferStatus', function (t) { return this.__lost ? 0x8CDD : CALL(this, 'checkFramebufferStatus', t); });
  m1('getFramebufferAttachmentParameter', function (t, a, p) { return this.__lost ? null : this.__objectParam(CALL(this, 'getFramebufferAttachmentParameter', t, a, p), p, t, a); });
  m1('__objectParam', function (v, p, t, a) {
    if (p === 0x8CD1 && v) {   // the object itself
      var type = CALL(this, 'getFramebufferAttachmentParameter', t, a, 0x8CD0);
      return this.__objs[(type === 0x8D41 ? 'Renderbuffer' : 'Texture') + ':' + v] || null;
    }
    return v;
  });
  m2('renderbufferStorageMultisample', function (t, samples, f, w, h) { CALL(this, 'renderbufferStorage', f, w, h, samples); });
  m2('framebufferTextureLayer', function (target, att, tex, level, layer) { CALL(this, 'framebufferTexture', target, att, 0x0DE1, handleOf(tex), level, layer); });
  m2('drawBuffers', function (bufs) { CALL(this, 'drawBuffers', Array.prototype.slice.call(bufs)); });
  m2('readBuffer', function (s) { CALL(this, 'readBuffer', s); });
  m2('invalidateFramebuffer', function () {});
  m2('invalidateSubFramebuffer', function () {});
  m2('blitFramebuffer', function (a, b, c, d, e, f, gg, h, mask, filter) { CALL(this, 'blitFramebuffer', a, b, c, d, e, f, gg, h, mask, filter); dirty(this); });
  m2('clearBufferfv', function (buffer, drawbuffer, v) { CALL(this, 'clearBuffer', buffer, drawbuffer, F, v); dirty(this); });
  m2('clearBufferiv', function (buffer, drawbuffer, v) { CALL(this, 'clearBuffer', buffer, drawbuffer, I, v); dirty(this); });
  m2('clearBufferuiv', function (buffer, drawbuffer, v) { CALL(this, 'clearBuffer', buffer, drawbuffer, U, v); dirty(this); });
  m2('clearBufferfi', function (buffer, drawbuffer, depth, stencil) { CALL(this, 'clearBufferfi', buffer, drawbuffer, depth, stencil); dirty(this); });

  // ------------------------------------------------------------------------------------------------------------------------------------------ state
  var stateCall = function (proto, name, op, argc) { simple(proto, name, op, argc); };
  m1('enable', function (c) { CALL(this, 'enable', c); });
  m1('disable', function (c) { CALL(this, 'disable', c); });
  m1('isEnabled', function (c) { return CALL(this, 'isEnabled', c); });
  stateCall(P1, 'viewport', 'viewport', 4);
  stateCall(P1, 'scissor', 'scissor', 4);
  stateCall(P1, 'depthRange', 'depthRange', 2);
  stateCall(P1, 'blendColor', 'blendColor', 4);
  m1('blendEquation', function (m) { CALL(this, 'blendEquationSeparate', m, m); });
  stateCall(P1, 'blendEquationSeparate', 'blendEquationSeparate', 2);
  m1('blendFunc', function (s, d) { CALL(this, 'blendFuncSeparate', s, d, s, d); });
  stateCall(P1, 'blendFuncSeparate', 'blendFuncSeparate', 4);
  stateCall(P1, 'colorMask', 'colorMask', 4);
  stateCall(P1, 'depthMask', 'depthMask', 1);
  stateCall(P1, 'depthFunc', 'depthFunc', 1);
  m1('stencilFunc', function (f, r, m) { CALL(this, 'stencilFuncSeparate', 0x408, f, r, m); });
  stateCall(P1, 'stencilFuncSeparate', 'stencilFuncSeparate', 4);
  m1('stencilOp', function (a, b, c) { CALL(this, 'stencilOpSeparate', 0x408, a, b, c); });
  stateCall(P1, 'stencilOpSeparate', 'stencilOpSeparate', 4);
  m1('stencilMask', function (m) { CALL(this, 'stencilMaskSeparate', 0x408, m); });
  stateCall(P1, 'stencilMaskSeparate', 'stencilMaskSeparate', 2);
  stateCall(P1, 'cullFace', 'cullFace', 1);
  stateCall(P1, 'frontFace', 'frontFace', 1);
  stateCall(P1, 'polygonOffset', 'polygonOffset', 2);
  stateCall(P1, 'lineWidth', 'lineWidth', 1);
  stateCall(P1, 'clearColor', 'clearColor', 4);
  stateCall(P1, 'clearDepth', 'clearDepth', 1);
  stateCall(P1, 'clearStencil', 'clearStencil', 1);
  stateCall(P1, 'sampleCoverage', 'sampleCoverage', 2);
  m1('hint', function () {});

  // ------------------------------------------------------------------------------------------------------------------------------------------ getParameter
  var F32 = ['ALIASED_LINE_WIDTH_RANGE', 'ALIASED_POINT_SIZE_RANGE', 'DEPTH_RANGE', 'COLOR_CLEAR_VALUE', 'BLEND_COLOR'].map(function (n) { return C1[n]; });
  var I32 = ['VIEWPORT', 'SCISSOR_BOX', 'MAX_VIEWPORT_DIMS'].map(function (n) { return C1[n]; });
  var BOOLS = [C1.DEPTH_WRITEMASK, C1.SAMPLE_COVERAGE_INVERT, C1.UNPACK_FLIP_Y_WEBGL, C1.UNPACK_PREMULTIPLY_ALPHA_WEBGL, C1.BLEND, C1.CULL_FACE, C1.DEPTH_TEST, C1.DITHER, C1.POLYGON_OFFSET_FILL,
               C1.SAMPLE_ALPHA_TO_COVERAGE, C1.SAMPLE_COVERAGE, C1.SCISSOR_TEST, C1.STENCIL_TEST, C2.RASTERIZER_DISCARD];
  var BINDINGS = {};
  BINDINGS[C1.ARRAY_BUFFER_BINDING] = 'Buffer'; BINDINGS[C1.ELEMENT_ARRAY_BUFFER_BINDING] = 'Buffer'; BINDINGS[C1.CURRENT_PROGRAM] = 'Program'; BINDINGS[C1.FRAMEBUFFER_BINDING] = 'Framebuffer';
  BINDINGS[C2.READ_FRAMEBUFFER_BINDING] = 'Framebuffer'; BINDINGS[C1.RENDERBUFFER_BINDING] = 'Renderbuffer'; BINDINGS[C1.TEXTURE_BINDING_2D] = 'Texture';
  BINDINGS[C1.TEXTURE_BINDING_CUBE_MAP] = 'Texture'; BINDINGS[C2.TEXTURE_BINDING_3D] = 'Texture'; BINDINGS[C2.TEXTURE_BINDING_2D_ARRAY] = 'Texture'; BINDINGS[C2.VERTEX_ARRAY_BINDING] = 'VertexArrayObject';
  BINDINGS[C2.COPY_READ_BUFFER_BINDING] = 'Buffer'; BINDINGS[C2.COPY_WRITE_BUFFER_BINDING] = 'Buffer'; BINDINGS[C2.UNIFORM_BUFFER_BINDING] = 'Buffer'; BINDINGS[C2.PIXEL_PACK_BUFFER_BINDING] = 'Buffer';
  BINDINGS[C2.PIXEL_UNPACK_BUFFER_BINDING] = 'Buffer'; BINDINGS[C2.SAMPLER_BINDING] = 'Sampler'; BINDINGS[C2.TRANSFORM_FEEDBACK_BUFFER_BINDING] = 'Buffer'; BINDINGS[C2.TRANSFORM_FEEDBACK_BINDING] = 'TransformFeedback';
  var GL2_ONLY = {}; for (var gk in C2) GL2_ONLY[C2[gk]] = true;
  m1('getParameter', function (p) {
    if (this.__lost) return null;
    var es3 = this.__es3;
    switch (p) {
      case C1.VENDOR: return 'WebKit';
      case C1.RENDERER: return 'WebKit WebGL';
      case C1.VERSION: return es3 ? 'WebGL 2.0 (OpenGL ES 3.0 TheoryCraftOS)' : 'WebGL 1.0 (OpenGL ES 2.0 TheoryCraftOS)';
      case C1.SHADING_LANGUAGE_VERSION: return es3 ? 'WebGL GLSL ES 3.00 (OpenGL ES GLSL ES 3.0 TheoryCraftOS)' : 'WebGL GLSL ES 1.0 (OpenGL ES GLSL ES 1.0 TheoryCraftOS)';
      case 0x9245: if (this.__ext.WEBGL_debug_renderer_info) return 'TheoryCraftOS'; break;
      case 0x9246: if (this.__ext.WEBGL_debug_renderer_info) return 'TheoryCraftOS Software Rasterizer'; break;
      case C1.COMPRESSED_TEXTURE_FORMATS: return new Uint32Array(0);
      case 0x84FF: if (this.__ext.EXT_texture_filter_anisotropic) return 16; break;
      case 0x8824: case 0x8CDF: if (!es3 && !this.__ext.WEBGL_draw_buffers) break; return call(this.__id, OP.getParameter, 0x8824);
      case 0x8825: break;
    }
    if (!es3 && GL2_ONLY[p] && !(p === C2.MAX_DRAW_BUFFERS && this.__ext.WEBGL_draw_buffers)) { this.__err(0x500); return null; }
    if (BINDINGS[p] !== undefined) {
      var h = call(this.__id, OP.boundObject, p);
      if (BINDINGS[p] === 'VertexArrayObject') { return h ? (this.__objs['VertexArrayObject:' + h] || null) : null; }
      return h ? (this.__objs[BINDINGS[p] + ':' + h] || null) : null;
    }
    var v = call(this.__id, OP.getParameter, p);
    if (BOOLS.indexOf(p) >= 0) return !!v;
    if (p === C1.COLOR_WRITEMASK) return Array.prototype.map.call(v, function (x) { return !!x; });
    if (F32.indexOf(p) >= 0) return new Float32Array(v);
    if (I32.indexOf(p) >= 0) return new Int32Array(v);
    if (v === undefined) { this.__err(0x500); return null; }
    return v;
  });

  // ------------------------------------------------------------------------------------------------------------------------------------------ shaders and programs
  m1('shaderSource', function (s, src) { if (!this.__lost) CALL(this, 'shaderSource', handleOf(s), String(src)); });
  m1('compileShader', function (s) { if (!this.__lost) CALL(this, 'compileShader', handleOf(s)); });
  m1('getShaderParameter', function (s, p) {
    if (this.__lost) return null;
    var v = CALL(this, 'getShaderParameter', handleOf(s), p);
    return (p === 0x8B80 || p === 0x8B81) ? !!v : v;
  });
  m1('getShaderInfoLog', function (s) { return this.__lost ? null : CALL(this, 'getShaderInfoLog', handleOf(s)); });
  m1('getShaderSource', function (s) { return this.__lost ? null : CALL(this, 'getShaderSource', handleOf(s)); });
  m1('getShaderPrecisionFormat', function (st, pt) {
    var o = Object.create(WebGLShaderPrecisionFormat.prototype);
    var int = pt >= C1.LOW_INT && pt <= C1.HIGH_INT;
    o.rangeMin = int ? 31 : 127; o.rangeMax = int ? 30 : 127; o.precision = int ? 0 : 23;
    return o;
  });
  m1('attachShader', function (p, s) { if (!this.__lost) CALL(this, 'attachShader', handleOf(p), handleOf(s)); });
  m1('detachShader', function (p, s) { if (!this.__lost) CALL(this, 'detachShader', handleOf(p), handleOf(s)); });
  m1('linkProgram', function (p) { if (!this.__lost) CALL(this, 'linkProgram', handleOf(p)); });
  m1('validateProgram', function (p) { if (!this.__lost) CALL(this, 'validateProgram', handleOf(p)); });
  m1('useProgram', function (p) { if (!this.__lost) CALL(this, 'useProgram', handleOf(p)); });
  m1('getProgramParameter', function (p, pn) {
    if (this.__lost) return null;
    var v = CALL(this, 'getProgramParameter', handleOf(p), pn);
    return (pn === 0x8B80 || pn === 0x8B82 || pn === 0x8B83) ? !!v : v;
  });
  m1('getProgramInfoLog', function (p) { return this.__lost ? null : CALL(this, 'getProgramInfoLog', handleOf(p)); });
  m1('bindAttribLocation', function (p, i, n) { CALL(this, 'bindAttribLocation', handleOf(p), i, String(n)); });
  m1('getAttribLocation', function (p, n) { return CALL(this, 'getAttribLocation', handleOf(p), String(n)); });
  var activeInfo = function (a) {
    if (!a) return null;
    var o = Object.create(WebGLActiveInfo.prototype);
    o.name = a[0]; o.size = a[1]; o.type = a[2];
    return o;
  };
  m1('getActiveUniform', function (p, i) { return activeInfo(CALL(this, 'getActiveUniform', handleOf(p), i)); });
  m1('getActiveAttrib', function (p, i) { return activeInfo(CALL(this, 'getActiveAttrib', handleOf(p), i)); });
  m1('getUniformLocation', function (p, n) {
    var loc = CALL(this, 'getUniformLocation', handleOf(p), String(n));
    if (loc === null || loc === undefined) return null;
    var o = Object.create(WebGLUniformLocation.prototype);
    Object.defineProperty(o, '__h', { value: loc });
    Object.defineProperty(o, '__prog', { value: p });
    return o;
  });
  m1('getAttachedShaders', function (p) {
    var hs = CALL(this, 'getAttachedShaders', handleOf(p)), self = this;
    return hs.map(function (h) { return self.__objs['Shader:' + h] || null; });
  });
  m1('getUniform', function (p, loc) {
    var v = CALL(this, 'getUniform', handleOf(p), handleOf(loc));
    return v === undefined ? null : v;
  });
  // uniform setters
  var locOf = function (l) { return l === null || l === undefined ? null : l.__h; };
  [['f', F], ['i', I], ['ui', U]].forEach(function (kk) {
    var suffix = kk[0], kind = kk[1];
    for (var n = 1; n <= 4; n++) (function (n) {
      var name = 'uniform' + n + suffix;
      var fn = function (loc, a, b, c, d) {
        if (this.__lost) return;
        var vals = [a, b, c, d].slice(0, n);
        CALL(this, 'uniform', locOf(loc), kind, 1, n, vals);
      };
      var fnv = function (loc, data, srcOffset, srcLength) {
        if (this.__lost) return;
        if (srcOffset) data = Array.prototype.slice.call(data, srcOffset, srcLength ? srcOffset + srcLength : undefined);
        if (data === null || data === undefined || typeof data.length !== 'number') throw new TypeError('Failed to execute uniform' + n + suffix + "v: parameter 2 is not a sequence of numbers.");
        CALL(this, 'uniform', locOf(loc), kind, 1, n, data);
      };
      method(suffix === 'ui' ? P2 : P1, name, fn);
      method(suffix === 'ui' ? P2 : P1, name.replace(/(\d)(f|i|ui)$/, '$1$2v'), fnv);
    })(n);
  });
  var matrixSizes = [[2, 2], [3, 3], [4, 4], [2, 3], [2, 4], [3, 2], [3, 4], [4, 2], [4, 3]];
  matrixSizes.forEach(function (sz) {
    var cols = sz[0], rows = sz[1];
    var name = 'uniformMatrix' + (cols === rows ? cols : cols + 'x' + rows) + 'fv';
    method(cols === rows ? P1 : P2, name, function (loc, transpose, data, srcOffset, srcLength) {
      if (this.__lost) return;
      if (transpose) { this.__err(0x501); return; }
      if (srcOffset) data = Array.prototype.slice.call(data, srcOffset, srcLength ? srcOffset + srcLength : undefined);
      CALL(this, 'uniform', locOf(loc), F, cols, rows, data);
    });
  });
  // vertex attributes
  m1('enableVertexAttribArray', function (i) { CALL(this, 'enableVertexAttribArray', i); });
  m1('disableVertexAttribArray', function (i) { CALL(this, 'disableVertexAttribArray', i); });
  m1('vertexAttribPointer', function (i, size, type, normalized, stride, offset) { CALL(this, 'vertexAttribPointer', i, size, type, normalized, stride, offset, false); });
  m2('vertexAttribIPointer', function (i, size, type, stride, offset) { CALL(this, 'vertexAttribPointer', i, size, type, false, stride, offset, true); });
  m2('vertexAttribDivisor', function (i, d) { CALL(this, 'vertexAttribDivisor', i, d); });
  for (var vn = 1; vn <= 4; vn++) (function (n) {
    m1('vertexAttrib' + n + 'f', function (i, a, b, c, d) { CALL(this, 'vertexAttrib', i, F, a, n > 1 ? b : 0, n > 2 ? c : 0, n > 3 ? d : 1); });
    m1('vertexAttrib' + n + 'fv', function (i, v) { CALL(this, 'vertexAttrib', i, F, v[0], n > 1 ? v[1] : 0, n > 2 ? v[2] : 0, n > 3 ? v[3] : 1); });
  })(vn);
  m2('vertexAttribI4i', function (i, a, b, c, d) { CALL(this, 'vertexAttrib', i, I, a, b, c, d); });
  m2('vertexAttribI4ui', function (i, a, b, c, d) { CALL(this, 'vertexAttrib', i, U, a, b, c, d); });
  m2('vertexAttribI4iv', function (i, v) { CALL(this, 'vertexAttrib', i, I, v[0], v[1], v[2], v[3]); });
  m2('vertexAttribI4uiv', function (i, v) { CALL(this, 'vertexAttrib', i, U, v[0], v[1], v[2], v[3]); });
  m1('getVertexAttrib', function (i, p) {
    if (p === 0x889F) { var h = CALL(this, 'getVertexAttrib', i, p); return h ? (this.__objs['Buffer:' + h] || null) : null; }
    var v = CALL(this, 'getVertexAttrib', i, p);
    if (p === 0x8622 || p === 0x886A || p === 0x88FD) return !!v;
    if (p === 0x8626) return new Float32Array(v);
    return v;
  });
  m1('getVertexAttribOffset', function (i, p) { return 0; });
  // vertex arrays (core in WebGL 2; the OES extension in WebGL 1)
  var vaoCreate = function () { var h = call(this.__id, OP.create, 118); return h ? newObj(Classes.VertexArrayObject, this, h, 'VertexArrayObject') : null; };
  var vaoDelete = function (o) { if (o && !o.__deleted) { call(this.__id, OP.destroy, 118, o.__h); o.__deleted = true; } };
  var vaoBind = function (o) { CALL(this, 'bindVertexArray', handleOf(o)); };
  var vaoIs = function (o) { return !!o && o instanceof Classes.VertexArrayObject && !o.__deleted; };
  m2('createVertexArray', vaoCreate); m2('deleteVertexArray', vaoDelete); m2('bindVertexArray', vaoBind); m2('isVertexArray', vaoIs);

  // ------------------------------------------------------------------------------------------------------------------------------------------ drawing
  var drawChecked = function (ctx) {
    syncSize(ctx);
    dirty(ctx);
  };
  m1('clear', function (mask) { if (this.__lost) return; drawChecked(this); CALL(this, 'clear', mask); });
  m1('drawArrays', function (mode, first, count) { if (this.__lost) return; drawChecked(this); CALL(this, 'drawArrays', mode, first, count, 1); });
  m1('drawElements', function (mode, count, type, offset) { if (this.__lost) return; drawChecked(this); CALL(this, 'drawElements', mode, count, type, offset, 1); });
  m2('drawArraysInstanced', function (mode, first, count, n) { if (this.__lost) return; drawChecked(this); CALL(this, 'drawArrays', mode, first, count, n); });
  m2('drawElementsInstanced', function (mode, count, type, offset, n) { if (this.__lost) return; drawChecked(this); CALL(this, 'drawElements', mode, count, type, offset, n); });
  m2('drawRangeElements', function (mode, start, end, count, type, offset) { if (this.__lost) return; drawChecked(this); CALL(this, 'drawRangeElements', mode, start, end, count, type, offset); });
  m1('readPixels', function (x, y, w, h, format, type, pixels, dstOffset) {
    if (this.__lost) return;
    if (pixels === null || pixels === undefined) { this.__err(0x502); return; }
    if (dstOffset && typeof pixels !== 'number') pixels = viewAt(pixels, dstOffset);
    __gl_present(this.__id);
    CALL(this, 'readPixels', x, y, w, h, format, type, pixels);
  });

  // queries, sync, transform feedback (the occlusion query is answered; transform feedback only records its state)
  m2('createQuery', function () { var h = call(this.__id, OP.create, 113); return h ? newObj(Classes.Query, this, h, 'Query') : null; });
  m2('deleteQuery', function (o) { if (o && !o.__deleted) { call(this.__id, OP.destroy, 113, o.__h); o.__deleted = true; } });
  m2('isQuery', function (o) { return !!o && o instanceof Classes.Query && !o.__deleted; });
  m2('beginQuery', function (t, q) { CALL(this, 'beginQuery', t, handleOf(q)); });
  m2('endQuery', function (t) { CALL(this, 'endQuery', t); });
  m2('getQuery', function (t, p) { var h = CALL(this, 'getQuery', t, p); return h ? (this.__objs['Query:' + h] || null) : null; });
  m2('getQueryParameter', function (q, p) { var v = CALL(this, 'getQueryParameter', handleOf(q), p); return p === 0x8867 ? !!v : v; });
  m2('fenceSync', function () { var o = Object.create(WebGLSync.prototype); Object.defineProperty(o, '__sync', { value: true }); return o; });
  m2('isSync', function (s) { return !!s && s instanceof WebGLSync; });
  m2('deleteSync', function () {});
  m2('clientWaitSync', function () { return C2.ALREADY_SIGNALED; });
  m2('waitSync', function () {});
  m2('getSyncParameter', function (s, p) { return p === C2.SYNC_STATUS ? C2.SIGNALED : (p === C2.SYNC_CONDITION ? C2.SYNC_GPU_COMMANDS_COMPLETE : (p === C2.OBJECT_TYPE ? C2.SYNC_FENCE : 0)); });
  m2('createTransformFeedback', function () { var h = call(this.__id, OP.create, 120); return h ? newObj(Classes.TransformFeedback, this, h, 'TransformFeedback') : null; });
  m2('deleteTransformFeedback', function (o) { if (o && !o.__deleted) { call(this.__id, OP.destroy, 120, o.__h); o.__deleted = true; } });
  m2('isTransformFeedback', function (o) { return !!o && o instanceof Classes.TransformFeedback && !o.__deleted; });
  m2('bindTransformFeedback', function () {});
  m2('beginTransformFeedback', function (mode) { CALL(this, 'beginTransformFeedback', mode); });
  m2('endTransformFeedback', function () { CALL(this, 'endTransformFeedback'); });
  m2('pauseTransformFeedback', function () {});
  m2('resumeTransformFeedback', function () {});
  m2('transformFeedbackVaryings', function (p, names, mode) { CALL(this, 'transformFeedbackVaryings', handleOf(p), Array.prototype.slice.call(names), mode); });
  m2('getTransformFeedbackVarying', function () { return null; });

  // uniform blocks
  m2('getUniformBlockIndex', function (p, n) { return CALL(this, 'getUniformBlockIndex', handleOf(p), String(n)); });
  m2('getActiveUniformBlockName', function (p, i) { return CALL(this, 'getActiveUniformBlockName', handleOf(p), i); });
  m2('getActiveUniformBlockParameter', function (p, i, pn) {
    var v = CALL(this, 'getActiveUniformBlockParameter', handleOf(p), i, pn);
    return (pn === C2.UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER || pn === C2.UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER) ? !!v : v;
  });
  m2('uniformBlockBinding', function (p, i, b) { CALL(this, 'uniformBlockBinding', handleOf(p), i, b); });
  m2('getFragDataLocation', function (p, n) { return CALL(this, 'getFragDataLocation', handleOf(p), String(n)); });
  m2('getUniformIndices', function (p, names) { return null; });
  m2('getIndexedParameter', function (target, index) { return null; });
  m2('getInternalformatParameter', function (t, f, p) { return p === 0x80A9 ? new Int32Array([4, 2, 1]) : new Int32Array(0); });
  m2('getBufferSubDataAsync', function () { return Promise.reject(new Error('not supported')); });

  // ------------------------------------------------------------------------------------------------------------------------------------------ extensions
  var ext1 = ['ANGLE_instanced_arrays', 'EXT_blend_minmax', 'EXT_color_buffer_half_float', 'EXT_frag_depth', 'EXT_shader_texture_lod', 'EXT_texture_filter_anisotropic', 'OES_element_index_uint',
              'OES_standard_derivatives', 'OES_texture_float', 'OES_texture_float_linear', 'OES_texture_half_float', 'OES_texture_half_float_linear', 'OES_vertex_array_object',
              'WEBGL_color_buffer_float', 'WEBGL_debug_renderer_info', 'WEBGL_depth_texture', 'WEBGL_draw_buffers', 'WEBGL_lose_context'];
  var ext2 = ['EXT_color_buffer_float', 'EXT_color_buffer_half_float', 'EXT_texture_filter_anisotropic', 'OES_texture_float_linear', 'WEBGL_debug_renderer_info', 'WEBGL_lose_context'];
  m1('getSupportedExtensions', function () { return (this.__es3 ? ext2 : ext1).slice(); });
  var makeExt = function (ctx, name) {
    var e = {};
    var nm = function (n, v) { Object.defineProperty(e, n, { value: v, enumerable: true }); };
    var tag = function () { Object.defineProperty(e, Symbol.toStringTag, { value: name, configurable: true }); };
    switch (name) {
      case 'ANGLE_instanced_arrays':
        nm('VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE', 0x88FE);
        nm('drawArraysInstancedANGLE', function (m, f, c, n) { ctx.drawArraysInstanced(m, f, c, n); });
        nm('drawElementsInstancedANGLE', function (m, c, t, o, n) { ctx.drawElementsInstanced(m, c, t, o, n); });
        nm('vertexAttribDivisorANGLE', function (i, d) { ctx.vertexAttribDivisor(i, d); });
        break;
      case 'EXT_blend_minmax': nm('MIN_EXT', 0x8007); nm('MAX_EXT', 0x8008); break;
      case 'EXT_texture_filter_anisotropic': nm('MAX_TEXTURE_MAX_ANISOTROPY_EXT', 0x84FF); nm('TEXTURE_MAX_ANISOTROPY_EXT', 0x84FE); break;
      case 'OES_standard_derivatives': nm('FRAGMENT_SHADER_DERIVATIVE_HINT_OES', 0x8B8B); break;
      case 'OES_texture_half_float': nm('HALF_FLOAT_OES', 0x8D61); break;
      case 'WEBGL_depth_texture': nm('UNSIGNED_INT_24_8_WEBGL', 0x84FA); break;
      case 'WEBGL_color_buffer_float': nm('RGBA32F_EXT', 0x8814); nm('RGB32F_EXT', 0x8815); nm('FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT', 0x8211); nm('UNSIGNED_NORMALIZED_EXT', 0x8C17); break;
      case 'EXT_color_buffer_half_float': nm('RGBA16F_EXT', 0x881A); nm('RGB16F_EXT', 0x881B); nm('FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE_EXT', 0x8211); nm('UNSIGNED_NORMALIZED_EXT', 0x8C17); break;
      case 'OES_vertex_array_object':
        nm('VERTEX_ARRAY_BINDING_OES', 0x85B5);
        nm('createVertexArrayOES', function () { return vaoCreate.call(ctx); });
        nm('deleteVertexArrayOES', function (o) { vaoDelete.call(ctx, o); });
        nm('bindVertexArrayOES', function (o) { vaoBind.call(ctx, o); });
        nm('isVertexArrayOES', function (o) { return vaoIs.call(ctx, o); });
        break;
      case 'WEBGL_draw_buffers':
        nm('drawBuffersWEBGL', function (b) { ctx.drawBuffers(b); });
        nm('MAX_COLOR_ATTACHMENTS_WEBGL', 8); nm('MAX_DRAW_BUFFERS_WEBGL', 8);
        for (var i = 0; i < 16; i++) { nm('COLOR_ATTACHMENT' + i + '_WEBGL', 0x8CE0 + i); nm('DRAW_BUFFER' + i + '_WEBGL', 0x8825 + i); }
        break;
      case 'WEBGL_debug_renderer_info': nm('UNMASKED_VENDOR_WEBGL', 0x9245); nm('UNMASKED_RENDERER_WEBGL', 0x9246); break;
      case 'WEBGL_lose_context':
        nm('loseContext', function () {
          if (ctx.__lost) return;
          ctx.__lost = true;
          call(ctx.__id, OP.lose);
          var ev = new Event('webglcontextlost', { cancelable: true });
          ctx.canvas.dispatchEvent(ev);
          ctx.__restorable = ev.defaultPrevented;
        });
        nm('restoreContext', function () {
          if (!ctx.__lost || !ctx.__restorable) return;
          setTimeout(function () { ctx.__lost = false; call(ctx.__id, OP.restore); ctx.canvas.dispatchEvent(new Event('webglcontextrestored')); }, 0);
        });
        break;
      case 'EXT_color_buffer_float': break;
    }
    tag();
    return e;
  };
  m1('getExtension', function (name) {
    if (this.__lost) return null;
    var supported = (this.__es3 ? ext2 : ext1);
    var lc = String(name).toLowerCase();
    for (var i = 0; i < supported.length; i++) {
      if (supported[i].toLowerCase() === lc) {
        var key = supported[i];
        if (!this.__ext[key]) this.__ext[key] = makeExt(this, key);
        return this.__ext[key];
      }
    }
    return null;
  });

  // ------------------------------------------------------------------------------------------------------------------------------------------ events
  g.WebGLContextEvent = function WebGLContextEvent(type, init) {
    var e = new Event(type, init);
    Object.defineProperty(e, 'statusMessage', { value: (init && init.statusMessage) || '', enumerable: true });
    return e;
  };
})(this);
)JSPRELUDE";

}  // namespace

const char* webgl_prelude() { return kWebGlPrelude; }

}  // namespace browser
