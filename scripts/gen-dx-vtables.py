#!/usr/bin/env python3
# TheoryCraftOS - a minimal UEFI x86_64 operating system.
# Copyright (C) 2026 Christopher Schaefer
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published
# by the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
"""Writes src/Windows/<dll>/vtbl.inc: the vtables of the COM objects of dxgi.dll and d3d12.dll.

The C sources define each method as a function named <prefix>_<Method> (fac_EnumAdapters, dev_CreateFence ...). For every interface listed below this script reads the
method list from the mingw headers and writes the vtable, taking each method from the first prefix that defines it and pointing the others at dx_not_implemented (E_NOTIMPL).
A method defined with `#define <prefix>_<Method> <function>` counts too (the IUnknown and ID3D12Object methods all objects share).

    python3 scripts/gen-dx-vtables.py [dll ...]
"""
import re, os, sys

INC = '/usr/x86_64-w64-mingw32/include/'
HEADERS = ['d3d12.h', 'd3d12video.h', 'd3d12sdklayers.h', 'dxgi1_6.h', 'dxgi1_5.h', 'dxgi1_4.h', 'dxgi1_3.h', 'dxgi1_2.h', 'dxgi.h', 'd3dcommon.h', 'd3d11.h', 'd3d11_1.h', 'd3d11_2.h', 'd3d11_3.h', 'd3d11_4.h', 'd3d9.h']
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'Windows')

# (interface, vtable variable, [prefixes in the order they are tried])
SPECS = {
    'dxgi': [
        ('IDXGIFactory6', 'fac_vtbl', ['fac', 'obj']),
        ('IDXGIAdapter4', 'adp_vtbl', ['adp', 'obj']),
        ('IDXGIOutput6', 'out_vtbl', ['out', 'obj']),
        ('IDXGISwapChain3', 'sc_vtbl', ['sc', 'obj']),
    ],
    'd3d12': [
        ('ID3D12Device14', 'dev_vtbl', ['dev', 'dobj', 'obj']),
        ('ID3D12CommandQueue', 'queue_vtbl', ['queue', 'dobj', 'obj']),
        ('ID3D12CommandAllocator', 'alloc_vtbl', ['alloc', 'dobj', 'obj']),
        ('ID3D12GraphicsCommandList10', 'list_vtbl', ['list', 'dobj', 'obj']),
        ('ID3D12Fence1', 'fence_vtbl', ['fence', 'dobj', 'obj']),
        ('ID3D12Resource2', 'res_vtbl', ['res', 'dobj', 'obj']),
        ('ID3D12Heap1', 'heap_vtbl', ['heap', 'dobj', 'obj']),
        ('ID3D12DescriptorHeap', 'dheap_vtbl', ['dheap', 'dobj', 'obj']),
        ('ID3D12RootSignature', 'rs_vtbl', ['rs', 'dobj', 'obj']),
        ('ID3D12PipelineState', 'pso_vtbl', ['pso', 'dobj', 'obj']),
        ('ID3D12QueryHeap', 'qheap_vtbl', ['qheap', 'dobj', 'obj']),
        ('ID3D12CommandSignature', 'sig_vtbl', ['sig', 'dobj', 'obj']),
        ('ID3D12PipelineLibrary1', 'plib_vtbl', ['plib', 'dobj', 'obj']),
        ('ID3D12ShaderCacheSession', 'scache_vtbl', ['scache', 'dobj', 'obj']),
        ('ID3D12LifetimeTracker', 'ltrack_vtbl', ['ltrack', 'dobj', 'obj']),
        ('ID3D12RootSignatureDeserializer', 'rsd_vtbl', ['rsd', 'obj']),
        ('ID3D12VersionedRootSignatureDeserializer', 'vrsd_vtbl', ['vrsd', 'obj']),
        ('ID3D12VideoDevice4', 'vdev_vtbl', ['vdev']),
        ('ID3D12VideoDecoder1', 'vdec_vtbl', ['vdec', 'dobj', 'obj']),
        ('ID3D12VideoDecoderHeap1', 'vdheap_vtbl', ['vdheap', 'dobj', 'obj']),
        ('ID3D12VideoProcessor1', 'vproc_vtbl', ['vproc', 'dobj', 'obj']),
        ('ID3D12VideoDecodeCommandList3', 'vdlist_vtbl', ['vdlist', 'vlist', 'list', 'dobj', 'obj']),
        ('ID3D12VideoProcessCommandList3', 'vplist_vtbl', ['vplist', 'vlist', 'list', 'dobj', 'obj']),
        ('ID3D12VideoMotionEstimator', 'vme_vtbl', ['vme', 'dobj', 'obj']),
        ('ID3D12VideoMotionVectorHeap', 'vmvh_vtbl', ['vmvh', 'dobj', 'obj']),
        ('ID3D12VideoEncoder', 'venc_vtbl', ['venc', 'dobj', 'obj']),
        ('ID3D12VideoEncoderHeap1', 'veheap_vtbl', ['veheap', 'dobj', 'obj']),
        ('ID3D12VideoEncodeCommandList4', 'velist_vtbl', ['velist', 'vlist', 'list', 'dobj', 'obj']),
        ('ID3D10Blob', 'blob_vtbl', ['blob', 'obj']),
    ],
    'd3d11': [
        ('ID3D11Device5', 'dev_vtbl', ['dev', 'obj']),
        ('ID3D11DeviceContext4', 'ctx_vtbl', ['ctx', 'child', 'obj']),
        ('ID3D11Buffer', 'buf_vtbl', ['buf', 'child', 'obj']),
        ('ID3D11Texture1D', 'tex1d_vtbl', ['tex1d', 'tex', 'child', 'obj']),
        ('ID3D11Texture2D1', 'tex_vtbl', ['tex', 'child', 'obj']),
        ('ID3D11Texture3D1', 'tex3_vtbl', ['tex3', 'tex', 'child', 'obj']),
        ('ID3D11RenderTargetView1', 'rtv_vtbl', ['rtv', 'view', 'child', 'obj']),
        ('ID3D11DepthStencilView', 'dsv_vtbl', ['dsv', 'view', 'child', 'obj']),
        ('ID3D11ShaderResourceView1', 'srv_vtbl', ['srv', 'view', 'child', 'obj']),
        ('ID3D11UnorderedAccessView1', 'uav_vtbl', ['uav', 'view', 'child', 'obj']),
        ('ID3D11VertexShader', 'vs_vtbl', ['child', 'obj']),
        ('ID3D11PixelShader', 'ps_vtbl', ['child', 'obj']),
        ('ID3D11GeometryShader', 'gs_vtbl', ['child', 'obj']),
        ('ID3D11HullShader', 'hs_vtbl', ['child', 'obj']),
        ('ID3D11DomainShader', 'ds_vtbl', ['child', 'obj']),
        ('ID3D11ComputeShader', 'cs_vtbl', ['child', 'obj']),
        ('ID3D11InputLayout', 'layout_vtbl', ['child', 'obj']),
        ('ID3D11BlendState1', 'blend_vtbl', ['blend', 'child', 'obj']),
        ('ID3D11DepthStencilState', 'depth_vtbl', ['depth', 'child', 'obj']),
        ('ID3D11RasterizerState2', 'raster_vtbl', ['raster', 'child', 'obj']),
        ('ID3D11SamplerState', 'sampler_vtbl', ['sampler', 'child', 'obj']),
        ('ID3D11Query1', 'query_vtbl', ['query', 'child', 'obj']),
        ('ID3D11Predicate', 'pred_vtbl', ['query', 'child', 'obj']),
        ('ID3D11ClassLinkage', 'link_vtbl', ['link', 'child', 'obj']),
        ('ID3D11ClassInstance', 'inst_vtbl', ['inst', 'child', 'obj']),
        ('ID3D11CommandList', 'cmdlist_vtbl', ['cmdlist', 'child', 'obj']),
        ('ID3DDeviceContextState', 'cstate_vtbl', ['cstate', 'child', 'obj']),
        ('ID3D11Fence', 'fence_vtbl', ['fence', 'child', 'obj']),
        ('ID3D11VideoDevice2', 'vdev_vtbl', ['vdev']),
        ('ID3D11VideoContext3', 'vctx_vtbl', ['vctx']),
        ('ID3D11VideoDecoder', 'vdec_vtbl', ['vdec', 'child', 'obj']),
        ('ID3D11VideoProcessorEnumerator1', 'venum_vtbl', ['venum', 'child', 'obj']),
        ('ID3D11VideoProcessor', 'vproc_vtbl', ['vproc', 'child', 'obj']),
        ('ID3D11VideoDecoderOutputView', 'dov_vtbl', ['dov', 'vview', 'child', 'obj']),
        ('ID3D11VideoProcessorInputView', 'piv_vtbl', ['piv', 'vview', 'child', 'obj']),
        ('ID3D11VideoProcessorOutputView', 'pov_vtbl', ['pov', 'vview', 'child', 'obj']),
    ],
    'd3d9': [
        ('IDirect3D9', 'd3d_vtbl', ['d3d', 'obj']),
        ('IDirect3DDevice9', 'dev_vtbl', ['dev', 'obj']),
        ('IDirect3DVertexBuffer9', 'vb_vtbl', ['vb', 'buf', 'res9', 'child', 'obj']),
        ('IDirect3DIndexBuffer9', 'ib_vtbl', ['ib', 'buf', 'res9', 'child', 'obj']),
        ('IDirect3DTexture9', 'tex_vtbl', ['tex', 'res9', 'child', 'obj']),
        ('IDirect3DCubeTexture9', 'cube_vtbl', ['cube', 'tex', 'res9', 'child', 'obj']),
        ('IDirect3DVolumeTexture9', 'vol_vtbl', ['vol', 'tex', 'res9', 'child', 'obj']),
        ('IDirect3DSurface9', 'surf_vtbl', ['surf', 'res9', 'child', 'obj']),
        ('IDirect3DVertexDeclaration9', 'decl_vtbl', ['decl', 'child', 'obj']),
        ('IDirect3DStateBlock9', 'sb_vtbl', ['sb', 'child', 'obj']),
        ('IDirect3DVertexShader9', 'vs_vtbl', ['shader', 'child', 'obj']),
        ('IDirect3DPixelShader9', 'ps_vtbl', ['shader', 'child', 'obj']),
    ],
}

text = ''.join(open(INC + h).read() for h in HEADERS)
# (what mingw's d3d12.h lacks: the interfaces of the later Agility SDKs)
text += open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'Windows', 'd3d12', 'd3d12_types.h')).read()
# (what mingw's d3d12video.h lacks: the later video interfaces, as the Windows SDK has them)
text += open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'Windows', 'd3d12', 'video_types.h')).read()
# (... and what mingw's d3d11_4.h lacks: ID3D11VideoDevice2, ID3D11VideoContext3)
text += open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'Windows', 'd3d11', 'video_types.h')).read()

def members(iface):
    m = re.search(r'typedef struct %sVtbl \{(.*?)\} %sVtbl;' % (iface, iface), text, re.S)
    if not m:
        return macro_members(iface)
    # (name, return type, parameters as the header writes them)
    return [(mm.group(2), ' '.join(mm.group(1).split()), ' '.join(mm.group(3).split()))
            for mm in re.finditer(r'(?:BEGIN_INTERFACE\s*)?([\w\s\*]+?)\s*\(\s*(?:STDMETHODCALLTYPE|WINAPI)?\s*\*\s*(\w+)\s*\)\s*\((.*?)\);', m.group(1), re.S)]

def macro_members(iface):
    """The methods of an interface the header declares with DECLARE_INTERFACE_ and STDMETHOD (d3d9.h): the same triples."""
    m = re.search(r'#define INTERFACE %s\s*\n\s*DECLARE_INTERFACE_\w*\([^)]*\)\s*\{(.*?)\n\};' % iface, text, re.S)
    if not m:
        sys.exit('no vtable for ' + iface)
    out = []
    for mm in re.finditer(r'STDMETHOD(_)?\(\s*([^()]*?)\s*\)\s*\(\s*THIS(_?)\s*(.*?)\)\s*PURE\s*;', m.group(1), re.S):
        inner = [part.strip() for part in mm.group(2).split(',')]
        result, name = (inner[0], inner[1]) if mm.group(1) else ('HRESULT', inner[0])
        arguments = ' '.join(mm.group(4).split())
        out.append((name, result, '%s *This%s' % (iface, ', ' + arguments if mm.group(3) and arguments else '')))
    return out

problems = []

def top_level_count(arguments):
    """How many parameters a parameter list has (commas outside brackets)."""
    arguments = arguments.strip()
    if arguments in ('', 'void'):
        return 0
    depth, count = 0, 1
    for c in arguments:
        if c in '([':
            depth += 1
        elif c in ')]':
            depth -= 1
        elif c == ',' and depth == 0:
            count += 1
    return count

def check_arguments(dll, fn, after, parameters):
    """A method that is written takes as many parameters as the header gives it: the vtable is filled in through a cast, which would hide a method
    that takes more or fewer - and in a 32-bit program such a method leaves the caller's stack wrong."""
    depth, end = 1, 0
    for end, c in enumerate(after):
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                break
    have, want = top_level_count(after[:end]), top_level_count(parameters)
    if have != want:
        problems.append('%s: %s takes %d parameters, the interface gives it %d' % (dll, fn, have, want))

def stub(iface, var, name, result, parameters):
    """A method nobody wrote, with the method's own signature: in a 32-bit program a method takes its arguments off the stack, so one function cannot
    stand in for them all. It answers E_NOTIMPL, or nothing (0, NULL) where the method has no HRESULT."""
    fn = '%s_none_%s' % (var, name)
    said = 'static volatile LONG said; dx_unimplemented(&said, "%s::%s");' % (iface, name)      # (what a program missed: said once, see dxcommon.h)
    if result == 'void':
        body = said
    elif result == 'HRESULT':
        body = said + ' return E_NOTIMPL;'
    else:
        body = said + ' return (%s)0;' % result
    return fn, 'static %s STDMETHODCALLTYPE %s(%s) { %s }' % (result, fn, parameters, body)

def parameter_names(parameters):
    """The names of a method's parameters (This first), from its declaration list."""
    names, depth, part = [], 0, ''
    for ch in parameters + ',':
        if ch == ',' and depth == 0:
            words = re.findall(r'[A-Za-z_]\w*', re.sub(r'\[[^\]]*\]', '', part))
            if words:
                names.append(words[-1])
            part = ''
            continue
        depth += ch in '([' ; depth -= ch in ')]'
        part += ch
    return names

def all_named(parameters):
    """Every parameter has a name (some headers leave them out: such a method is not wrapped)."""
    depth, part, parts = 0, '', []
    for ch in parameters + ',':
        if ch == ',' and depth == 0:
            parts.append(part.strip()); part = ''; continue
        depth += ch in '([' ; depth -= ch in ')]'
        part += ch
    for p in parts:
        bare = re.sub(r'\[[^\]]*\]', '', p).strip()
        words = [w for w in re.findall(r'[A-Za-z_]\w*', bare) if w not in ('const', 'struct', 'enum', 'unsigned', 'signed', 'volatile')]
        if bare.endswith('*') or len(words) < 2:
            return False
    return True

def checked(iface, var, name, impl, parameters):
    """A written method that answers an HRESULT, wrapped so its first failures are said (dx_failed in dxcommon.h): what made a program give up."""
    fn = '%s_checked_%s' % (var, name)
    names = parameter_names(parameters)
    call = '%s(%s)' % (impl, ', '.join(['(void *)' + names[0]] + names[1:]))      # (the object as the implementation takes it: the rest have the interface's types)
    return fn, ('static HRESULT STDMETHODCALLTYPE %s(%s) { const HRESULT hr = %s; if (FAILED(hr)) { static volatile LONG said; dx_failed(&said, "%s::%s", hr); } '
                'return hr; }') % (fn, parameters, call, iface, name)

# (the DLLs named on the command line, or all of them)
only = set(sys.argv[1:])
for dll, specs in SPECS.items():
    if only and dll not in only:
        continue
    folder = os.path.join(ROOT, dll)
    source = ''
    for f in sorted(os.listdir(folder)):
        if f.endswith('.c') or (f.endswith('.inc') and f != 'vtbl.inc'):
            source += open(os.path.join(folder, f)).read()
    out = ['/* Generated by scripts/gen-dx-vtables.py: do not edit. */']
    for iface, var, prefixes in specs:
        table = ['const %sVtbl %s = {' % (iface, var)]
        for name, result, parameters in members(iface):
            impl = None
            for p in prefixes:
                fn = '%s_%s' % (p, name)
                found = re.search(r'^(?:static\s+)?[\w\s\*]*?\b%s\s*\(' % fn, source, re.M)
                if found or re.search(r'^#define %s\b' % fn, source, re.M):
                    impl = fn
                    if found:
                        check_arguments(dll, fn, source[found.end():], parameters)
                    break
            if impl is None:
                impl, definition = stub(iface, var, name, result, parameters)
                out.append(definition)
            elif result == 'HRESULT' and name != 'QueryInterface' and all_named(parameters):      # (interfaces not there are said by the QueryInterfaces)
                impl, definition = checked(iface, var, name, impl, parameters)
                out.append(definition)
            table.append('    .%s = (__typeof__(((%sVtbl *)0)->%s))%s,' % (name, iface, name, impl))
        out += table + ['};']
    open(os.path.join(folder, 'vtbl.inc'), 'w').write('\n'.join(out) + '\n')
    print('wrote', dll, 'vtbl.inc')
if problems:
    sys.exit('\n'.join(problems))
