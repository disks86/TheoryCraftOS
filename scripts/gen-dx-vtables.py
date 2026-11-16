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

    python3 scripts/gen-dx-vtables.py
"""
import re, os, sys

INC = '/usr/x86_64-w64-mingw32/include/'
HEADERS = ['d3d12.h', 'd3d12sdklayers.h', 'dxgi1_6.h', 'dxgi1_5.h', 'dxgi1_4.h', 'dxgi1_3.h', 'dxgi1_2.h', 'dxgi.h', 'd3dcommon.h', 'd3d11.h']
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
        ('ID3D12Device', 'dev_vtbl', ['dev', 'dobj', 'obj']),
        ('ID3D12CommandQueue', 'queue_vtbl', ['queue', 'dobj', 'obj']),
        ('ID3D12CommandAllocator', 'alloc_vtbl', ['alloc', 'dobj', 'obj']),
        ('ID3D12GraphicsCommandList', 'list_vtbl', ['list', 'dobj', 'obj']),
        ('ID3D12Fence', 'fence_vtbl', ['fence', 'dobj', 'obj']),
        ('ID3D12Resource', 'res_vtbl', ['res', 'dobj', 'obj']),
        ('ID3D12Heap', 'heap_vtbl', ['heap', 'dobj', 'obj']),
        ('ID3D12DescriptorHeap', 'dheap_vtbl', ['dheap', 'dobj', 'obj']),
        ('ID3D12RootSignature', 'rs_vtbl', ['rs', 'dobj', 'obj']),
        ('ID3D12PipelineState', 'pso_vtbl', ['pso', 'dobj', 'obj']),
        ('ID3D12QueryHeap', 'qheap_vtbl', ['qheap', 'dobj', 'obj']),
        ('ID3D12CommandSignature', 'sig_vtbl', ['sig', 'dobj', 'obj']),
        ('ID3D10Blob', 'blob_vtbl', ['blob', 'obj']),
    ],
    'd3d11': [
        ('ID3D11Device', 'dev_vtbl', ['dev', 'obj']),
        ('ID3D11DeviceContext', 'ctx_vtbl', ['ctx', 'child', 'obj']),
        ('ID3D11Buffer', 'buf_vtbl', ['buf', 'child', 'obj']),
        ('ID3D11Texture2D', 'tex_vtbl', ['tex', 'child', 'obj']),
        ('ID3D11RenderTargetView', 'rtv_vtbl', ['rtv', 'view', 'child', 'obj']),
        ('ID3D11DepthStencilView', 'dsv_vtbl', ['dsv', 'view', 'child', 'obj']),
        ('ID3D11ShaderResourceView', 'srv_vtbl', ['srv', 'view', 'child', 'obj']),
        ('ID3D11VertexShader', 'vs_vtbl', ['child', 'obj']),
        ('ID3D11PixelShader', 'ps_vtbl', ['child', 'obj']),
        ('ID3D11InputLayout', 'layout_vtbl', ['child', 'obj']),
        ('ID3D11BlendState', 'blend_vtbl', ['blend', 'child', 'obj']),
        ('ID3D11DepthStencilState', 'depth_vtbl', ['depth', 'child', 'obj']),
        ('ID3D11RasterizerState', 'raster_vtbl', ['raster', 'child', 'obj']),
        ('ID3D11SamplerState', 'sampler_vtbl', ['sampler', 'child', 'obj']),
    ],
}

text = ''.join(open(INC + h).read() for h in HEADERS)

def members(iface):
    m = re.search(r'typedef struct %sVtbl \{(.*?)\} %sVtbl;' % (iface, iface), text, re.S)
    if not m:
        sys.exit('no vtable for ' + iface)
    # (name, return type, parameters as the header writes them)
    return [(mm.group(2), ' '.join(mm.group(1).split()), ' '.join(mm.group(3).split()))
            for mm in re.finditer(r'(?:BEGIN_INTERFACE\s*)?([\w\s\*]+?)\s*\(\s*(?:STDMETHODCALLTYPE|WINAPI)?\s*\*\s*(\w+)\s*\)\s*\((.*?)\);', m.group(1), re.S)]

def stub(var, name, result, parameters):
    """A method nobody wrote, with the method's own signature: in a 32-bit program a method takes its arguments off the stack, so one function cannot
    stand in for them all. It answers E_NOTIMPL, or nothing (0, NULL) where the method has no HRESULT."""
    fn = '%s_none_%s' % (var, name)
    if result == 'void':
        body = ''
    elif result == 'HRESULT':
        body = 'return E_NOTIMPL;'
    else:
        body = 'return (%s)0;' % result
    return fn, 'static %s STDMETHODCALLTYPE %s(%s) { %s }' % (result, fn, parameters, body)

for dll, specs in SPECS.items():
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
                if re.search(r'^(?:static\s+)?[\w\s\*]*?\b%s\s*\(' % fn, source, re.M) or re.search(r'^#define %s\b' % fn, source, re.M):
                    impl = fn
                    break
            if impl is None:
                impl, definition = stub(var, name, result, parameters)
                out.append(definition)
            table.append('    .%s = (__typeof__(((%sVtbl *)0)->%s))%s,' % (name, iface, name, impl))
        out += table + ['};']
    open(os.path.join(folder, 'vtbl.inc'), 'w').write('\n'.join(out) + '\n')
    print('wrote', dll, 'vtbl.inc')
