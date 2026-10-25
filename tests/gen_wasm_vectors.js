// Generates tests/data/wasm_vectors.txt for tests/WasmTest.cpp: small WebAssembly modules (assembled here) together with what a reference engine (the WebAssembly of
// the node that runs this script) gives for calls into them. The C++ test replays the calls against the runtime in src/Wasm and compares.
//
//   node tests/gen_wasm_vectors.js > tests/data/wasm_vectors.txt
//
// Lines:  module <hex>             a module the following calls go to
//         call <name> <args> => <results>     or   => trap
//         invalid <hex>            a module that must not validate
//         valid <hex>              a module that must validate
// Values: i32:<signed decimal>  i64:<signed decimal>  f32:0x<bits>  f64:0x<bits>
'use strict';

const out = [];
const I32 = 0x7f, I64 = 0x7e, F32 = 0x7d, F64 = 0x7c, V128 = 0x7b, FUNCREF = 0x70, EXTERNREF = 0x6f;

// --- an assembler -----------------------------------------------------------------------------------------------------------------
const u32 = (n) => { const b = []; n = BigInt(n); do { let x = Number(n & 0x7fn); n >>= 7n; if (n) x |= 0x80; b.push(x); } while (n); return b; };
const sleb = (n) => { n = BigInt(n); const b = []; for (;;) { const x = Number(n & 0x7fn); n >>= 7n; if ((n === 0n && !(x & 0x40)) || (n === -1n && (x & 0x40))) { b.push(x); return b; } b.push(x | 0x80); } };
const vec = (items) => [...u32(items.length), ...items.flat(Infinity)];
const str = (s) => vec([...Buffer.from(s)].map((c) => [c]));
const section = (id, bytes) => [id, ...u32(bytes.length), ...bytes];
const f32bytes = (v) => [...new Uint8Array(new Float32Array([v]).buffer)];
const f64bytes = (v) => [...new Uint8Array(new Float64Array([v]).buffer)];
const i32c = (v) => [0x41, ...sleb(v | 0)];
const i64c = (v) => [0x42, ...sleb(BigInt.asIntN(64, BigInt(v)))];
const f32c = (v) => [0x43, ...f32bytes(v)];
const f64c = (v) => [0x44, ...f64bytes(v)];
const lget = (i) => [0x20, ...u32(i)], lset = (i) => [0x21, ...u32(i)], ltee = (i) => [0x22, ...u32(i)];
const gget = (i) => [0x23, ...u32(i)], gset = (i) => [0x24, ...u32(i)];
const call = (i) => [0x10, ...u32(i)];
const END = 0x0b;
// blocktype: undefined (empty), a value type, or {type: index}
const bt = (t) => (t === undefined ? [0x40] : typeof t === 'object' ? sleb(t.type) : [t]);
const block = (t, ...body) => [0x02, ...bt(t), ...body.flat(Infinity), END];
const loop = (t, ...body) => [0x03, ...bt(t), ...body.flat(Infinity), END];
const iff = (t, then, els) => [0x04, ...bt(t), ...then.flat(Infinity), ...(els ? [0x05, ...els.flat(Infinity)] : []), END];
const br = (n) => [0x0c, ...u32(n)], brif = (n) => [0x0d, ...u32(n)];
const brtable = (labels, d) => [0x0e, ...vec(labels.map((l) => u32(l))), ...u32(d)];
const mem = (op, align, off) => [op, ...u32(align), ...u32(off)];
const fc = (n, ...imm) => [0xfc, ...u32(n), ...imm.flat(Infinity)];
const simd = (n, ...imm) => [0xfd, ...u32(n), ...imm.flat(Infinity)];
const vconst = (lo, hi) => { const b = []; for (const w of [lo, hi]) { let x = BigInt.asUintN(64, BigInt(w)); for (let i = 0; i < 8; i++) { b.push(Number(x & 0xffn)); x >>= 8n; } } return simd(12, b); };

function limits(min, max, shared) { return max === undefined ? [0, ...u32(min)] : [shared ? 3 : 1, ...u32(min), ...u32(max)]; }

// spec: {types: [[params],[results]], imports: [[mod,name,kind,desc]], funcs: [{type, locals:[[n,t]], body}], tables: [[reftype, min, max]], mems: [[min,max]], globals: [[type, mut, initbytes]],
//        exports: [[name, kind, index]], start, elems: [bytes], datas: [bytes], datacount}
function module(spec) {
  const m = [0, 0x61, 0x73, 0x6d, 1, 0, 0, 0];
  const types = spec.types || [];
  if (types.length) m.push(...section(1, vec(types.map(([p, r]) => [0x60, ...vec(p.map((x) => [x])), ...vec(r.map((x) => [x]))]))));
  if (spec.imports) m.push(...section(2, vec(spec.imports.map(([mod, name, kind, desc]) => [...str(mod), ...str(name), kind, ...desc]))));
  const funcs = spec.funcs || [];
  if (funcs.length) m.push(...section(3, vec(funcs.map((f) => u32(f.type)))));
  if (spec.tables) m.push(...section(4, vec(spec.tables.map(([t, min, max]) => [t, ...limits(min, max)]))));
  if (spec.mems) m.push(...section(5, vec(spec.mems.map(([min, max]) => limits(min, max)))));
  if (spec.globals) m.push(...section(6, vec(spec.globals.map(([t, mut, init]) => [t, mut, ...init, END]))));
  if (spec.exports) m.push(...section(7, vec(spec.exports.map(([n, k, i]) => [...str(n), k, ...u32(i)]))));
  if (spec.start !== undefined) m.push(...section(8, u32(spec.start)));
  if (spec.elems) m.push(...section(9, vec(spec.elems)));
  if (spec.datacount !== undefined) m.push(...section(12, u32(spec.datacount)));
  if (funcs.length) m.push(...section(10, vec(funcs.map((f) => { const body = [...vec((f.locals || []).map(([n, t]) => [...u32(n), t])), ...f.body.flat(Infinity), END]; return [...u32(body.length), ...body]; }))));
  if (spec.datas) m.push(...section(11, vec(spec.datas)));
  return Uint8Array.from(m);
}
const hex = (b) => Buffer.from(b).toString('hex');

// --- values ------------------------------------------------------------------------------------------------------------------------
const f32buf = new Float32Array(1), u32buf = new Uint32Array(f32buf.buffer);
const f64buf = new Float64Array(1), u64buf = new BigUint64Array(f64buf.buffer);
const tokI32 = (v) => `i32:${v | 0}`;
const tokI64 = (v) => `i64:${BigInt.asIntN(64, BigInt(v))}`;
const tokF32 = (v) => { f32buf[0] = v; return `f32:0x${u32buf[0].toString(16).padStart(8, '0')}`; };
const tokF64 = (v) => { f64buf[0] = v; return `f64:0x${u64buf[0].toString(16).padStart(16, '0')}`; };
const tok = (t, v) => (t === I32 ? tokI32(v) : t === I64 ? tokI64(v) : t === F32 ? tokF32(v) : tokF64(v));

// Runs `name` of instance on args (JS values) and prints the line.
function emitCall(inst, name, ptypes, rtypes, args) {
  let line = `call ${name}`;
  for (let i = 0; i < ptypes.length; i++) line += ' ' + tok(ptypes[i], args[i]);
  line += ' =>';
  try {
    let r = inst.exports[name](...args);
    if (rtypes.length === 0) r = [];
    else if (rtypes.length === 1) r = [r];
    for (let i = 0; i < rtypes.length; i++) line += ' ' + tok(rtypes[i], r[i]);
  } catch (e) {
    if (e instanceof WebAssembly.RuntimeError || e instanceof RangeError) line += ' trap';
    else throw e;
  }
  out.push(line);
}

function emitModule(bytes, imports) {
  out.push('module ' + hex(bytes));
  return new WebAssembly.Instance(new WebAssembly.Module(bytes), imports || {});
}

// --- 1. every scalar numeric operation ------------------------------------------------------------------------------------------------
const i32s = [0, 1, -1, 2, 7, 31, 32, 0x7fffffff, -0x80000000, 0x12345678, 0xdeadbeef | 0, 255, -256];
const i64s = [0n, 1n, -1n, 2n, 63n, 64n, 0x7fffffffffffffffn, -0x8000000000000000n, 0x123456789abcdef0n, 0xffffffffn, 0x100000000n, -2n];
const f32s = [0, -0, 1, -1, 0.5, 1.5, 2.5, -2.5, Math.fround(1e-45), Math.fround(3.4028235e38), Infinity, -Infinity, NaN, 16777216, 8388607.5, 2147483648, 4294967296,
  -2147483904, -2147483648, Math.fround(100.7), Math.fround(9.3e18), Math.fround(1.8446744e19), -0.9999999];
const f64s = [0, -0, 1, -1, 0.5, 1.5, 2.5, -2.5, 5e-324, 1.7976931348623157e308, Infinity, -Infinity, NaN, 4503599627370496, 4503599627370495.5, 2147483648, 4294967296, 2147483647.9,
  -2147483648.9, -2147483649, 4294967295.9, 100.7, 9223372036854775807, 9223372036854775808, 1.8446744073709552e19, 18446744073709549568, -0.99999999999];
const pool = (t) => (t === I32 ? i32s : t === I64 ? i64s : t === F32 ? f32s : f64s);

function numericSig(op) {
  if (op === 0x45) return [[I32], I32];
  if (op >= 0x46 && op <= 0x4f) return [[I32, I32], I32];
  if (op === 0x50) return [[I64], I32];
  if (op >= 0x51 && op <= 0x5a) return [[I64, I64], I32];
  if (op >= 0x5b && op <= 0x60) return [[F32, F32], I32];
  if (op >= 0x61 && op <= 0x66) return [[F64, F64], I32];
  if (op >= 0x67 && op <= 0x69) return [[I32], I32];
  if (op >= 0x6a && op <= 0x78) return [[I32, I32], I32];
  if (op >= 0x79 && op <= 0x7b) return [[I64], I64];
  if (op >= 0x7c && op <= 0x8a) return [[I64, I64], I64];
  if (op >= 0x8b && op <= 0x91) return [[F32], F32];
  if (op >= 0x92 && op <= 0x98) return [[F32, F32], F32];
  if (op >= 0x99 && op <= 0x9f) return [[F64], F64];
  if (op >= 0xa0 && op <= 0xa6) return [[F64, F64], F64];
  const t = { 0xa7: [I64, I32], 0xa8: [F32, I32], 0xa9: [F32, I32], 0xaa: [F64, I32], 0xab: [F64, I32], 0xac: [I32, I64], 0xad: [I32, I64], 0xae: [F32, I64], 0xaf: [F32, I64], 0xb0: [F64, I64], 0xb1: [F64, I64],
    0xb2: [I32, F32], 0xb3: [I32, F32], 0xb4: [I64, F32], 0xb5: [I64, F32], 0xb6: [F64, F32], 0xb7: [I32, F64], 0xb8: [I32, F64], 0xb9: [I64, F64], 0xba: [I64, F64], 0xbb: [F32, F64],
    0xbc: [F32, I32], 0xbd: [F64, I64], 0xbe: [I32, F32], 0xbf: [I64, F64], 0xc0: [I32, I32], 0xc1: [I32, I32], 0xc2: [I64, I64], 0xc3: [I64, I64], 0xc4: [I64, I64] }[op];
  return [[t[0]], t[1]];
}

function numericTests() {
  const ops = [];
  for (let op = 0x45; op <= 0xc4; op++) ops.push([op, [op]]);
  for (let n = 0; n < 8; n++) ops.push([0x100 + n, [0xfc, n]]);
  for (const [id, enc] of ops) {
    let params, result;
    if (id >= 0x100) { const n = id - 0x100; params = [n % 4 < 2 ? F32 : F64]; result = n < 4 ? I32 : I64; } else [params, result] = numericSig(id);
    const bytes = module({ types: [[params, [result]]], funcs: [{ type: 0, body: [params.map((_, i) => lget(i)), enc] }], exports: [['f', 0, 0]] });
    const inst = emitModule(bytes);
    const list = params.length === 1 ? pool(params[0]).map((a) => [a]) : pool(params[0]).flatMap((a) => pool(params[1]).map((b) => [a, b]));
    for (const args of list) emitCall(inst, 'f', params, [result], args);
  }
}

// --- 2. control flow, memory, tables, calls --------------------------------------------------------------------------------------------
function program(spec, calls) {
  const inst = emitModule(module(spec));
  for (const [name, ptypes, rtypes, argLists] of calls) for (const args of argLists) emitCall(inst, name, ptypes, rtypes, args);
}

function controlTests() {
  // fib, recursion; sum with loop; factorial i64
  program({
    types: [[[I32], [I32]], [[I64], [I64]], [[], []]],
    funcs: [
      { type: 0, body: [lget(0), i32c(2), [0x48], iff(I32, [lget(0)], [lget(0), i32c(1), [0x6b], call(0), lget(0), i32c(2), [0x6b], call(0), [0x6a]])] },
      { type: 0, locals: [[1, I32]], body: [block(undefined, loop(undefined, lget(0), [0x45], brif(1), lget(1), lget(0), [0x6a], lset(1), lget(0), i32c(1), [0x6b], lset(0), br(0))), lget(1)] },
      { type: 1, body: [lget(0), i64c(1), [0x57], iff(I64, [i64c(1)], [lget(0), lget(0), i64c(1), [0x7d], call(2), [0x7e]])] },
      { type: 2, body: [[0x00]] },
    ],
    exports: [['fib', 0, 0], ['sum', 0, 1], ['fact', 0, 2], ['boom', 0, 3]],
  }, [
    ['fib', [I32], [I32], [0, 1, 2, 5, 10, 20].map((n) => [n])],
    ['sum', [I32], [I32], [0, 1, 10, 100, 1000].map((n) => [n])],
    ['fact', [I64], [I64], [0n, 1n, 5n, 20n, 25n].map((n) => [n])],
    ['boom', [], [], [[]]],
  ]);
  // br_table switch, nested blocks with values, br out of a loop with a value
  program({
    types: [[[I32], [I32]]],
    funcs: [
      { type: 0, body: [block(I32, block(I32, block(I32, block(I32, i32c(100), lget(0), brtable([0, 1, 2], 3)), i32c(1), [0x6a], br(2)), i32c(10), [0x6a], br(1)), i32c(1000), [0x6a])] },
      { type: 0, locals: [[1, I32]], body: [loop(I32, lget(1), i32c(1), [0x6a], ltee(1), lget(0), [0x4e], iff(undefined, [lget(1), i32c(7), [0x6c], br(2)]), br(0))] },
    ],
    exports: [['sw', 0, 0], ['lp', 0, 1]],
  }, [['sw', [I32], [I32], [-1, 0, 1, 2, 3, 4, 100].map((n) => [n])], ['lp', [I32], [I32], [0, 1, 5, 9].map((n) => [n])]]);
  // multi-value: swap, block params, if params, loops with params
  program({
    types: [[[I32, I32], [I32, I32]], [[I32, I32], [I32]], [[I32], [I32, I32, I32]]],
    funcs: [
      { type: 0, body: [lget(1), lget(0)] },
      { type: 1, body: [lget(0), lget(1), block({ type: 1 }, [0x6b])] },
      { type: 1, body: [lget(0), lget(1), lget(0), lget(1), [0x48], iff({ type: 1 }, [[0x6a]], [[0x6c]])] },
      { type: 2, body: [lget(0), lget(0), i32c(1), [0x6a], lget(0), i32c(2), [0x6a]] },
      { type: 1, body: [lget(0), lget(1), loop({ type: 1 }, [0x6a])] },
      { type: 0, body: [lget(0), lget(1), call(0), call(0), call(0)] },
    ],
    exports: [['swap', 0, 0], ['sub', 0, 1], ['pick', 0, 2], ['three', 0, 3], ['addloop', 0, 4], ['swap3', 0, 5]],
  }, [
    ['swap', [I32, I32], [I32, I32], [[1, 2], [-5, 9]]],
    ['sub', [I32, I32], [I32], [[10, 3], [3, 10]]],
    ['pick', [I32, I32], [I32], [[3, 4], [4, 3], [5, 5]]],
    ['three', [I32], [I32, I32, I32], [[5], [-1]]],
    ['addloop', [I32, I32], [I32], [[4, 5]]],
    ['swap3', [I32, I32], [I32, I32], [[7, 8]]],
  ]);
  // select, drop, tee, locals of every type, unreachable code after br/return, deep blocks
  program({
    types: [[[I32, I64, F32, F64], [F64, F32, I64, I32]], [[I32, I32, I32], [I32]], [[I32], [I32]]],
    funcs: [
      { type: 0, locals: [[2, F64]], body: [lget(3), lget(2), lget(1), lget(0)] },
      { type: 1, body: [lget(0), lget(1), lget(2), [0x1b]] },
      { type: 1, body: [lget(0), lget(1), lget(2), [0x1c, 1, I32]] },
      { type: 2, body: [block(I32, i32c(5), br(0), [0x6a, 0x6a, 0x00], i32c(1)), lget(0), [0x6a], [0x0f]] },
      { type: 2, body: [i32c(1), iff(I32, [lget(0), [0x0f]], [lget(0), i32c(2), [0x6c]])] },
      { type: 2, body: [block(I32, block(I32, lget(0), lget(0), brif(1), [0x1a], i32c(3)), i32c(4), [0x6a])] },
    ],
    exports: [['regs', 0, 0], ['sel', 0, 1], ['selt', 0, 2], ['dead', 0, 3], ['early', 0, 4], ['brif', 0, 5]],
  }, [
    ['regs', [I32, I64, F32, F64], [F64, F32, I64, I32], [[1, 2n, 3.5, 4.25]]],
    ['sel', [I32, I32, I32], [I32], [[10, 20, 1], [10, 20, 0], [10, 20, -1]]],
    ['selt', [I32, I32, I32], [I32], [[10, 20, 1], [10, 20, 0]]],
    ['dead', [I32], [I32], [[1]]],
    ['early', [I32], [I32], [[7]]],
    ['brif', [I32], [I32], [[0], [1], [9]]],
  ]);
}

function memoryTests() {
  const d = (off, bytes) => [0, ...i32c(off), END, ...vec(bytes.map((b) => [b]))];
  const types = [[[I32], [I32]], [[I32], [I64]], [[I32], [F32]], [[I32], [F64]], [[I32, I32], []], [[I32, I64], []], [[I32, F32], []], [[I32, F64], []], [[], [I32]], [[I32], [I32]], [[I32, I32, I32], []]];
  const loads = [[0x28, 0, 2, I32], [0x2c, 0, 0, I32], [0x2d, 0, 0, I32], [0x2e, 0, 1, I32], [0x2f, 0, 1, I32], [0x29, 1, 3, I64], [0x30, 1, 0, I64], [0x31, 1, 0, I64], [0x32, 1, 1, I64], [0x33, 1, 1, I64],
    [0x34, 1, 2, I64], [0x35, 1, 2, I64], [0x2a, 2, 2, F32], [0x2b, 3, 3, F64]];
  const funcs = [], exports = [];
  loads.forEach(([op, ty, al, rt], i) => { funcs.push({ type: ty, body: [lget(0), mem(op, al, 3)] }); exports.push([`ld${i}`, 0, i]); });
  const stores = [[0x36, 4, 2, I32], [0x3a, 4, 0, I32], [0x3b, 4, 1, I32], [0x37, 5, 3, I64], [0x3c, 5, 0, I64], [0x3d, 5, 1, I64], [0x3e, 5, 2, I64], [0x38, 6, 2, F32], [0x39, 7, 3, F64]];
  stores.forEach(([op, ty, al, vt], i) => { funcs.push({ type: ty, body: [lget(0), lget(1), mem(op, al, 5)] }); exports.push([`st${i}`, 0, loads.length + i]); });
  const base = funcs.length;
  funcs.push({ type: 8, body: [[0x3f, 0]] }); exports.push(['size', 0, base]);
  funcs.push({ type: 9, body: [lget(0), [0x40, 0]] }); exports.push(['grow', 0, base + 1]);
  funcs.push({ type: 10, body: [lget(0), lget(1), lget(2), fc(11, 0)] }); exports.push(['fill', 0, base + 2]);
  funcs.push({ type: 10, body: [lget(0), lget(1), lget(2), fc(10, 0, 0)] }); exports.push(['copy', 0, base + 3]);
  funcs.push({ type: 10, body: [lget(0), lget(1), lget(2), fc(8, 0, 0)] }); exports.push(['init', 0, base + 4]);
  funcs.push({ type: 8, body: [fc(9, 0), i32c(0)] }); exports.push(['drop', 0, base + 5]);
  funcs.push({ type: 0, body: [lget(0), mem(0x28, 0, 0xffffffff)] }); exports.push(['bigoff', 0, base + 6]);
  funcs.push({ type: 0, body: [lget(0), mem(0x2d, 0, 0)] }); exports.push(['byte', 0, base + 7]);
  const inst = emitModule(module({ types, funcs, mems: [[1, 3]], exports: exports, datacount: 2, datas: [d(16, [1, 2, 3, 4, 5, 6, 7, 8, 0xfe, 0xff, 0x80, 0x7f]), [1, ...vec([[0xaa], [0xbb], [0xcc], [0xdd]])]] }));
  const addrs = [0, 16, 20, 24, 21, 65532, 65533, 65535, 65536, -1, 70000, 65528];
  loads.forEach(([op, ty, al, rt], i) => { for (const a of addrs) emitCall(inst, `ld${i}`, [I32], [rt], [a]); });
  stores.forEach(([op, ty, al, vt], i) => {
    const vals = vt === I32 ? [0x11223344, -1, 0x80, 0x8000] : vt === I64 ? [0x1122334455667788n, -1n, 0x80n] : vt === F32 ? [1.5, -0, NaN] : [Math.PI, -0];
    for (const a of [0, 100, 65528, 65531, 65532, 65535, 65536, -1]) for (const v of vals) { emitCall(inst, `st${i}`, [I32, vt], [], [a, v]); emitCall(inst, 'byte', [I32], [I32], [a]); emitCall(inst, 'byte', [I32], [I32], [a + 3]); }
  });
  for (const a of [0, 100, 70000]) emitCall(inst, 'bigoff', [I32], [I32], [a]);
  emitCall(inst, 'size', [], [I32], []);
  emitCall(inst, 'fill', [I32, I32, I32], [], [200, 0x1ab, 5]); for (let i = 198; i < 207; i++) emitCall(inst, 'byte', [I32], [I32], [i]);
  emitCall(inst, 'fill', [I32, I32, I32], [], [65530, 1, 7]);
  emitCall(inst, 'fill', [I32, I32, I32], [], [65536, 1, 0]);
  emitCall(inst, 'copy', [I32, I32, I32], [], [18, 16, 6]); for (let i = 15; i < 26; i++) emitCall(inst, 'byte', [I32], [I32], [i]);
  emitCall(inst, 'copy', [I32, I32, I32], [], [16, 18, 6]); for (let i = 15; i < 26; i++) emitCall(inst, 'byte', [I32], [I32], [i]);
  emitCall(inst, 'copy', [I32, I32, I32], [], [65530, 0, 7]);
  emitCall(inst, 'init', [I32, I32, I32], [], [300, 1, 2]); for (let i = 299; i < 304; i++) emitCall(inst, 'byte', [I32], [I32], [i]);
  emitCall(inst, 'init', [I32, I32, I32], [], [300, 3, 2]);
  emitCall(inst, 'drop', [], [I32], []);
  emitCall(inst, 'init', [I32, I32, I32], [], [300, 0, 1]);
  emitCall(inst, 'init', [I32, I32, I32], [], [300, 0, 0]);
  emitCall(inst, 'grow', [I32], [I32], [1]); emitCall(inst, 'size', [], [I32], []);
  emitCall(inst, 'st0', [I32, I32], [], [131068, 77]); emitCall(inst, 'ld0', [I32], [I32], [131068]);
  emitCall(inst, 'grow', [I32], [I32], [5]); emitCall(inst, 'grow', [I32], [I32], [1]); emitCall(inst, 'grow', [I32], [I32], [1]); emitCall(inst, 'grow', [I32], [I32], [0]);
}

function tableTests() {
  // call_indirect, ref.func, table ops, elem segments
  const types = [[[I32], [I32]], [[I32, I32], [I32]], [[I32], [EXTERNREF]], [[], [I32]], [[I32, I32], [I32]]];
  const funcs = [
    { type: 0, body: [lget(0), i32c(1), [0x6a]] },
    { type: 0, body: [lget(0), i32c(2), [0x6c]] },
    { type: 1, body: [lget(0), lget(1), [0x6b]] },
    { type: 1, body: [lget(0), lget(1), [0x11, ...u32(0), 0]] },   // 3: call_indirect type 0 table 0: (x, idx) -> table[idx](x)
    { type: 3, body: [i32c(0), [0x25, 0], [0xd1]] },    // 4: table.get 0 is null?
    { type: 0, body: [[0xd2, ...u32(1)], lget(0), fc(15, 0)] },   // 5: table.grow(ref.func 1, n)
    { type: 3, body: [fc(16, 0)] },   // 6: table.size
    { type: 0, body: [lget(0), [0x25, 0], [0xd1]] },   // 7: is_null(table.get(i))
    { type: 4, body: [lget(0), [0xd2, ...u32(0)], [0x26, 0], lget(1)] },   // 8: table.set(i, ref.func 0); return b
    { type: 4, body: [lget(0), [0xd2, ...u32(2)], lget(1), fc(17, 0), i32c(0)] },   // 9: table.fill(i, ref.func 2, n)
    { type: 4, body: [lget(0), lget(1), i32c(2), fc(14, 0, 0), i32c(0)] },   // 10: table.copy(dst, src, 2)
    { type: 4, body: [lget(0), lget(1), i32c(2), fc(12, 0, 0), i32c(0)] },   // 11: table.init elem 0
    { type: 3, body: [fc(13, 0), i32c(0)] },   // 12: elem.drop 0
  ];
  const exports = [['inc', 0, 0], ['dbl', 0, 1], ['sub', 0, 2], ['calli', 0, 3], ['null0', 0, 4], ['tgrow', 0, 5], ['tsize', 0, 6], ['isnull', 0, 7], ['tset', 0, 8], ['tfill', 0, 9], ['tcopy', 0, 10], ['tinit', 0, 11], ['edrop', 0, 12]];
  const inst = emitModule(module({ types, funcs, tables: [[FUNCREF, 6, 20]], exports, elems: [[0, ...i32c(0), END, ...vec([u32(0), u32(1), u32(2)])], [1, 0, ...vec([u32(1), u32(0)])], [3, 0, ...vec([u32(0)])]] }));
  for (const i of [0, 1, 2, 3, 5, 6, 100, -1]) emitCall(inst, 'calli', [I32, I32], [I32], [10, i]);
  emitCall(inst, 'null0', [], [I32], []);
  for (const i of [0, 2, 3, 5, 6, 7]) emitCall(inst, 'isnull', [I32], [I32], [i]);
  emitCall(inst, 'tsize', [], [I32], []);
  emitCall(inst, 'tgrow', [I32], [I32], [3]); emitCall(inst, 'tsize', [], [I32], []); emitCall(inst, 'tgrow', [I32], [I32], [100]); emitCall(inst, 'tgrow', [I32], [I32], [0]);
  emitCall(inst, 'tset', [I32, I32], [I32], [4, 55]); emitCall(inst, 'calli', [I32, I32], [I32], [10, 4]); emitCall(inst, 'tset', [I32, I32], [I32], [400, 55]);
  emitCall(inst, 'tfill', [I32, I32], [I32], [6, 3]); emitCall(inst, 'calli', [I32, I32], [I32], [10, 7]); emitCall(inst, 'tfill', [I32, I32], [I32], [28, 3]);
  emitCall(inst, 'tcopy', [I32, I32], [I32], [1, 0]); emitCall(inst, 'calli', [I32, I32], [I32], [10, 2]); emitCall(inst, 'tcopy', [I32, I32], [I32], [30, 0]);
  emitCall(inst, 'tinit', [I32, I32], [I32], [4, 0]); emitCall(inst, 'tinit', [I32, I32], [I32], [4, 1]); emitCall(inst, 'tinit', [I32, I32], [I32], [4, 5]);
  emitCall(inst, 'edrop', [], [I32], []); emitCall(inst, 'tinit', [I32, I32], [I32], [4, 0]);
  // call_indirect through a table whose entry has another type
  const inst2 = emitModule(module({ types: [[[I32], [I32]], [[I32, I32], [I32]]], funcs: [{ type: 0, body: [lget(0)] }, { type: 0, body: [lget(0), lget(0), i32c(0), [0x11, ...u32(1), 0]] }], tables: [[FUNCREF, 2]], exports: [['mismatch', 0, 1]], elems: [[0, ...i32c(0), END, ...vec([u32(0)])]] }));
  emitCall(inst2, 'mismatch', [I32], [I32], [1]);
}

function globalTests() {
  const inst = emitModule(module({
    types: [[[I32], [I32]], [[], [I64]], [[F64], [F64]], [[], []]],
    funcs: [
      { type: 0, body: [gget(0), lget(0), [0x6a], gset(0), gget(0)] },
      { type: 1, body: [gget(1), gget(2), [0x7c]] },
      { type: 2, body: [lget(0), gget(3), [0xa0], ltee(0), gset(3), gget(3)] },
      { type: 3, body: [i32c(5), gset(0)] },
    ],
    globals: [[I32, 1, i32c(10)], [I64, 0, i64c(-3)], [I64, 0, [...i64c(100), ...i64c(4), 0x7c, ...i64c(3), 0x7e]], [F64, 1, f64c(1.5)]],
    exports: [['add', 0, 0], ['g12', 0, 1], ['fadd', 0, 2], ['setg', 0, 3]],
    start: 3,
  }));
  for (const n of [1, 5, -20]) emitCall(inst, 'add', [I32], [I32], [n]);
  emitCall(inst, 'g12', [], [I64], []);
  for (const x of [0.5, 1e300]) emitCall(inst, 'fadd', [F64], [F64], [x]);
}

function tailTests() {
  const inst = emitModule(module({
    types: [[[I32, I32], [I32]], [[I32], [I32]]],
    funcs: [
      { type: 0, body: [lget(0), [0x45], iff(I32, [lget(1)], [lget(0), i32c(1), [0x6b], lget(1), i32c(2), [0x6a], [0x12, ...u32(0)]])] },
      { type: 1, body: [lget(0), i32c(0), [0x12, ...u32(0)]] },
      { type: 1, body: [lget(0), i32c(0), i32c(0), [0x13, ...u32(0), 0]] },
    ],
    tables: [[FUNCREF, 1]], elems: [[0, ...i32c(0), END, ...vec([u32(0)])]],
    exports: [['count', 0, 0], ['tail', 0, 1], ['ind', 0, 2]],
  }));
  emitCall(inst, 'count', [I32, I32], [I32], [10, 0]);
  emitCall(inst, 'count', [I32, I32], [I32], [1000000, 0]);
  emitCall(inst, 'tail', [I32], [I32], [300000]);
  emitCall(inst, 'ind', [I32], [I32], [200000]);
}

// --- 3. SIMD ------------------------------------------------------------------------------------------------------------------------
function simdTests() {
  // f(a_lo, a_hi, b_lo, b_hi) -> (lo, hi): builds the vectors from the i64 halves
  const make = (lo, hi) => [lget(lo), simd(18), lget(hi), simd(30, 1)];   // i64x2.splat(lo); replace_lane 1 hi
  const lanesLo = [0n, 1n, -1n, 0x7fn, 0x80n, 0xffn, 0x7fff7fff7fff7fffn, 0x8000800080008000n, 0x0123456789abcdefn, -0x8000000000000000n, 0x00ff00ff00ff00ffn, 0x0102030405060708n, 0x7fffffff80000000n, 0xfedcba9876543210n];
  const lanesHi = [0n, -1n, 0x1n, 0x5555555555555555n, 0x8000000080000000n, 0x4000400040004000n, 0x00010002ffff7ffen, 0x7f80ff7f0180ff00n];
  const run = (name, unary, extraBytes, ptypes, rtype) => {
    const body = unary ? [...make(0, 1), ...extraBytes] : [...make(0, 1), ...make(2, 3), ...extraBytes];
    const spec = { types: [[unary ? [I64, I64] : [I64, I64, I64, I64], [I64, I64]]], funcs: [{ type: 0, locals: [[1, V128]], body: [body, lset(unary ? 2 : 4), lget(unary ? 2 : 4), simd(29, 0), lget(unary ? 2 : 4), simd(29, 1)].flat(Infinity) }], exports: [['f', 0, 0]] };
    const inst = emitModule(module(spec));
    const pt = unary ? [I64, I64] : [I64, I64, I64, I64];
    for (let i = 0; i < lanesLo.length; i++) for (let j = 0; j < 3; j++) {
      const a = [lanesLo[i], lanesHi[(i + j) % lanesHi.length]];
      if (unary) emitCall(inst, 'f', pt, [I64, I64], a);
      else emitCall(inst, 'f', pt, [I64, I64], [...a, lanesLo[(i * 3 + j) % lanesLo.length], lanesHi[(i + 2 * j) % lanesHi.length]]);
    }
  };
  const noFloat = new Set([
    // float lanes: tested with their own inputs below
    ...[65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 94, 95, 103, 104, 105, 106, 116, 117, 122, 148, 224, 225, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255]]);
  const binary = [35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 78, 79, 80, 81, 14, 101, 102, 110, 111, 112, 113, 114, 115, 118, 119, 120, 121, 123, 130, 133, 134, 142, 143, 144, 145, 146, 147, 149, 150, 151, 152, 153, 155,
    156, 157, 158, 159, 174, 177, 181, 182, 183, 184, 185, 186, 188, 189, 190, 191, 206, 209, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223];
  const unary = [77, 96, 97, 98, 124, 125, 126, 127, 128, 129, 135, 136, 137, 138, 160, 161, 167, 168, 169, 170, 192, 193, 199, 200, 201, 202];
  const toI32 = [83, 99, 100, 131, 132, 163, 164, 195, 196];
  const shifts = [107, 108, 109, 139, 140, 141, 171, 172, 173, 203, 204, 205];
  for (const s of binary) run(`op${s}`, false, [simd(s)], null);
  for (const s of unary) run(`op${s}`, true, [simd(s)], null);
  for (const s of toI32) { const spec = { types: [[[I64, I64], [I32]]], funcs: [{ type: 0, body: [make(0, 1), simd(s)].flat(Infinity) }], exports: [['f', 0, 0]] }; const inst = emitModule(module(spec)); for (let i = 0; i < lanesLo.length; i++) for (const h of lanesHi) emitCall(inst, 'f', [I64, I64], [I32], [lanesLo[i], h]); }
  for (const s of shifts) { const spec = { types: [[[I64, I64, I32], [I64, I64]]], funcs: [{ type: 0, locals: [[1, V128]], body: [make(0, 1), lget(2), simd(s), lset(3), lget(3), simd(29, 0), lget(3), simd(29, 1)].flat(Infinity) }], exports: [['f', 0, 0]] };
    const inst = emitModule(module(spec)); for (let i = 0; i < lanesLo.length; i++) for (const c of [0, 1, 3, 7, 8, 15, 16, 31, 33, 63, 64, 65, -1]) emitCall(inst, 'f', [I64, I64, I32], [I64, I64], [lanesLo[i], lanesHi[i % lanesHi.length], c]); }
  // bitselect, any_true, shuffle, swizzle, splats, extract/replace, const
  run('bitselect', false, [vconst(0x0f0f0f0f33333333n, 0xaaaaaaaa55555555n), simd(82)], null);
  {
    const spec = { types: [[[I32, I64, F32, F64], [I64, I64, I64, I64, I64, I64, I64, I64]]], funcs: [{ type: 0, body: [
      lget(0), simd(15), simd(29, 0), lget(0), simd(15), simd(29, 1),
      lget(0), simd(16), simd(29, 0), lget(0), simd(17), simd(29, 1),
      lget(1), simd(18), simd(29, 1), lget(2), simd(19), simd(27, 3), [0xad], lget(3), simd(20), simd(33, 1), [0xbd], lget(0), simd(17), simd(27, 2), [0xad]].flat(Infinity) }], exports: [['f', 0, 0]] };
    const inst = emitModule(module(spec));
    emitCall(inst, 'f', [I32, I64, F32, F64], [I64, I64, I64, I64, I64, I64, I64, I64], [0x12345678, 0x1122334455667788n, 1.5, 2.25]);
    emitCall(inst, 'f', [I32, I64, F32, F64], [I64, I64, I64, I64, I64, I64, I64, I64], [-1, -2n, -0, 1e300]);
  }
  for (const [name, bytes] of [['shuf', [simd(13, [0, 17, 2, 19, 4, 21, 6, 23, 8, 25, 10, 27, 12, 29, 14, 31])]], ['shuf2', [simd(13, [31, 30, 29, 28, 27, 26, 25, 24, 7, 6, 5, 4, 3, 2, 1, 0])]]]) run(name, false, bytes, null);
  run('swizzle', false, [simd(14)], null);
  {   // extract/replace with sign: i8x16 lanes
    const spec = { types: [[[I64, I64, I32], [I32, I32, I32, I32, I32, I32, I32, I32, I32, I32]]], funcs: [{ type: 0, locals: [[1, V128]], body: [make(0, 1), lset(3),
      lget(3), simd(21, 3), lget(3), simd(22, 3), lget(3), simd(21, 15), lget(3), simd(24, 7), lget(3), simd(25, 7), lget(3), simd(27, 2), lget(3), lget(2), simd(23, 5), simd(22, 5),
      lget(3), lget(2), simd(26, 1), simd(24, 1), lget(3), lget(2), simd(28, 3), simd(27, 3), lget(3), simd(29, 1), [0xa7]].flat(Infinity) }], exports: [['f', 0, 0]] };
    const inst = emitModule(module(spec));
    for (const lo of lanesLo) emitCall(inst, 'f', [I64, I64, I32], [I32, I32, I32, I32, I32, I32, I32, I32, I32, I32], [lo, 0x8070605040302010n, 0x1ff]);
  }
  // memory ops
  {
    const types = [[[I32], [I64, I64]], [[I32, I64, I64], []], [[I32, I64, I64, I32], [I64, I64]]];
    const mk = (name, body) => ({ name, body });
    const loadOps = [[0, 4], [1, 3], [2, 3], [3, 3], [4, 3], [5, 3], [6, 3], [7, 0], [8, 1], [9, 2], [10, 3], [92, 2], [93, 3]];
    const funcs = [], exports = [];
    loadOps.forEach(([s, al], i) => { funcs.push({ type: 0, locals: [[1, V128]], body: [lget(0), simd(s, u32(al), u32(1)), lset(1), lget(1), simd(29, 0), lget(1), simd(29, 1)].flat(Infinity) }); exports.push([`ld${s}`, 0, i]); });
    funcs.push({ type: 1, body: [lget(0), make(1, 2), simd(11, u32(0), u32(3))].flat(Infinity) }); exports.push(['store', 0, funcs.length - 1]);
    const lane = (s, al, l) => ({ type: 2, locals: [[1, V128]], body: [lget(0), make(1, 2), simd(s, u32(al), u32(0), [l]), lset(4), lget(4), simd(29, 0), lget(4), simd(29, 1)].flat(Infinity) });
    [[84, 0, 3], [85, 1, 5], [86, 2, 2], [87, 3, 1]].forEach(([s, al, l]) => { funcs.push(lane(s, al, l)); exports.push([`ll${s}`, 0, funcs.length - 1]); });
    const sl = (s, al, l) => ({ type: 1, body: [lget(0), make(1, 2), simd(s, u32(al), u32(0), [l])].flat(Infinity) });
    [[88, 0, 3], [89, 1, 5], [90, 2, 2], [91, 3, 1]].forEach(([s, al, l]) => { funcs.push(sl(s, al, l)); exports.push([`sl${s}`, 0, funcs.length - 1]); });
    funcs.push({ type: 0, body: [lget(0), mem(0x29, 0, 0), lget(0), mem(0x29, 0, 8)] }); exports.push(['rd', 0, funcs.length - 1]);
    types.push([[I32], [I64, I64]]);
    funcs[funcs.length - 1].type = 0;
    const inst = emitModule(module({ types, funcs, mems: [[1, 1]], exports, datas: [[0, ...i32c(0), END, ...vec([...Array(40).keys()].map((i) => [i * 7 + 1]))]] }));
    // datacount 0 with 1 data segment is invalid: drop it below
    for (const [s] of loadOps) for (const a of [0, 3, 8, 24, 65520, 65529, 65536]) emitCall(inst, `ld${s}`, [I32], [I64, I64], [a]);
    for (const a of [0, 100, 65520, 65521]) { emitCall(inst, 'store', [I32, I64, I64], [], [a, 0x0807060504030201n, 0x100f0e0d0c0b0a09n]); emitCall(inst, 'rd', [I32], [I64, I64], [a]); }
    for (const s of [84, 85, 86, 87]) for (const a of [0, 100, 65535]) emitCall(inst, `ll${s}`, [I32, I64, I64, I32], [I64, I64], [a, 0x1111111111111111n, 0x2222222222222222n, 0]);
    for (const s of [88, 89, 90, 91]) for (const a of [0, 100, 65535]) { emitCall(inst, `sl${s}`, [I32, I64, I64], [], [a, 0x0807060504030201n, 0x100f0e0d0c0b0a09n]); emitCall(inst, 'rd', [I32], [I64, I64], [a]); }
  }
}

function simdFloatTests() {
  const F = (v) => v;
  const f32lanes = [[1.5, -2.5, 0.5, 100.7], [NaN, 0, -0, Infinity], [-Infinity, 3.5, 2.5, -1.5], [16777216, 8388607.5, 2147483648, -2147483904], [4294967296, 1e10, -1e-3, 5e-324], [Math.fround(0.1), -0.9999999, 4294967040, 3.4028235e38]];
  const f64lanes = [[1.5, -2.5], [NaN, 0], [-0, Infinity], [-Infinity, 3.5], [2.5, -1.5], [4503599627370496, 4503599627370495.5], [2147483648, -2147483649], [4294967296, 1e300], [0.1, -100.7], [9223372036854775808, 4294967295.9]];
  const mk32 = (a, b, c, d, base) => [f32c(a), simd(19), f32c(b), simd(32, 1), f32c(c), simd(32, 2), f32c(d), simd(32, 3)].flat(Infinity);
  const getAll32 = (l) => [lget(l), simd(31, 0), lget(l), simd(31, 1), lget(l), simd(31, 2), lget(l), simd(31, 3)].flat(Infinity);
  const getAll64 = (l) => [lget(l), simd(33, 0), lget(l), simd(33, 1)].flat(Infinity);
  const getI32 = (l) => [lget(l), simd(27, 0), lget(l), simd(27, 1), lget(l), simd(27, 2), lget(l), simd(27, 3)].flat(Infinity);
  const bin32 = [65, 66, 67, 68, 69, 70, 228, 229, 230, 231, 232, 233, 234, 235];
  const un32 = [103, 104, 105, 106, 224, 225, 227];
  for (const s of [...bin32, ...un32]) {
    const unary = un32.includes(s);
    const cmp = s >= 65 && s <= 70;
    const rt = cmp ? I32 : F32;
    const spec = { types: [[[F32, F32, F32, F32, F32, F32, F32, F32], [rt, rt, rt, rt]]], funcs: [{ type: 0, locals: [[3, V128]], body: [
      [lget(0), simd(19), lget(1), simd(32, 1), lget(2), simd(32, 2), lget(3), simd(32, 3), lset(8)].flat(Infinity),
      [lget(4), simd(19), lget(5), simd(32, 1), lget(6), simd(32, 2), lget(7), simd(32, 3), lset(9)].flat(Infinity),
      lget(8), unary ? [] : lget(9), simd(s), lset(10), cmp ? getI32(10) : getAll32(10)].flat(Infinity) }], exports: [['f', 0, 0]] };
    const inst = emitModule(module(spec));
    for (let i = 0; i < f32lanes.length; i++) for (let j = 0; j < f32lanes.length; j++) emitCall(inst, 'f', Array(8).fill(F32), Array(4).fill(rt), [...f32lanes[i], ...f32lanes[j]]);
  }
  const bin64 = [71, 72, 73, 74, 75, 76, 240, 241, 242, 243, 244, 245, 246, 247];
  const un64 = [116, 117, 122, 148, 236, 237, 239];
  for (const s of [...bin64, ...un64]) {
    const unary = un64.includes(s);
    const cmp = s >= 71 && s <= 76;
    const spec = { types: [[[F64, F64, F64, F64], cmp ? [I64, I64] : [F64, F64]]], funcs: [{ type: 0, locals: [[3, V128]], body: [
      [lget(0), simd(20), lget(1), simd(34, 1), lset(4)].flat(Infinity), [lget(2), simd(20), lget(3), simd(34, 1), lset(5)].flat(Infinity),
      lget(4), unary ? [] : lget(5), simd(s), lset(6), cmp ? [lget(6), simd(29, 0), lget(6), simd(29, 1)].flat(Infinity) : getAll64(6)].flat(Infinity) }], exports: [['f', 0, 0]] };
    const inst = emitModule(module(spec));
    for (let i = 0; i < f64lanes.length; i++) for (let j = 0; j < f64lanes.length; j++) emitCall(inst, 'f', Array(4).fill(F64), cmp ? [I64, I64] : [F64, F64], [...f64lanes[i], ...f64lanes[j]]);
  }
  // conversions
  const conv = [[248, 4, F32, I32], [249, 4, F32, I32], [250, 4, I32, F32], [251, 4, I32, F32], [252, 2, F64, I32], [253, 2, F64, I32], [254, 2, I32, F64], [255, 2, I32, F64], [94, 2, F64, F32], [95, 2, F32, F64]];
  for (const [s, n, from, to] of conv) {
    const nin = (s === 254 || s === 255 || s === 95) ? 4 : n;
    const inLanes = from === F32 ? 4 : from === F64 ? 2 : 4;
    const setLane = from === F32 ? 32 : from === F64 ? 34 : 28, splat = from === F32 ? 19 : from === F64 ? 20 : 17;
    const build = (off, count) => { const b = [lget(off), simd(splat)]; for (let k = 1; k < count; k++) b.push(lget(off + k), simd(setLane, k)); return b.flat(Infinity); };
    const outLanes = to === F32 ? 4 : to === F64 ? 2 : 4;
    const getLane = to === F32 ? 31 : to === F64 ? 33 : 27;
    const spec = { types: [[Array(inLanes).fill(from), Array(outLanes).fill(to)]], funcs: [{ type: 0, locals: [[2, V128]], body: [build(0, inLanes), simd(s), lset(inLanes), Array.from({ length: outLanes }, (_, k) => [lget(inLanes), simd(getLane, k)]).flat(Infinity)].flat(Infinity) }], exports: [['f', 0, 0]] };
    const inst = emitModule(module(spec));
    const src = from === F32 ? f32lanes : from === F64 ? f64lanes : [[0, 1, -1, 5], [0x7fffffff, -0x80000000, 255, 65536], [16777217, -16777217, 123456789, -5]];
    for (const l of src) emitCall(inst, 'f', Array(inLanes).fill(from), Array(outLanes).fill(to), l.slice(0, inLanes));
  }
}


// --- 3b. relaxed SIMD: only where every engine must give the same answer (lane selects with whole-lane masks, in-range truncations, values that multiply exactly ...)
function relaxedTests() {
  const make = (lo, hi) => [lget(lo), simd(18), lget(hi), simd(30, 1)];
  const intsLo = [0x0123456789abcdefn, 0x7f7f7f7f7f7f7f7fn, 0x0001000200030004n, -0x1000200030004n, 0x0102030405060708n, 0x7fn];
  const intsHi = [0x1111222233334444n, 0x7e7d7c7b7a797877n, 0x00ff00ff00ff00ffn, 0x0000000100000002n];
  // (masks of whole lanes: a mask that is neither all ones nor all zeros in a lane is up to the engine)
  const masksFor = { 0x109: [[-1n, 0n], [0xff00ff00ff00ff00n, 0xff00ff00ff00ff00n], [0xffff0000ffff0000n, 0x0000ffff0000ffffn], [-1n, -1n], [0n, 0n]],
    0x10a: [[-1n, 0n], [0xffff0000ffff0000n, 0x0000ffff0000ffffn], [-1n, -1n], [0n, 0n]], 0x10b: [[-1n, 0n], [0xffffffff00000000n, 0x00000000ffffffffn], [-1n, -1n], [0n, 0n]],
    0x10c: [[-1n, 0n], [0n, -1n], [-1n, -1n], [0n, 0n]] };
  const run3 = (name, bytes, third, bFixed) => {   // f(a_lo, a_hi, b_lo, b_hi, c_lo, c_hi) -> (lo, hi)
    const spec = { types: [[[I64, I64, I64, I64, I64, I64], [I64, I64]]], funcs: [{ type: 0, locals: [[1, V128]], body: [make(0, 1), make(2, 3), make(4, 5), bytes, lset(6), lget(6), simd(29, 0), lget(6), simd(29, 1)].flat(Infinity) }], exports: [['f', 0, 0]] };
    const inst = emitModule(module(spec));
    for (let i = 0; i < intsLo.length; i++) for (let j = 0; j < intsHi.length; j++) for (const c of third) emitCall(inst, 'f', Array(6).fill(I64), [I64, I64], [intsLo[i], intsHi[j], bFixed ? bFixed[0] : intsLo[(i + 2) % intsLo.length], bFixed ? bFixed[1] : intsHi[(j + 1) % intsHi.length], c[0], c[1]]);
  };
  for (const s of [0x109, 0x10a, 0x10b, 0x10c]) run3(`select${s}`, [simd(s)], masksFor[s]);
  const run2 = (name, bytes, bs) => {
    const spec = { types: [[[I64, I64, I64, I64], [I64, I64]]], funcs: [{ type: 0, locals: [[1, V128]], body: [make(0, 1), make(2, 3), bytes, lset(4), lget(4), simd(29, 0), lget(4), simd(29, 1)].flat(Infinity) }], exports: [['f', 0, 0]] };
    const inst = emitModule(module(spec));
    for (let i = 0; i < intsLo.length; i++) for (let j = 0; j < intsHi.length; j++) for (const b of bs) emitCall(inst, 'f', Array(4).fill(I64), [I64, I64], [intsLo[i], intsHi[j], b[0], b[1]]);
  };
  run2('swizzle', [simd(0x100)], [[0x0f0e0d0c0b0a0908n, 0x0706050403020100n], [0x0001020304050607n, 0x08090a0b0c0d0e0fn]]);
  run2('q15', [simd(0x111)], [[0x0001000200030004n, 0x0005000600070008n], [0x1000200030004000n, 0x0100020003000400n]]);
  run2('dot', [simd(0x112)], [[0x0102030405060708n, 0x090a0b0c0d0e0f10n], [0x7f7e7d7c7b7a7978n, 0x0000000100020003n]]);
  run3('dotadd', [simd(0x113)], [[1n, 2n], [0x100000001n, -1n]], [0x0102030405060708n, 0x7f7e7d7c7b7a7978n]);
  // floats
  // (values whose products are exact: an engine may or may not fuse the multiply and the add)
  const f32a = [[1.5, -2.5, 0.5, 100.5], [3.5, -1.5, 2.5, -100.5], [2, 4, 8, 16], [0.25, 0.5, 0.75, 1.25]];
  const f64a = [[1.5, -2.5], [3.5, -1.5], [2, 4], [0.25, 0.5]];
  const mk32 = (o) => [lget(o), simd(19), lget(o + 1), simd(32, 1), lget(o + 2), simd(32, 2), lget(o + 3), simd(32, 3)];
  const mk64 = (o) => [lget(o), simd(20), lget(o + 1), simd(34, 1)];
  for (const s of [0x10d, 0x10e, 0x105, 0x106]) {
    const ternary = s === 0x105 || s === 0x106;
    const n = ternary ? 12 : 8;
    const spec = { types: [[Array(n).fill(F32), Array(4).fill(F32)]], funcs: [{ type: 0, locals: [[1, V128]], body: [mk32(0), mk32(4), ternary ? mk32(8) : [], simd(s), lset(n), [0, 1, 2, 3].map((k) => [lget(n), simd(31, k)])].flat(Infinity) }], exports: [['f', 0, 0]] };
    const inst = emitModule(module(spec));
    for (const a of f32a) for (const b of f32a) for (const c of ternary ? f32a : [[]]) emitCall(inst, 'f', Array(n).fill(F32), Array(4).fill(F32), [...a, ...b, ...c]);
  }
  for (const s of [0x10f, 0x110, 0x107, 0x108]) {
    const ternary = s === 0x107 || s === 0x108;
    const n = ternary ? 6 : 4;
    const spec = { types: [[Array(n).fill(F64), Array(2).fill(F64)]], funcs: [{ type: 0, locals: [[1, V128]], body: [mk64(0), mk64(2), ternary ? mk64(4) : [], simd(s), lset(n), [0, 1].map((k) => [lget(n), simd(33, k)])].flat(Infinity) }], exports: [['f', 0, 0]] };
    const inst = emitModule(module(spec));
    for (const a of f64a) for (const b of f64a) for (const c of ternary ? f64a : [[]]) emitCall(inst, 'f', Array(n).fill(F64), Array(2).fill(F64), [...a, ...b, ...c]);
  }
  // truncations of in-range values
  for (const [s, from, lanes] of [[0x101, F32, 4], [0x102, F32, 4], [0x103, F64, 2], [0x104, F64, 2]]) {
    const unsigned = s === 0x102 || s === 0x104;
    const spec = { types: [[Array(lanes).fill(from), Array(4).fill(I32)]], funcs: [{ type: 0, locals: [[1, V128]], body: [lanes === 4 ? mk32(0) : mk64(0), simd(s), lset(lanes), [0, 1, 2, 3].map((k) => [lget(lanes), simd(27, k)])].flat(Infinity) }], exports: [['f', 0, 0]] };
    const inst = emitModule(module(spec));
    const inputs = lanes === 4 ? [[1.5, 2.9, 100.2, 7], [0.5, 3, 4.99, 1e6]] : [[1.5, 2.9], [100.2, 1e6]];
    for (const l of inputs) emitCall(inst, 'f', Array(lanes).fill(from), Array(4).fill(I32), unsigned ? l : l.map((x, i) => (i % 2 ? -x : x)));
  }
}

// --- 4. a few modules that must not validate ----------------------------------------------------------------------------------------------
function invalidTests() {
  const bad = (spec) => out.push('invalid ' + hex(module(spec)));
  const good = (spec) => out.push('valid ' + hex(module(spec)));
  const fn = (types, body, locals) => ({ types, funcs: [{ type: 0, body, locals }] });
  bad(fn([[[], [I32]]], [[0x01]]));                                     // missing result
  bad(fn([[[], [I32]]], [i64c(1)]));                                     // wrong type
  bad(fn([[[], []]], [i32c(1)]));                                        // value left
  bad(fn([[[], []]], [[0x6a]]));                                         // pop from empty
  bad(fn([[[I32], [I32]]], [lget(1)]));                                  // unknown local
  bad(fn([[[], []]], [br(1)]));                                          // unknown label
  bad(fn([[[], []]], [call(5)]));                                        // unknown function
  bad(fn([[[], []]], [i32c(0), [0x28, 0, 0]]));                          // no memory
  bad({ types: [[[], []]], funcs: [{ type: 0, body: [i32c(0), mem(0x28, 3, 0), [0x1a]] }], mems: [[1]] });   // alignment too large
  bad(fn([[[], [I32]]], [block(I32, br(0))].flat(Infinity)));                    // br without value
  bad(fn([[[], [I32]]], [i32c(1), iff(I32, [i32c(2)])]));                // if without else with result
  bad({ types: [[[], []]], funcs: [{ type: 0, body: [gget(0)] }], globals: [[I32, 0, i32c(0)]] });   // value left (gget)
  bad({ types: [[[], []]], funcs: [{ type: 0, body: [i32c(1), gset(0)] }], globals: [[I32, 0, i32c(0)]] });   // immutable global
  bad({ types: [[[], []]], funcs: [{ type: 0, body: [[0xd2, 0]] , }] });                                // undeclared ref.func
  bad({ types: [[[], []]], funcs: [{ type: 0, body: [] }], exports: [['a', 0, 0], ['a', 0, 0]] });    // duplicate export
  bad({ types: [[[I32], []]], funcs: [{ type: 0, body: [] }], start: 0 });                              // start with params
  bad({ types: [[[], []]], funcs: [{ type: 0, body: [] }], mems: [[2, 1]] });                           // min > max
  bad({ types: [[[], []]], funcs: [{ type: 0, body: [fc(8, 0, 0)] }], mems: [[1]] });                   // memory.init without datacount
  good(fn([[[], []]], [[0x00], [0x6a], [0x0b].slice(0, 0)].slice(0, 1)));        // unreachable is fine
  good(fn([[[], [I32]]], [[0x00], [0x6a]]));                              // polymorphic stack
  good(fn([[[], [I32]]], [block(I32, i32c(1), br(0), [0x6a])]));
  good({ types: [[[], []]], funcs: [{ type: 0, body: [[0xd2, 0], [0x1a]] }], exports: [['f', 0, 0]] });
}

// --- 5. mutations of the modules above: the reference engine says which still validate ----------------------------------------------------------
// (validate() may skip function bodies, so the module is compiled)
const isValid = (b) => { try { new WebAssembly.Module(b); return true; } catch (e) { return false; } };

function mutations() {
  let seed = 12345;
  const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed; };
  const mods = out.filter((l) => l.startsWith('module ')).map((l) => Buffer.from(l.slice(7), 'hex'));
  const seen = new Set();
  const unique = mods.filter((m) => { const k = m.toString('hex'); if (seen.has(k)) return false; seen.add(k); return true; });
  let count = 0;
  for (let n = 0; n < (Number(process.env.MUTANTS) || 2500) && unique.length; n++) {
    const base = unique[rnd() % unique.length];
    if (base.length > 400) continue;
    const m = Buffer.from(base);
    const kind = rnd() % 4;
    if (kind === 0) m[8 + (rnd() % (m.length - 8))] ^= 1 << (rnd() % 8);
    else if (kind === 1) m[8 + (rnd() % (m.length - 8))] = rnd() & 0xff;
    else if (kind === 2) { const cut = 8 + (rnd() % (m.length - 8)); out.push((isValid(m.subarray(0, cut)) ? 'valid ' : 'invalid ') + m.subarray(0, cut).toString('hex')); count++; continue; }
    else { const i = 8 + (rnd() % (m.length - 8)); m[i] = (m[i] + 1) & 0xff; }
    out.push((isValid(m) ? 'valid ' : 'invalid ') + m.toString('hex'));
    count++;
  }
}

numericTests();
controlTests();
memoryTests();
tableTests();
globalTests();
tailTests();
simdTests();
simdFloatTests();
relaxedTests();
invalidTests();
mutations();
console.log(out.join('\n'));
