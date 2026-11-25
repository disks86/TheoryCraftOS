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

// A SPIR-V module writer: the numbers of the SPIR-V 1.5 specification and GLSL.std.450 that the TGSI compiler (TgsiSpirv.cpp) uses, and a builder that keeps a
// module's sections apart and gives each type and constant one id. Words go into allocator-backed lists; a failed allocation makes the builder fail (ok() false)
// rather than write a short module.
#pragma once
#include <initializer_list>
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/StringView.hpp"

namespace graphics::spirv {

// ---- the specification's numbers ---------------------------------------------------------------------------------------------------------------------------
enum Op : uint16_t {
  OpNop = 0, OpName = 5, OpExtension = 10, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16, OpCapability = 17,
  OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27,
  OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpTypeFunction = 33,
  OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpConstantComposite = 44, OpConstantNull = 46,
  OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpImageTexelPointer = 60, OpLoad = 61, OpStore = 62, OpAccessChain = 65, OpArrayLength = 68,
  OpDecorate = 71, OpMemberDecorate = 72, OpVectorExtractDynamic = 77, OpVectorShuffle = 79, OpCompositeConstruct = 80, OpCompositeExtract = 81,
  OpCompositeInsert = 82, OpSampledImage = 86, OpImageSampleImplicitLod = 87, OpImageSampleExplicitLod = 88, OpImageFetch = 95, OpImageGather = 96,
  OpImageRead = 98, OpImageWrite = 99, OpImageQuerySizeLod = 103, OpImageQuerySize = 104, OpImageQueryLod = 105, OpImageQueryLevels = 106,
  OpImageQuerySamples = 107, OpConvertFToU = 109, OpConvertFToS = 110, OpConvertSToF = 111, OpConvertUToF = 112, OpFConvert = 115, OpBitcast = 124,
  OpSNegate = 126, OpFNegate = 127, OpIAdd = 128, OpFAdd = 129, OpISub = 130, OpFSub = 131, OpIMul = 132, OpFMul = 133, OpUDiv = 134, OpSDiv = 135,
  OpFDiv = 136, OpUMod = 137, OpSRem = 138, OpSMod = 139, OpDot = 148, OpUMulExtended = 151, OpSMulExtended = 152, OpAny = 154, OpAll = 155, OpIsNan = 156,
  OpLogicalOr = 166, OpLogicalAnd = 167, OpLogicalNot = 168, OpSelect = 169, OpIEqual = 170, OpINotEqual = 171, OpUGreaterThan = 172, OpSGreaterThan = 173,
  OpUGreaterThanEqual = 174, OpSGreaterThanEqual = 175, OpULessThan = 176, OpSLessThan = 177, OpULessThanEqual = 178, OpSLessThanEqual = 179,
  OpFOrdEqual = 180, OpFUnordEqual = 181, OpFOrdNotEqual = 182, OpFUnordNotEqual = 183, OpFOrdLessThan = 184, OpFUnordLessThan = 185, OpFOrdGreaterThan = 186,
  OpFUnordGreaterThan = 187, OpFOrdLessThanEqual = 188, OpFUnordLessThanEqual = 189, OpFOrdGreaterThanEqual = 190, OpFUnordGreaterThanEqual = 191,
  OpShiftRightLogical = 194, OpShiftRightArithmetic = 195, OpShiftLeftLogical = 196, OpBitwiseOr = 197, OpBitwiseXor = 198, OpBitwiseAnd = 199, OpNot = 200,
  OpDPdx = 207, OpDPdy = 208, OpDPdxFine = 210, OpDPdyFine = 211, OpDPdxCoarse = 213, OpDPdyCoarse = 214, OpEmitVertex = 218, OpEndPrimitive = 219,
  OpEmitStreamVertex = 220, OpEndStreamPrimitive = 221, OpControlBarrier = 224, OpMemoryBarrier = 225, OpAtomicExchange = 229, OpAtomicCompareExchange = 230,
  OpAtomicIAdd = 234, OpAtomicSMin = 236, OpAtomicUMin = 237, OpAtomicSMax = 238, OpAtomicUMax = 239, OpAtomicAnd = 240, OpAtomicOr = 241, OpAtomicXor = 242,
  OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250, OpReturn = 253, OpUnreachable = 255,
  OpImageSparseSampleImplicitLod = 305, OpImageSparseSampleExplicitLod = 306, OpImageSparseFetch = 313, OpImageSparseGather = 314, OpImageSparseTexelsResident = 316,
  OpDemoteToHelperInvocation = 5380,
};

enum Capability : uint32_t {
  CapShader = 1, CapGeometry = 2, CapTessellation = 3, CapFloat64 = 10, CapTessellationPointSize = 23, CapGeometryPointSize = 24, CapImageGatherExtended = 25,
  CapStorageImageMultisample = 27, CapClipDistance = 32, CapImageCubeArray = 34, CapSampleRateShading = 35, CapSampled1D = 43, CapImage1D = 44,
  CapSampledCubeArray = 45, CapSampledBuffer = 46, CapImageBuffer = 47, CapStorageImageExtendedFormats = 49, CapImageQuery = 50, CapDerivativeControl = 51,
  CapTransformFeedback = 53, CapGeometryStreams = 54, CapStorageImageReadWithoutFormat = 55, CapStorageImageWriteWithoutFormat = 56, CapMultiViewport = 57,
  CapShaderLayer = 69, CapShaderViewportIndex = 70, CapDrawParameters = 4427, CapDemoteToHelperInvocation = 5379,
  CapSparseResidency = 41, CapMinLod = 42, CapFragmentShadingRateKHR = 4422, CapFragmentBarycentricKHR = 5284,
};

enum StorageClass : uint32_t {
  UniformConstant = 0, Input = 1, Uniform = 2, Output = 3, Workgroup = 4, Private = 6, Function = 7, PushConstant = 9, StorageBuffer = 12,
};

enum Decoration : uint32_t {
  DecBlock = 2, DecArrayStride = 6, DecBuiltIn = 11, DecNoPerspective = 13, DecFlat = 14, DecPatch = 15, DecCentroid = 16, DecSample = 17,
  DecNonWritable = 24, DecNonReadable = 25, DecStream = 29, DecLocation = 30, DecComponent = 31, DecIndex = 32, DecBinding = 33, DecDescriptorSet = 34,
  DecOffset = 35, DecXfbBuffer = 36, DecXfbStride = 37, DecPerVertexKHR = 5285,
};

enum BuiltIn : uint32_t {
  BiPosition = 0, BiPointSize = 1, BiClipDistance = 3, BiPrimitiveId = 7, BiInvocationId = 8, BiLayer = 9, BiViewportIndex = 10, BiTessLevelOuter = 11,
  BiTessLevelInner = 12, BiTessCoord = 13, BiPatchVertices = 14, BiFragCoord = 15, BiFrontFacing = 17, BiSampleId = 18, BiSamplePosition = 19, BiSampleMask = 20,
  BiFragDepth = 22, BiHelperInvocation = 23, BiNumWorkgroups = 24, BiWorkgroupId = 26, BiLocalInvocationId = 27, BiGlobalInvocationId = 28,
  BiLocalInvocationIndex = 29, BiVertexIndex = 42, BiInstanceIndex = 43, BiBaseVertex = 4424, BiBaseInstance = 4425,
  BiPrimitiveShadingRateKHR = 4432, BiShadingRateKHR = 4444, BiBaryCoordKHR = 5286, BiBaryCoordNoPerspKHR = 5287,
};

enum ExecutionModel : uint32_t { ModelVertex = 0, ModelTessControl = 1, ModelTessEval = 2, ModelGeometry = 3, ModelFragment = 4, ModelGLCompute = 5 };

enum ExecutionMode : uint32_t {
  ModeInvocations = 0, ModeSpacingEqual = 1, ModeSpacingFractionalEven = 2, ModeSpacingFractionalOdd = 3, ModeVertexOrderCw = 4, ModeVertexOrderCcw = 5,
  ModeOriginUpperLeft = 7, ModeEarlyFragmentTests = 9, ModePointMode = 10, ModeXfb = 11, ModeDepthReplacing = 12, ModeLocalSize = 17, ModeInputPoints = 19,
  ModeInputLines = 20, ModeInputLinesAdjacency = 21, ModeTriangles = 22, ModeInputTrianglesAdjacency = 23, ModeQuads = 24, ModeIsolines = 25,
  ModeOutputVertices = 26, ModeOutputPoints = 27, ModeOutputLineStrip = 28, ModeOutputTriangleStrip = 29,
};

enum Dim : uint32_t { Dim1D = 0, Dim2D = 1, Dim3D = 2, DimCube = 3, DimBuffer = 5 };

enum ImageOperand : uint32_t { ImBias = 0x1, ImLod = 0x2, ImGrad = 0x4, ImConstOffset = 0x8, ImOffset = 0x10, ImSample = 0x40, ImMinLod = 0x80 };

enum Scope : uint32_t { ScopeDevice = 1, ScopeWorkgroup = 2, ScopeInvocation = 4 };
enum Semantics : uint32_t {
  SemAcquireRelease = 0x8, SemUniformMemory = 0x40, SemWorkgroupMemory = 0x100, SemImageMemory = 0x800,
};

// GLSL.std.450
enum Glsl : uint32_t {
  GlslRoundEven = 2, GlslTrunc = 3, GlslFAbs = 4, GlslSAbs = 5, GlslFSign = 6, GlslSSign = 7, GlslFloor = 8, GlslCeil = 9, GlslFract = 10, GlslSin = 13,
  GlslCos = 14, GlslPow = 26, GlslExp2 = 29, GlslLog2 = 30, GlslSqrt = 31, GlslInverseSqrt = 32, GlslUMin = 38, GlslSMin = 39, GlslUMax = 41, GlslSMax = 42,
  GlslFClamp = 43, GlslFma = 50, GlslCross = 68, GlslNMin = 79, GlslNMax = 80,
};

// The image formats of OpTypeImage.
enum ImageFormat : uint32_t {
  FmtUnknown = 0, FmtRgba32f = 1, FmtRgba16f = 2, FmtR32f = 3, FmtRgba8 = 4, FmtRgba8Snorm = 5, FmtRg32f = 6, FmtRg16f = 7, FmtR11fG11fB10f = 8, FmtR16f = 9,
  FmtRgba16 = 10, FmtRgb10A2 = 11, FmtRg16 = 12, FmtRg8 = 13, FmtR16 = 14, FmtR8 = 15, FmtRgba16Snorm = 16, FmtRg16Snorm = 17, FmtRg8Snorm = 18,
  FmtR16Snorm = 19, FmtR8Snorm = 20, FmtRgba32i = 21, FmtRgba16i = 22, FmtRgba8i = 23, FmtR32i = 24, FmtRg32i = 25, FmtRg16i = 26, FmtRg8i = 27,
  FmtR16i = 28, FmtR8i = 29, FmtRgba32ui = 30, FmtRgba16ui = 31, FmtRgba8ui = 32, FmtR32ui = 33, FmtRgb10a2ui = 34, FmtRg32ui = 35, FmtRg16ui = 36,
  FmtRg8ui = 37, FmtR16ui = 38, FmtR8ui = 39,
};

// ---- the builder ------------------------------------------------------------------------------------------------------------------------------------------
class Builder {
 public:
  explicit Builder(allocators::IAllocator& allocator);

  bool ok() const { return ok_; }
  uint32_t id() { return next_++; }

  // Sections (in module order).
  enum Section : uint8_t { kCapabilities, kExtensions, kImports, kMemoryModel, kEntryPoints, kModes, kDebug, kAnnotations, kGlobals, kFunctionVariables, kCode, kCount };
  void op(Section s, uint16_t opcode, std::initializer_list<uint32_t> words);
  // An instruction with a string operand after `before` and `after` words (OpExtension, OpExtInstImport, OpEntryPoint, OpName).
  void op_string(Section s, uint16_t opcode, std::initializer_list<uint32_t> before, StringView text, const uint32_t* after, uint32_t after_count);
  // An instruction whose words are a list (made by the caller).
  void op_list(Section s, uint16_t opcode, const uint32_t* words, uint32_t count);

  void capability(uint32_t cap);   // once each
  void extension(StringView name);  // once each
  void decorate(uint32_t target, uint32_t decoration, std::initializer_list<uint32_t> values = {});
  void member_decorate(uint32_t type, uint32_t member, uint32_t decoration, std::initializer_list<uint32_t> values = {});

  // Types (one id each).
  uint32_t t_void();
  uint32_t t_bool();
  uint32_t t_uint();
  uint32_t t_int();
  uint32_t t_float();
  uint32_t t_double();
  uint32_t t_vector(uint32_t component, uint32_t count);
  uint32_t t_pointer(uint32_t storage, uint32_t type);
  uint32_t t_array(uint32_t element, uint32_t length);        // (no stride decoration: Function / Private / interface arrays)
  uint32_t t_function_void();
  uint32_t t_sampler();
  uint32_t t_image(uint32_t sampled_type, uint32_t dim, bool arrayed, bool multisampled, uint32_t sampled, uint32_t format);
  uint32_t t_sampled_image(uint32_t image);

  // Constants (one id each).
  uint32_t c_uint(uint32_t v);
  uint32_t c_int(int32_t v) { return constant(t_int(), static_cast<uint32_t>(v)); }
  uint32_t c_float_bits(uint32_t bits) { return constant(t_float(), bits); }
  uint32_t c_float(float f);
  uint32_t c_bool(bool v);
  uint32_t c_double(double v);
  uint32_t c_composite(uint32_t type, std::initializer_list<uint32_t> parts);
  uint32_t c_uvec4(const uint32_t bits[4]);
  uint32_t c_null(uint32_t type);

  // A global variable (listed in the entry point's interface; SPIR-V 1.4+ lists every global).
  uint32_t global(uint32_t storage, uint32_t type, uint32_t initializer = 0);
  const List<uint32_t>& interface() const { return interface_; }

  // The module's words (header + sections).
  bool finish(List<uint32_t>* out);

 private:
  uint32_t constant(uint32_t type, uint32_t value);
  struct Key {
    uint32_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0;
    bool operator==(const Key& o) const { return a == o.a && b == o.b && c == o.c && d == o.d && e == o.e && f == o.f && g == o.g; }
  };
  struct Cached {
    Key key;
    uint32_t id;
  };
  uint32_t find(const Key& k) const;
  void remember(const Key& k, uint32_t id);
  void put(Section s, uint32_t word);

  allocators::IAllocator& allocator_;
  List<uint32_t> sections_[kCount];
  List<Cached> cache_;
  List<uint32_t> interface_;
  List<uint32_t> capabilities_;
  uint32_t next_ = 1;
  bool ok_ = true;
};

}  // namespace graphics::spirv
