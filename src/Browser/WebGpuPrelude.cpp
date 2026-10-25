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

// The JavaScript side of WebGPU: navigator.gpu, adapters, devices, resources, command encoders and the canvas context, over the natives in DomBindingsGpu.cpp.
#include "Browser/WebPrelude.hpp"

namespace browser {

namespace {

const char kWebGpuPrelude[] = R"JSPRELUDE((function (g) {
  if (typeof __gpu_create !== 'function' || g.GPU !== undefined) return;
  var OPS = __gpu_ops(), OP = {};
  for (var oi = 0; oi < OPS.length; oi++) OP[OPS[oi]] = oi;
  var call = __gpu;
  var hide = function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true, configurable: true }); };
  var def = function (o, k, v) { Object.defineProperty(o, k, { value: v, writable: true, configurable: true, enumerable: true }); };
  var getter = function (o, k, fn) { Object.defineProperty(o, k, { get: fn, configurable: true, enumerable: true }); };

  var table = function (list) { var t = {}; for (var i = 0; i < list.length; i++) t[list[i]] = i; return t; };
  var COMPARE = table(['never', 'less', 'equal', 'less-equal', 'greater', 'not-equal', 'greater-equal', 'always']);
  var STENCIL_OP = table(['keep', 'zero', 'replace', 'invert', 'increment-clamp', 'decrement-clamp', 'increment-wrap', 'decrement-wrap']);
  var BLEND_FACTOR = table(['zero', 'one', 'src', 'one-minus-src', 'src-alpha', 'one-minus-src-alpha', 'dst', 'one-minus-dst', 'dst-alpha', 'one-minus-dst-alpha', 'src-alpha-saturated', 'constant', 'one-minus-constant']);
  var BLEND_OP = table(['add', 'subtract', 'reverse-subtract', 'min', 'max']);
  var TOPOLOGY = table(['point-list', 'line-list', 'line-strip', 'triangle-list', 'triangle-strip']);
  var VERTEX_FORMAT = table(['uint8x2', 'uint8x4', 'sint8x2', 'sint8x4', 'unorm8x2', 'unorm8x4', 'snorm8x2', 'snorm8x4', 'uint16x2', 'uint16x4', 'sint16x2', 'sint16x4', 'unorm16x2', 'unorm16x4',
                             'snorm16x2', 'snorm16x4', 'float16x2', 'float16x4', 'float32', 'float32x2', 'float32x3', 'float32x4', 'uint32', 'uint32x2', 'uint32x3', 'uint32x4', 'sint32', 'sint32x2',
                             'sint32x3', 'sint32x4', 'unorm10-10-10-2', 'unorm8x4-bgra']);
  var VIEW_DIM = table(['1d', '2d', '2d-array', 'cube', 'cube-array', '3d']);
  var ASPECT = table(['all', 'depth-only', 'stencil-only']);
  var FILTER = { nearest: 0x2600, linear: 0x2601 };
  var ADDRESS = { 'clamp-to-edge': 0x812F, repeat: 0x2901, 'mirror-repeat': 0x8370 };

  // ------------------------------------------------------------------------------------------------------------------------------------ constants
  var constants = function (name, o) {
    var C = function () { throw new TypeError('Illegal constructor'); };
    for (var k in o) Object.defineProperty(C, k, { value: o[k], enumerable: true });
    g[name] = C;
  };
  constants('GPUBufferUsage', { MAP_READ: 1, MAP_WRITE: 2, COPY_SRC: 4, COPY_DST: 8, INDEX: 16, VERTEX: 32, UNIFORM: 64, STORAGE: 128, INDIRECT: 256, QUERY_RESOLVE: 512 });
  constants('GPUTextureUsage', { COPY_SRC: 1, COPY_DST: 2, TEXTURE_BINDING: 4, STORAGE_BINDING: 8, RENDER_ATTACHMENT: 16 });
  constants('GPUMapMode', { READ: 1, WRITE: 2 });
  constants('GPUShaderStage', { VERTEX: 1, FRAGMENT: 2, COMPUTE: 4 });
  constants('GPUColorWrite', { RED: 1, GREEN: 2, BLUE: 4, ALPHA: 8, ALL: 15 });

  var makeClass = function (name, proto) {
    var C = function () { throw new TypeError('Illegal constructor'); };
    Object.defineProperty(C, 'name', { value: name });
    C.prototype = proto || {};
    Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
    Object.defineProperty(C.prototype, 'constructor', { value: C, writable: true, configurable: true });
    g[name] = C;
    return C;
  };
  var make = function (C, props) { var o = Object.create(C.prototype); for (var k in props) hide(o, k, props[k]); return o; };
  var labelled = function (C) { Object.defineProperty(C.prototype, 'label', { get: function () { return this.__label || ''; }, set: function (v) { this.__label = String(v); }, configurable: true, enumerable: true }); };

  var GPUSupportedLimits = makeClass('GPUSupportedLimits');
  var LIMITS = { maxTextureDimension1D: 8192, maxTextureDimension2D: 8192, maxTextureDimension3D: 2048, maxTextureArrayLayers: 2048, maxBindGroups: 4, maxBindGroupsPlusVertexBuffers: 24,
                 maxBindingsPerBindGroup: 1000, maxDynamicUniformBuffersPerPipelineLayout: 8, maxDynamicStorageBuffersPerPipelineLayout: 4, maxSampledTexturesPerShaderStage: 16,
                 maxSamplersPerShaderStage: 16, maxStorageBuffersPerShaderStage: 8, maxStorageTexturesPerShaderStage: 4, maxUniformBuffersPerShaderStage: 12, maxUniformBufferBindingSize: 65536,
                 maxStorageBufferBindingSize: 134217728, minUniformBufferOffsetAlignment: 256, minStorageBufferOffsetAlignment: 256, maxVertexBuffers: 8, maxBufferSize: 268435456,
                 maxVertexAttributes: 16, maxVertexBufferArrayStride: 2048, maxInterStageShaderVariables: 16, maxColorAttachments: 8, maxColorAttachmentBytesPerSample: 32,
                 maxComputeWorkgroupStorageSize: 16384, maxComputeInvocationsPerWorkgroup: 256, maxComputeWorkgroupSizeX: 256, maxComputeWorkgroupSizeY: 256, maxComputeWorkgroupSizeZ: 64,
                 maxComputeWorkgroupsPerDimension: 65535 };
  var newLimits = function () { var l = Object.create(GPUSupportedLimits.prototype); for (var k in LIMITS) Object.defineProperty(l, k, { value: LIMITS[k], enumerable: true }); return l; };
  var FEATURES = ['depth-clip-control', 'depth32float-stencil8', 'indirect-first-instance', 'bgra8unorm-storage', 'float32-filterable', 'rg11b10ufloat-renderable', 'timestamp-query'];
  var GPUSupportedFeatures = makeClass('GPUSupportedFeatures');
  var newFeatures = function (list) {
    var s = new Set(list);
    Object.setPrototypeOf(s, GPUSupportedFeatures.prototype);
    return s;
  };
  Object.setPrototypeOf(GPUSupportedFeatures.prototype, Set.prototype);

  // ------------------------------------------------------------------------------------------------------------------------------------ errors
  var GPUError = makeClass('GPUError');
  Object.defineProperty(GPUError.prototype, 'message', { get: function () { return this.__message; }, configurable: true, enumerable: true });
  var errorClass = function (name) {
    var C = function (message) { var e = Object.create(C.prototype); hide(e, '__message', String(message)); return e; };
    Object.defineProperty(C, 'name', { value: name });
    C.prototype = Object.create(GPUError.prototype);
    Object.defineProperty(C.prototype, Symbol.toStringTag, { value: name, configurable: true });
    Object.defineProperty(C.prototype, 'constructor', { value: C, writable: true, configurable: true });
    g[name] = C;
    return C;
  };
  var GPUValidationError = errorClass('GPUValidationError'), GPUOutOfMemoryError = errorClass('GPUOutOfMemoryError'), GPUInternalError = errorClass('GPUInternalError');
  g.GPUPipelineError = (function () {
    var C = function (message, init) { var e = new Error(message); e.name = 'GPUPipelineError'; e.reason = init && init.reason; return e; };
    return C;
  })();
  g.GPUUncapturedErrorEvent = function GPUUncapturedErrorEvent(type, init) {
    var e = new Event(type, init);
    Object.defineProperty(e, 'error', { value: init && init.error, enumerable: true });
    return e;
  };
  g.GPUDeviceLostInfo = function () { throw new TypeError('Illegal constructor'); };

  // ------------------------------------------------------------------------------------------------------------------------------------ the GPU
  var GPU = makeClass('GPU');
  var GPUAdapter = makeClass('GPUAdapter');
  var GPUAdapterInfo = makeClass('GPUAdapterInfo');
  var GPUDevice = makeClass('GPUDevice', Object.create((g.EventTarget || Object).prototype));
  var GPUQueue = makeClass('GPUQueue');
  var GPUBuffer = makeClass('GPUBuffer');
  var GPUTexture = makeClass('GPUTexture');
  var GPUTextureView = makeClass('GPUTextureView');
  var GPUSampler = makeClass('GPUSampler');
  var GPUBindGroupLayout = makeClass('GPUBindGroupLayout');
  var GPUPipelineLayout = makeClass('GPUPipelineLayout');
  var GPUBindGroup = makeClass('GPUBindGroup');
  var GPUShaderModule = makeClass('GPUShaderModule');
  var GPUComputePipeline = makeClass('GPUComputePipeline');
  var GPURenderPipeline = makeClass('GPURenderPipeline');
  var GPUCommandBuffer = makeClass('GPUCommandBuffer');
  var GPUCommandEncoder = makeClass('GPUCommandEncoder');
  var GPUComputePassEncoder = makeClass('GPUComputePassEncoder');
  var GPURenderPassEncoder = makeClass('GPURenderPassEncoder');
  var GPURenderBundle = makeClass('GPURenderBundle');
  var GPURenderBundleEncoder = makeClass('GPURenderBundleEncoder');
  var GPUQuerySet = makeClass('GPUQuerySet');
  var GPUCanvasContext = makeClass('GPUCanvasContext');
  var GPUExternalTexture = makeClass('GPUExternalTexture');
  [GPUBuffer, GPUTexture, GPUTextureView, GPUSampler, GPUBindGroupLayout, GPUPipelineLayout, GPUBindGroup, GPUShaderModule, GPUComputePipeline, GPURenderPipeline, GPUCommandBuffer,
   GPUCommandEncoder, GPUComputePassEncoder, GPURenderPassEncoder, GPURenderBundle, GPURenderBundleEncoder, GPUQuerySet, GPUDevice, GPUQueue].forEach(labelled);

  var gpu = make(GPU, {});
  def(GPU.prototype, 'requestAdapter', function (options) {
    var adapter = make(GPUAdapter, {});
    def(adapter, 'features', newFeatures(FEATURES));
    def(adapter, 'limits', newLimits());
    def(adapter, 'isFallbackAdapter', true);
    var info = make(GPUAdapterInfo, {});
    def(info, 'vendor', 'theorycraftos'); def(info, 'architecture', 'software'); def(info, 'device', 'rasterizer'); def(info, 'description', 'TheoryCraftOS software rasterizer');
    def(adapter, 'info', info);
    def(adapter, 'requestAdapterInfo', function () { return Promise.resolve(info); });
    def(adapter, 'requestDevice', function (desc) { return Promise.resolve(createDevice(adapter, desc)); });
    return Promise.resolve(adapter);
  });
  def(GPU.prototype, 'getPreferredCanvasFormat', function () { return 'bgra8unorm'; });
  getter(GPU.prototype, 'wgslLanguageFeatures', function () { return new Set(['readonly_and_readwrite_storage_textures', 'packed_4x8_integer_dot_product', 'unrestricted_pointer_parameters', 'pointer_composite_access']); });
  if (g.navigator) Object.defineProperty(g.navigator, 'gpu', { value: gpu, configurable: true, enumerable: true });
  g.__gpu_object = gpu;

  // ------------------------------------------------------------------------------------------------------------------------------------ the device
  var createDevice = function (adapter, desc) {
    var id = __gpu_create();
    var dev = Object.create(GPUDevice.prototype);
    if (g.EventTarget) { try { g.EventTarget.call(dev); } catch (e) {} }
    hide(dev, '__id', id); hide(dev, '__scopes', []); hide(dev, '__canvases', []); hide(dev, '__destroyed', false);
    hide(dev, '__pendingPresent', false);
    def(dev, 'features', newFeatures(desc && desc.requiredFeatures ? Array.prototype.slice.call(desc.requiredFeatures) : []));
    var lim = newLimits();
    if (desc && desc.requiredLimits) for (var k in desc.requiredLimits) Object.defineProperty(lim, k, { value: desc.requiredLimits[k], enumerable: true });
    def(dev, 'limits', lim);
    var lostResolve;
    def(dev, 'lost', new Promise(function (resolve) { lostResolve = resolve; }));
    hide(dev, '__lost', function (reason, message) { if (lostResolve) { var info = Object.create(g.GPUDeviceLostInfo.prototype); info.reason = reason; info.message = message; lostResolve(info); lostResolve = null; } });
    var queue = Object.create(GPUQueue.prototype);
    hide(queue, '__dev', dev);
    def(dev, 'queue', queue);
    def(dev, 'adapterInfo', adapter && adapter.info);
    hide(dev, '__handler', null);
    Object.defineProperty(dev, 'onuncapturederror', { get: function () { return this.__handler; }, set: function (f) { this.__handler = f; }, configurable: true, enumerable: true });
    return dev;
  };
  var D = GPUDevice.prototype;
  var op = function (dev, name) { return call.apply(null, [dev.__id, OP[name]].concat(Array.prototype.slice.call(arguments, 2))); };
  var deviceError = function (dev, type, message) {
    for (var i = dev.__scopes.length - 1; i >= 0; i--) {
      var s = dev.__scopes[i];
      if (s.filter === type) { if (!s.error) s.error = type === 'validation' ? GPUValidationError(message) : (type === 'out-of-memory' ? GPUOutOfMemoryError(message) : GPUInternalError(message)); return; }
    }
    var err = type === 'validation' ? GPUValidationError(message) : (type === 'out-of-memory' ? GPUOutOfMemoryError(message) : GPUInternalError(message));
    var ev = new g.GPUUncapturedErrorEvent('uncapturederror', { error: err, cancelable: true });
    if (dev.dispatchEvent) dev.dispatchEvent(ev);
    if (dev.__handler) dev.__handler.call(dev, ev);
    if (!ev.defaultPrevented && typeof console !== 'undefined') console.warn('WebGPU: ' + message);
  };
  var pullError = function (dev) { var e = op(dev, 'lastError'); if (e) deviceError(dev, 'validation', e); return e; };
  def(D, 'pushErrorScope', function (filter) { this.__scopes.push({ filter: filter, error: null }); });
  def(D, 'popErrorScope', function () {
    var s = this.__scopes.pop();
    if (!s) return Promise.reject(new DOMException('There is no error scope to pop.', 'OperationError'));
    return Promise.resolve(s.error);
  });
  def(D, 'destroy', function () { this.__destroyed = true; this.__lost('destroyed', 'The device was destroyed.'); });
  var assertDevice = function (dev, name) { if (!(dev instanceof GPUDevice)) throw new TypeError(name + ': not a GPUDevice'); };

  var toSize = function (s) {
    if (s && typeof s.length === 'number') return [s[0] || 1, s[1] === undefined ? 1 : s[1], s[2] === undefined ? 1 : s[2]];
    return [s.width || 1, s.height === undefined ? 1 : s.height, s.depthOrArrayLayers === undefined ? 1 : s.depthOrArrayLayers];
  };
  var toOrigin = function (o) {
    if (!o) return [0, 0, 0];
    if (typeof o.length === 'number') return [o[0] || 0, o[1] || 0, o[2] || 0];
    return [o.x || 0, o.y || 0, o.z || 0];
  };
  var bytesOf = function (data, offset, size) {
    var u8, bpe = data.BYTES_PER_ELEMENT || 1;
    if (data instanceof ArrayBuffer) u8 = new Uint8Array(data);
    else u8 = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    var start = (offset || 0) * (data instanceof ArrayBuffer ? 1 : bpe);
    var len = size === undefined ? u8.length - start : size * (data instanceof ArrayBuffer ? 1 : bpe);
    return [u8, start, len];
  };

  // buffers
  def(D, 'createBuffer', function (desc) {
    var id = op(this, 'createBuffer', desc.size, desc.usage);
    if (!id) pullError(this);
    var b = Object.create(GPUBuffer.prototype);
    hide(b, '__dev', this); hide(b, '__id', id); hide(b, '__label', desc.label || ''); hide(b, '__state', 'unmapped'); hide(b, '__mode', 0); hide(b, '__ranges', []); hide(b, '__abort', null); hide(b, '__created', false); hide(b, '__off', 0); hide(b, '__len', 0);
    def(b, 'size', desc.size); def(b, 'usage', desc.usage);
    if (desc.mappedAtCreation) { if (desc.size % 4 !== 0) deviceError(this, 'validation', 'A buffer mapped at creation needs a size that is a multiple of 4.'); b.__state = 'mapped'; b.__mode = 2; b.__off = 0; b.__len = desc.size; b.__created = true; }
    return b;
  });
  getter(GPUBuffer.prototype, 'mapState', function () { return this.__state; });
  def(GPUBuffer.prototype, 'mapAsync', function (mode, offset, size) {
    var b = this;
    offset = offset === undefined ? 0 : offset;
    if (b.__destroyed) return Promise.reject(new DOMException('The buffer is destroyed.', 'OperationError'));
    if (b.__state !== 'unmapped') return Promise.reject(new DOMException('The buffer is already mapped or being mapped.', 'OperationError'));
    if (mode !== 1 && mode !== 2) return Promise.reject(new TypeError("Failed to execute 'mapAsync' on 'GPUBuffer': mode must be GPUMapMode.READ or GPUMapMode.WRITE."));
    if (!(b.usage & (mode === 1 ? 1 : 2))) return Promise.reject(new DOMException('The buffer was not created with the usage this map needs.', 'OperationError'));
    size = size === undefined ? b.size - offset : size;
    if (offset % 8 !== 0 || size % 4 !== 0 || offset > b.size || offset + size > b.size) return Promise.reject(new DOMException('The mapped range is misaligned or out of the buffer.', 'OperationError'));
    b.__state = 'pending';
    return new Promise(function (resolve, reject) {
      b.__abort = reject;
      Promise.resolve().then(function () {
        if (b.__state !== 'pending') return;   // unmapped or destroyed meanwhile: already rejected
        b.__abort = null; b.__state = 'mapped'; b.__mode = mode; b.__off = offset; b.__len = size;
        resolve();
      });
    });
  });
  def(GPUBuffer.prototype, 'getMappedRange', function (offset, size) {
    if (this.__state !== 'mapped') throw new DOMException('The buffer is not mapped.', 'OperationError');
    offset = offset === undefined ? this.__off : offset;
    size = size === undefined ? this.__off + this.__len - offset : size;
    if (offset % 8 !== 0 || size % 4 !== 0 || offset < this.__off || offset + size > this.__off + this.__len)
      throw new DOMException('The requested range is misaligned or outside the mapped range.', 'OperationError');
    for (var i = 0; i < this.__ranges.length; i++) {
      var r = this.__ranges[i];
      if (offset < r.offset + r.ab.byteLength && r.offset < offset + size) throw new DOMException('The requested range overlaps one already returned.', 'OperationError');
    }
    var ab = new ArrayBuffer(size);
    if (this.__mode === 1 || this.__created) op(this.__dev, 'readBuffer', this.__id, offset, new Uint8Array(ab));
    this.__ranges.push({ offset: offset, ab: ab });
    return ab;
  });
  var endMap = function (b, write) {
    for (var i = 0; i < b.__ranges.length; i++) {
      var r = b.__ranges[i];
      if (write) op(b.__dev, 'writeBuffer', b.__id, r.offset, new Uint8Array(r.ab));
      op(b.__dev, 'detach', r.ab);   // script can no longer reach the mapped memory
    }
    b.__ranges = [];
    if (b.__abort) { var rej = b.__abort; b.__abort = null; rej(new DOMException('The map was cancelled.', 'AbortError')); }
    b.__state = 'unmapped'; b.__mode = 0; b.__created = false;
  };
  def(GPUBuffer.prototype, 'unmap', function () {
    if (this.__state === 'unmapped' && !this.__abort) return;
    endMap(this, this.__mode === 2);
  });
  def(GPUBuffer.prototype, 'destroy', function () { if (this.__state !== 'unmapped') endMap(this, false); op(this.__dev, 'destroyBuffer', this.__id); this.__destroyed = true; });

  // textures
  var texDim = { '1d': 0, '2d': 1, '3d': 2 };
  def(D, 'createTexture', function (desc) {
    var size = toSize(desc.size);
    var dim = desc.dimension || '2d';
    var id = op(this, 'createTexture', size[0], size[1], size[2], desc.mipLevelCount || 1, desc.sampleCount || 1, texDim[dim], desc.format, desc.usage);
    if (!id) pullError(this);
    var t = Object.create(GPUTexture.prototype);
    hide(t, '__dev', this); hide(t, '__id', id); hide(t, '__label', desc.label || '');
    def(t, 'width', size[0]); def(t, 'height', size[1]); def(t, 'depthOrArrayLayers', size[2]); def(t, 'mipLevelCount', desc.mipLevelCount || 1);
    def(t, 'sampleCount', desc.sampleCount || 1); def(t, 'dimension', dim); def(t, 'format', desc.format); def(t, 'usage', desc.usage);
    return t;
  });
  def(GPUTexture.prototype, 'createView', function (desc) {
    desc = desc || {};
    var dim = desc.dimension || (this.dimension === '3d' ? '3d' : (this.dimension === '1d' ? '1d' : (this.depthOrArrayLayers > 1 ? '2d-array' : '2d')));
    var baseMip = desc.baseMipLevel || 0;
    var mips = desc.mipLevelCount === undefined ? this.mipLevelCount - baseMip : desc.mipLevelCount;
    var baseLayer = desc.baseArrayLayer || 0;
    var layers = desc.arrayLayerCount === undefined ? (dim === 'cube' ? 6 : (dim === '2d-array' || dim === 'cube-array' ? this.depthOrArrayLayers - baseLayer : 1)) : desc.arrayLayerCount;
    var vid = op(this.__dev, 'createView', this.__id, { format: desc.format || '', dim: VIEW_DIM[dim], baseMip: baseMip, mipCount: mips, baseLayer: baseLayer, layerCount: layers, aspect: ASPECT[desc.aspect || 'all'] });
    if (!vid) pullError(this.__dev);
    var v = Object.create(GPUTextureView.prototype);
    hide(v, '__dev', this.__dev); hide(v, '__id', vid); hide(v, '__texture', this); hide(v, '__label', desc.label || '');
    return v;
  });
  def(GPUTexture.prototype, 'destroy', function () { op(this.__dev, 'destroyTexture', this.__id); });

  // samplers
  def(D, 'createSampler', function (desc) {
    desc = desc || {};
    var min = FILTER[desc.minFilter || 'nearest'], mag = FILTER[desc.magFilter || 'nearest'];
    var mipLinear = (desc.mipmapFilter || 'nearest') === 'linear';
    var glMin = min === 0x2600 ? (mipLinear ? 0x2702 : 0x2700) : (mipLinear ? 0x2703 : 0x2701);
    var s = { min: glMin, mag: mag, ws: ADDRESS[desc.addressModeU || 'clamp-to-edge'], wt: ADDRESS[desc.addressModeV || 'clamp-to-edge'], wr: ADDRESS[desc.addressModeW || 'clamp-to-edge'],
              lodMin: desc.lodMinClamp || 0, lodMax: desc.lodMaxClamp === undefined ? 32 : desc.lodMaxClamp, compare: desc.compare ? 0x200 + COMPARE[desc.compare] : 0 };
    var sp = Object.create(GPUSampler.prototype);
    hide(sp, '__dev', this); hide(sp, '__id', op(this, 'createSampler', s)); hide(sp, '__label', desc.label || '');
    return sp;
  });

  // shader modules
  def(D, 'createShaderModule', function (desc) {
    var r = op(this, 'createShaderModule', String(desc.code));
    var m = Object.create(GPUShaderModule.prototype);
    hide(m, '__dev', this); hide(m, '__id', r.id); hide(m, '__label', desc.label || ''); hide(m, '__error', r.error);
    if (r.error) deviceError(this, 'validation', 'Shader module: ' + r.error);
    return m;
  });
  def(GPUShaderModule.prototype, 'getCompilationInfo', function () {
    var messages = [];
    if (this.__error) {
      var line = /line (\d+)/.exec(this.__error);
      messages.push({ message: this.__error, type: 'error', lineNum: line ? +line[1] : 0, linePos: 0, offset: 0, length: 0 });
    }
    return Promise.resolve({ messages: messages });
  });

  // bind groups
  var stageBits = function (v) { return v === undefined ? 0 : v; };
  def(D, 'createBindGroupLayout', function (desc) {
    var entries = (desc.entries || []).map(function (e) {
      var kind = 0, bt = 0, dyn = 0, min = 0, st = 0, sample = 0, vd = 1, ms = 0, access = 0;
      if (e.buffer) { kind = 0; bt = { uniform: 0, storage: 1, 'read-only-storage': 2 }[e.buffer.type || 'uniform']; dyn = e.buffer.hasDynamicOffset ? 1 : 0; min = e.buffer.minBindingSize || 0; }
      else if (e.sampler) { kind = 1; st = { filtering: 0, 'non-filtering': 1, comparison: 2 }[e.sampler.type || 'filtering']; }
      else if (e.texture) { kind = 2; sample = { float: 0, 'unfilterable-float': 1, depth: 2, sint: 3, uint: 4 }[e.texture.sampleType || 'float']; vd = VIEW_DIM[e.texture.viewDimension || '2d']; ms = e.texture.multisampled ? 1 : 0; }
      else if (e.storageTexture) { kind = 3; access = { 'write-only': 0, 'read-only': 1, 'read-write': 2 }[e.storageTexture.access || 'write-only']; vd = VIEW_DIM[e.storageTexture.viewDimension || '2d']; }
      else if (e.externalTexture) kind = 4;
      return [e.binding, stageBits(e.visibility), kind, bt, dyn, min, st, sample, vd, ms, access];
    });
    var l = Object.create(GPUBindGroupLayout.prototype);
    hide(l, '__dev', this); hide(l, '__id', op(this, 'createBindGroupLayout', entries)); hide(l, '__label', desc.label || '');
    return l;
  });
  def(D, 'createPipelineLayout', function (desc) {
    var l = Object.create(GPUPipelineLayout.prototype);
    hide(l, '__dev', this); hide(l, '__id', op(this, 'createPipelineLayout', (desc.bindGroupLayouts || []).map(function (b) { return b.__id; }))); hide(l, '__label', desc.label || '');
    return l;
  });
  def(D, 'createBindGroup', function (desc) {
    var dev = this;
    var entries = (desc.entries || []).map(function (e) {
      var r = e.resource;
      if (r instanceof GPUSampler) return [e.binding, 1, r.__id, 0, 0];
      if (r instanceof GPUTextureView) return [e.binding, 2, r.__id, 0, 0];
      if (r instanceof GPUExternalTexture) return [e.binding, 2, r.__view.__id, 0, 0];
      var buf = r.buffer || r;
      return [e.binding, 0, buf.__id, r.offset || 0, r.size === undefined ? 0 : r.size];
    });
    var bg = Object.create(GPUBindGroup.prototype);
    var id = op(this, 'createBindGroup', desc.layout.__id, entries);
    if (!id) pullError(this);
    hide(bg, '__dev', this); hide(bg, '__id', id); hide(bg, '__label', desc.label || '');
    return bg;
  });

  // pipelines
  var constantList = function (c) { var list = []; if (c) for (var k in c) list.push([k, c[k]]); return list; };
  var blendOf = function (c) { return c ? [BLEND_OP[c.operation || 'add'], BLEND_FACTOR[c.srcFactor || 'one'], BLEND_FACTOR[c.dstFactor || 'zero']] : [0, 1, 0]; };
  var pipelineLayout = function (desc) { return desc.layout && desc.layout !== 'auto' ? desc.layout.__id : 0; };
  var makePipeline = function (dev, C, id, compute, desc) {
    var p = Object.create(C.prototype);
    hide(p, '__dev', dev); hide(p, '__id', id); hide(p, '__label', desc.label || ''); hide(p, '__compute', compute);
    return p;
  };
  def(D, 'createRenderPipeline', function (desc) {
    var v = desc.vertex || {}, f = desc.fragment, prim = desc.primitive || {}, ds = desc.depthStencil, ms = desc.multisample || {};
    var d = {
      layout: pipelineLayout(desc), vs: v.module.__id, vsEntry: v.entryPoint || '', vsConstants: constantList(v.constants),
      fs: f ? f.module.__id : 0, fsEntry: f ? (f.entryPoint || '') : '', fsConstants: constantList(f && f.constants),
      buffers: (v.buffers || []).map(function (b) {
        return b ? { stride: b.arrayStride, step: b.stepMode === 'instance' ? 1 : 0, attrs: (b.attributes || []).map(function (a) { return [a.shaderLocation, a.offset, VERTEX_FORMAT[a.format]]; }) } : { stride: 0, step: 0, attrs: [] };
      }),
      targets: f ? (f.targets || []).map(function (t) {
        if (!t) return { format: '', mask: 15 };
        var o = { format: t.format, mask: t.writeMask === undefined ? 15 : t.writeMask };
        if (t.blend) { o.color = blendOf(t.blend.color); o.alpha = blendOf(t.blend.alpha); }
        return o;
      }) : [],
      topology: TOPOLOGY[prim.topology || 'triangle-list'], stripFormat: prim.stripIndexFormat === 'uint16' ? 1 : (prim.stripIndexFormat === 'uint32' ? 2 : 0),
      ccw: (prim.frontFace || 'ccw') === 'ccw' ? 1 : 0, cull: { none: 0, front: 1, back: 2 }[prim.cullMode || 'none'], unclipped: prim.unclippedDepth ? 1 : 0, samples: ms.count || 1
    };
    if (ds) {
      var face = function (s) { s = s || {}; return [COMPARE[s.compare || 'always'], STENCIL_OP[s.failOp || 'keep'], STENCIL_OP[s.depthFailOp || 'keep'], STENCIL_OP[s.passOp || 'keep']]; };
      d.ds = { format: ds.format, depthWrite: ds.depthWriteEnabled ? 1 : 0, depthCompare: COMPARE[ds.depthCompare || 'always'], front: face(ds.stencilFront), back: face(ds.stencilBack),
               readMask: ds.stencilReadMask === undefined ? 0xFFFFFFFF : ds.stencilReadMask, writeMask: ds.stencilWriteMask === undefined ? 0xFFFFFFFF : ds.stencilWriteMask,
               bias: ds.depthBias || 0, biasSlope: ds.depthBiasSlopeScale || 0, biasClamp: ds.depthBiasClamp || 0 };
    }
    var id = op(this, 'createRenderPipeline', d);
    if (!id) pullError(this);
    return makePipeline(this, GPURenderPipeline, id, false, desc);
  });
  def(D, 'createComputePipeline', function (desc) {
    var c = desc.compute || {};
    var id = op(this, 'createComputePipeline', { layout: pipelineLayout(desc), module: c.module.__id, entry: c.entryPoint || '', constants: constantList(c.constants) });
    if (!id) pullError(this);
    return makePipeline(this, GPUComputePipeline, id, true, desc);
  });
  def(D, 'createRenderPipelineAsync', function (desc) { var dev = this; return Promise.resolve().then(function () { return dev.createRenderPipeline(desc); }); });
  def(D, 'createComputePipelineAsync', function (desc) { var dev = this; return Promise.resolve().then(function () { return dev.createComputePipeline(desc); }); });
  var getBgl = function (i) {
    var id = op(this.__dev, 'getBindGroupLayout', this.__id, this.__compute ? 1 : 0, i);
    var l = Object.create(GPUBindGroupLayout.prototype);
    hide(l, '__dev', this.__dev); hide(l, '__id', id); hide(l, '__label', '');
    return l;
  };
  def(GPURenderPipeline.prototype, 'getBindGroupLayout', getBgl);
  def(GPUComputePipeline.prototype, 'getBindGroupLayout', getBgl);
  def(D, 'createQuerySet', function (desc) {
    var q = Object.create(GPUQuerySet.prototype);
    hide(q, '__dev', this); hide(q, '__id', op(this, 'createQuerySet', desc.type === 'timestamp' ? 1 : 0, desc.count)); hide(q, '__label', desc.label || '');
    def(q, 'type', desc.type); def(q, 'count', desc.count);
    def(q, 'destroy', function () {});
    return q;
  });
  def(D, 'importExternalTexture', function (desc) {
    var e = Object.create(GPUExternalTexture.prototype);
    var tex = this.createTexture({ size: [desc.source.videoWidth || desc.source.width || 1, desc.source.videoHeight || desc.source.height || 1], format: 'rgba8unorm', usage: 4 | 2 | 16 });
    this.queue.copyExternalImageToTexture({ source: desc.source }, { texture: tex }, [tex.width, tex.height]);
    hide(e, '__view', tex.createView());
    return e;
  });

  // ------------------------------------------------------------------------------------------------------------------------------------ command encoding
  var passCommon = function (P) {
    def(P, 'setPipeline', function (p) { this.__cmds.push([4, p.__id]); });
    def(P, 'setBindGroup', function (index, group, a, b, c) {
      var offsets = [];
      if (a && typeof a.length === 'number') {
        if (typeof b === 'number') { for (var i = 0; i < c; i++) offsets.push(a[b + i]); }
        else for (var j = 0; j < a.length; j++) offsets.push(a[j]);
      }
      this.__cmds.push([5, index, group ? group.__id : 0, offsets]);
    });
    def(P, 'pushDebugGroup', function () {}); def(P, 'popDebugGroup', function () {}); def(P, 'insertDebugMarker', function () {});
  };
  var renderCommon = function (P) {
    passCommon(P);
    def(P, 'setVertexBuffer', function (slot, buf, offset, size) { this.__cmds.push([6, slot, buf ? buf.__id : 0, offset || 0, size === undefined ? 0 : size]); });
    def(P, 'setIndexBuffer', function (buf, format, offset, size) { this.__cmds.push([7, buf.__id, format === 'uint16' ? 0 : 1, offset || 0, size === undefined ? 0 : size]); });
    def(P, 'draw', function (vc, ic, fv, fi) { this.__cmds.push([8, vc, ic === undefined ? 1 : ic, fv || 0, fi || 0]); });
    def(P, 'drawIndexed', function (ic, inst, fi, bv, finst) { this.__cmds.push([9, ic, inst === undefined ? 1 : inst, fi || 0, bv || 0, finst || 0]); });
    def(P, 'drawIndirect', function (buf, off) { this.__cmds.push([24, buf.__id, off]); });
    def(P, 'drawIndexedIndirect', function (buf, off) { this.__cmds.push([25, buf.__id, off]); });
  };
  renderCommon(GPURenderPassEncoder.prototype);
  renderCommon(GPURenderBundleEncoder.prototype);
  passCommon(GPUComputePassEncoder.prototype);
  var RP = GPURenderPassEncoder.prototype;
  def(RP, 'setViewport', function (x, y, w, h, mn, mx) { this.__cmds.push([11, x, y, w, h, mn, mx]); });
  def(RP, 'setScissorRect', function (x, y, w, h) { this.__cmds.push([12, x, y, w, h]); });
  def(RP, 'setBlendConstant', function (c) { var a = c && typeof c.length === 'number' ? c : [c.r, c.g, c.b, c.a]; this.__cmds.push([13, a[0], a[1], a[2], a[3]]); });
  def(RP, 'setStencilReference', function (n) { this.__cmds.push([14, n]); });
  def(RP, 'beginOcclusionQuery', function (i) { this.__cmds.push([20, i]); });
  def(RP, 'endOcclusionQuery', function () { this.__cmds.push([21]); });
  def(RP, 'executeBundles', function (bundles) {
    for (var i = 0; i < bundles.length; i++) for (var k = 0; k < bundles[i].__cmds.length; k++) this.__cmds.push(bundles[i].__cmds[k]);
  });
  def(RP, 'end', function () { this.__cmds.push([2]); this.__enc.__open = false; });
  def(GPUComputePassEncoder.prototype, 'dispatchWorkgroups', function (x, y, z) { this.__cmds.push([10, x, y === undefined ? 1 : y, z === undefined ? 1 : z]); });
  def(GPUComputePassEncoder.prototype, 'dispatchWorkgroupsIndirect', function (buf, off) { this.__cmds.push([26, buf.__id, off]); });
  def(GPUComputePassEncoder.prototype, 'end', function () { this.__cmds.push([2]); this.__enc.__open = false; });
  def(GPURenderBundleEncoder.prototype, 'finish', function (desc) { var b = Object.create(GPURenderBundle.prototype); hide(b, '__cmds', this.__cmds); hide(b, '__label', (desc && desc.label) || ''); return b; });
  def(D, 'createRenderBundleEncoder', function (desc) { var e = Object.create(GPURenderBundleEncoder.prototype); hide(e, '__cmds', []); hide(e, '__label', (desc && desc.label) || ''); return e; });

  def(D, 'createCommandEncoder', function (desc) {
    var e = Object.create(GPUCommandEncoder.prototype);
    hide(e, '__dev', this); hide(e, '__cmds', []); hide(e, '__open', false); hide(e, '__label', (desc && desc.label) || ''); hide(e, '__finished', false);
    return e;
  });
  var CE = GPUCommandEncoder.prototype;
  var clearOf = function (c) { return c && typeof c.length === 'number' ? [c[0], c[1], c[2], c[3]] : (c ? [c.r || 0, c.g || 0, c.b || 0, c.a || 0] : [0, 0, 0, 0]); };
  def(CE, 'beginRenderPass', function (desc) {
    var enc = this;
    var d = { colors: (desc.colorAttachments || []).filter(function (c) { return c; }).map(function (c) {
      return { view: c.view.__id, resolve: c.resolveTarget ? c.resolveTarget.__id : 0, load: c.loadOp === 'load' ? 1 : 0, store: c.storeOp === 'discard' ? 0 : 1, clear: clearOf(c.clearValue) };
    }) };
    var ds = desc.depthStencilAttachment;
    if (ds) {
      d.depthView = ds.view.__id;
      d.depthLoad = ds.depthLoadOp === 'load' ? 1 : 0; d.depthStore = ds.depthStoreOp === 'discard' ? 0 : 1; d.depthReadOnly = ds.depthReadOnly ? 1 : 0;
      d.depthClear = ds.depthClearValue === undefined ? 1 : ds.depthClearValue;
      d.stencilLoad = ds.stencilLoadOp === 'load' ? 1 : 0; d.stencilStore = ds.stencilStoreOp === 'discard' ? 0 : 1; d.stencilReadOnly = ds.stencilReadOnly ? 1 : 0;
      d.stencilClear = ds.stencilClearValue || 0;
    }
    if (desc.occlusionQuerySet) d.occlusionSet = desc.occlusionQuerySet.__id;
    this.__cmds.push([1, d]);
    this.__open = true;
    var p = Object.create(RP);
    hide(p, '__cmds', this.__cmds); hide(p, '__enc', enc); hide(p, '__label', desc.label || '');
    return p;
  });
  def(CE, 'beginComputePass', function (desc) {
    this.__cmds.push([3]);
    this.__open = true;
    var p = Object.create(GPUComputePassEncoder.prototype);
    hide(p, '__cmds', this.__cmds); hide(p, '__enc', this); hide(p, '__label', (desc && desc.label) || '');
    return p;
  });
  def(CE, 'copyBufferToBuffer', function (src, so, dst, dof, size) { this.__cmds.push([15, src.__id, so, dst.__id, dof, size]); });
  def(CE, 'copyBufferToTexture', function (s, d, size) {
    var o = toOrigin(d.origin), sz = toSize(size);
    this.__cmds.push([16, s.buffer.__id, s.offset || 0, s.bytesPerRow || 0, s.rowsPerImage || 0, d.texture.__id, d.mipLevel || 0, o[0], o[1], o[2], ASPECT[d.aspect || 'all'], sz[0], sz[1], sz[2]]);
  });
  def(CE, 'copyTextureToBuffer', function (s, d, size) {
    var o = toOrigin(s.origin), sz = toSize(size);
    this.__cmds.push([17, s.texture.__id, s.mipLevel || 0, o[0], o[1], o[2], ASPECT[s.aspect || 'all'], d.buffer.__id, d.offset || 0, d.bytesPerRow || 0, d.rowsPerImage || 0, sz[0], sz[1], sz[2]]);
  });
  def(CE, 'copyTextureToTexture', function (s, d, size) {
    var so = toOrigin(s.origin), dofs = toOrigin(d.origin), sz = toSize(size);
    this.__cmds.push([18, s.texture.__id, s.mipLevel || 0, so[0], so[1], so[2], ASPECT[s.aspect || 'all'], d.texture.__id, d.mipLevel || 0, dofs[0], dofs[1], dofs[2], ASPECT[d.aspect || 'all'], sz[0], sz[1], sz[2]]);
  });
  def(CE, 'clearBuffer', function (buf, off, size) { this.__cmds.push([19, buf.__id, off || 0, size === undefined ? 0 : size]); });
  def(CE, 'writeTimestamp', function (qs, i) { this.__cmds.push([22, qs.__id, i]); });
  def(CE, 'resolveQuerySet', function (qs, first, count, dst, dof) { this.__cmds.push([23, qs.__id, first, count, dst.__id, dof]); });
  def(CE, 'pushDebugGroup', function () {}); def(CE, 'popDebugGroup', function () {}); def(CE, 'insertDebugMarker', function () {});
  def(CE, 'finish', function (desc) {
    if (this.__finished) deviceError(this.__dev, 'validation', 'The command encoder is already finished.');
    if (this.__open) deviceError(this.__dev, 'validation', 'A pass is still open.');
    this.__finished = true;
    var cb = Object.create(GPUCommandBuffer.prototype);
    hide(cb, '__cmds', this.__cmds); hide(cb, '__label', (desc && desc.label) || ''); hide(cb, '__dev', this.__dev);
    return cb;
  });

  // ------------------------------------------------------------------------------------------------------------------------------------ the queue
  var Q = GPUQueue.prototype;
  var schedulePresent = function (dev) {
    if (dev.__pendingPresent || dev.__canvases.length === 0) return;
    dev.__pendingPresent = true;
    Promise.resolve().then(function () {
      dev.__pendingPresent = false;
      for (var i = 0; i < dev.__canvases.length; i++) {
        var c = dev.__canvases[i];
        if (c.__tex && c.__used) { __gpu_present(dev.__id, c.__tex.__id, c.canvas, c.__alpha === 'premultiplied'); c.__used = false; c.__presented = true; }
      }
    });
  };
  def(Q, 'submit', function (buffers) {
    var dev = this.__dev;
    var lists = [];
    for (var i = 0; i < buffers.length; i++) lists.push(buffers[i].__cmds);
    if (!op(dev, 'submit', lists)) pullError(dev);
    schedulePresent(dev);
  });
  def(Q, 'writeBuffer', function (buf, offset, data, dataOffset, size) {
    if (buf.__state !== 'unmapped') { deviceError(this.__dev, 'validation', 'The buffer is mapped.'); return; }
    var b = bytesOf(data, dataOffset, size);
    if (!op(this.__dev, 'writeBuffer', buf.__id, offset, b[0], b[1], b[2])) pullError(this.__dev);
  });
  def(Q, 'writeTexture', function (dest, data, layout, size) {
    var o = toOrigin(dest.origin), sz = toSize(size), b = bytesOf(data, 0);
    var view = b[0].subarray(b[1], b[1] + b[2]);
    if (!op(this.__dev, 'writeTexture', dest.texture.__id, dest.mipLevel || 0, o[0], o[1], o[2], ASPECT[dest.aspect || 'all'], view, (layout.offset || 0), layout.bytesPerRow || 0, layout.rowsPerImage || 0, sz[0], sz[1], 1)) pullError(this.__dev);
    // (depth/array layers are copied by a second call each; see below)
    if (sz[2] > 1) {
      for (var z = 1; z < sz[2]; z++)
        op(this.__dev, 'writeTexture', dest.texture.__id, dest.mipLevel || 0, o[0], o[1], o[2] + z, ASPECT[dest.aspect || 'all'], view, (layout.offset || 0) + z * (layout.rowsPerImage || sz[1]) * (layout.bytesPerRow || 0), layout.bytesPerRow || 0, layout.rowsPerImage || 0, sz[0], sz[1], 1);
    }
  });
  def(Q, 'copyExternalImageToTexture', function (src, dest, size) {
    var o = toOrigin(dest.origin), so = toOrigin(src.origin), sz = toSize(size);
    if (!op(this.__dev, 'copyExternal', dest.texture.__id, dest.mipLevel || 0, o[0], o[1], o[2], src.source, src.flipY ? 1 : 0, dest.premultipliedAlpha ? 1 : 0)) pullError(this.__dev);
  });
  def(Q, 'onSubmittedWorkDone', function () { return Promise.resolve(); });

  // ------------------------------------------------------------------------------------------------------------------------------------ the canvas context
  g.__make_gpu_context = function (kind, attrs) {
    var canvas = this;
    var ctx = Object.create(GPUCanvasContext.prototype);
    def(ctx, 'canvas', canvas);
    hide(ctx, '__dev', null); hide(ctx, '__tex', null); hide(ctx, '__used', false); hide(ctx, '__presented', false); hide(ctx, '__alpha', 'opaque'); hide(ctx, '__config', null);
    hide(ctx, '__w', 0); hide(ctx, '__h', 0);
    return ctx;
  };
  var CC = GPUCanvasContext.prototype;
  def(CC, 'configure', function (config) {
    if (!config || !config.device) throw new TypeError("Failed to execute 'configure' on 'GPUCanvasContext': device is required.");
    if (this.__dev) this.unconfigure();
    this.__dev = config.device;
    this.__config = config;
    this.__alpha = config.alphaMode || 'opaque';
    config.device.__canvases.push(this);
  });
  def(CC, 'unconfigure', function () {
    if (this.__dev) { var i = this.__dev.__canvases.indexOf(this); if (i >= 0) this.__dev.__canvases.splice(i, 1); }
    this.__dev = null; this.__tex = null; this.__config = null;
  });
  def(CC, 'getConfiguration', function () { return this.__config ? { device: this.__config.device, format: this.__config.format, usage: this.__config.usage || 16, alphaMode: this.__alpha, viewFormats: [] } : null; });
  def(CC, 'getCurrentTexture', function () {
    if (!this.__dev) throw new DOMException('The canvas context is not configured.', 'InvalidStateError');
    var w = this.canvas.width, h = this.canvas.height;
    if (!this.__tex || this.__w !== w || this.__h !== h) {
      this.__tex = this.__dev.createTexture({ size: [w, h], format: this.__config.format, usage: (this.__config.usage || 16) | 1 | 2 | 4 });
      this.__w = w; this.__h = h;
    } else if (this.__presented) {
      // a new frame starts transparent black
      var enc = this.__dev.createCommandEncoder();
      var pass = enc.beginRenderPass({ colorAttachments: [{ view: this.__tex.createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 0] }] });
      pass.end();
      this.__dev.queue.submit([enc.finish()]);
    }
    this.__presented = false;
    this.__used = true;
    return this.__tex;
  });
})(this);
)JSPRELUDE";

}  // namespace

const char* webgpu_prelude() { return kWebGpuPrelude; }

}  // namespace browser
