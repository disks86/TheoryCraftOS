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
"""Writes src/Windows/ddraw/d3d9_ddraw.h: the interfaces of Direct3D 9 (their vtables) and the structures and enumerations they take, in a form that can
be included after mingw's <ddraw.h> and <d3d.h>.

ddraw.dll's Direct3D 1 .. 7 is a layer on d3d9.dll's COM interfaces, but d3d.h (d3dtypes.h, d3dcaps.h) and d3d9.h cannot be in one translation unit: they
define the same enumerations (D3DRENDERSTATETYPE, D3DTRANSFORMSTATETYPE, D3DPRIMITIVETYPE ...) with other members. Here an enumeration of Direct3D 9 that
Direct3D 7 also has is a DWORD wherever Direct3D 9 uses it (an enumeration is 32 bits in the Windows ABI, so the calls are the same); the structures Direct3D 7
has with the same layout (D3DVECTOR, D3DCOLORVALUE, D3DRECT, D3DMATRIX) are Direct3D 7's; every other enumeration and structure is copied.

    python3 scripts/gen-d3d9-ddraw-abi.py
"""
import os
import re

INC = '/usr/x86_64-w64-mingw32/include/'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'Windows', 'ddraw', 'd3d9_ddraw.h')

d3d9 = open(INC + 'd3d9.h').read()
types = open(INC + 'd3d9types.h').read()
caps = open(INC + 'd3d9caps.h').read()
old = ''.join(open(INC + f).read() for f in ('d3dtypes.h', 'd3dcaps.h', 'd3d.h', 'ddraw.h'))
old_names = set(re.findall(r'\b(\w+)\b', old))

INTERFACES = ['IDirect3D9', 'IDirect3DVolume9', 'IDirect3DSwapChain9', 'IDirect3DResource9', 'IDirect3DSurface9', 'IDirect3DVertexBuffer9', 'IDirect3DIndexBuffer9',
              'IDirect3DBaseTexture9', 'IDirect3DCubeTexture9', 'IDirect3DTexture9', 'IDirect3DVolumeTexture9', 'IDirect3DVertexDeclaration9',
              'IDirect3DVertexShader9', 'IDirect3DPixelShader9', 'IDirect3DStateBlock9', 'IDirect3DQuery9', 'IDirect3DDevice9']

ENUMS = ['D3DDECLUSAGE', 'D3DDECLMETHOD', 'D3DDECLTYPE', 'D3DQUERYTYPE', 'D3DBACKBUFFER_TYPE', 'D3DBASISTYPE', 'D3DBLENDOP', 'D3DCUBEMAP_FACES', 'D3DDEGREETYPE',
         'D3DDEVTYPE', 'D3DFORMAT', 'D3DMULTISAMPLE_TYPE', 'D3DPATCHEDGESTYLE', 'D3DPOOL', 'D3DRESOURCETYPE', 'D3DSWAPEFFECT', 'D3DTEXTUREFILTERTYPE',
         'D3DSAMPLERSTATETYPE', 'D3DORDERTYPE']
STRUCTS = [(types, 'D3DVERTEXELEMENT9'), (types, 'D3DADAPTER_IDENTIFIER9'), (types, 'D3DBOX'), (types, 'D3DCLIPSTATUS9'), (types, 'D3DDEVICE_CREATION_PARAMETERS'),
           (types, 'D3DDISPLAYMODE'), (types, 'D3DGAMMARAMP'), (types, 'D3DINDEXBUFFER_DESC'), (types, 'D3DLIGHT9'), (types, 'D3DLOCKED_BOX'), (types, 'D3DLOCKED_RECT'),
           (types, 'D3DMATERIAL9'), (types, 'D3DPRESENT_PARAMETERS_'), (types, 'D3DRASTER_STATUS'), (types, 'D3DRECTPATCH_INFO'), (types, 'D3DSURFACE_DESC'),
           (types, 'D3DTRIPATCH_INFO'), (types, 'D3DVERTEXBUFFER_DESC'), (types, 'D3DVIEWPORT9'), (types, 'D3DVOLUME_DESC'),
           (caps, 'D3DVSHADERCAPS2_0'), (caps, 'D3DPSHADERCAPS2_0'), (caps, 'D3DCAPS9')]

# the enumerations of Direct3D 9 whose names Direct3D 7 has too: a DWORD here
all_enums = set(re.findall(r'typedef enum _?\w*\s*\{.*?\}\s*(\w+)', types + caps, re.S))
DWORD_TYPES = sorted(e for e in all_enums if e in old_names)


def dwordify(text):
    for name in DWORD_TYPES:
        text = re.sub(r'\b%s\b' % name, 'DWORD', text)
    return text


def strip_cpp(text):
    return re.sub(r'^\s*#.*\n', '', text, flags=re.M)


def struct(text, name):
    m = re.search(r'typedef struct _%s \{.*?\} [^;]*;' % name, text, re.S)
    assert m, name
    return dwordify(strip_cpp(m.group(0)))


def enum(text, name):
    m = re.search(r'typedef enum _%s \{.*?\} %s[^;]*;' % (name, name), text, re.S)
    assert m, name
    return strip_cpp(m.group(0))


def methods(iface):
    m = re.search(r'DECLARE_INTERFACE_IID_\(%s,(\w+),"([-0-9a-f]+)"\)\s*\{(.*?)\};' % iface, d3d9, re.S)
    assert m, iface
    out = []
    for mm in re.finditer(r'STDMETHOD(_\(([^,]+),\s*(\w+)\s*\)|\((\w+)\))\s*\((.*?)\)\s*PURE;', m.group(3), re.S):
        if mm.group(4):
            ret, name = 'HRESULT', mm.group(4)
        else:
            ret, name = mm.group(2).strip(), mm.group(3).strip()
        params = ' '.join(mm.group(5).split())
        params = re.sub(r'^THIS_\s*', '', params)
        params = '' if params == 'THIS' else params
        out.append((ret, name, params))
    return out


def guid(iface):
    m = re.search(r'DEFINE_GUID\(IID_%s,\s*([^)]*)\);' % iface, d3d9)
    assert m, iface
    return ' '.join(m.group(1).split())


lines = ['/* Generated by scripts/gen-d3d9-ddraw-abi.py from mingw-w64\'s d3d9.h: do not edit.',
         ' * The interfaces of Direct3D 9 for ddraw.dll, to be included after <ddraw.h> and <d3d.h>: enumerations Direct3D 7 has too are DWORDs (see the script). */',
         '#ifndef TCOS_D3D9_DDRAW_H', '#define TCOS_D3D9_DDRAW_H', '']
lines.append('/* (as DWORD here: %s) */' % ', '.join(DWORD_TYPES))
lines += ['#ifndef MAKEFOURCC', '#define MAKEFOURCC(a, b, c, d) ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))', '#endif']
lines += ['#ifndef MAX_DEVICE_IDENTIFIER_STRING', '#define MAX_DEVICE_IDENTIFIER_STRING 512', '#endif']
lines += ['#ifdef __i386__', '#pragma pack(push,4)      /* (as d3d9types.h and d3d9caps.h have their structures in 32-bit programs) */', '#endif']
for e in ENUMS:
    lines.append(enum(types, e))
for text, s in STRUCTS:
    lines.append(struct(text, s))
lines += ['typedef D3DPRESENT_PARAMETERS D3DPRESENT_PARAMETERS9;', '#ifdef __i386__', '#pragma pack(pop)', '#endif', '']
for iface in INTERFACES:
    lines.append('typedef struct %s { const struct %sVtbl *lpVtbl; } %s;' % (iface, iface, iface))
lines.append('')
for iface in INTERFACES:
    lines.append('DEFINE_GUID(IID_%s, %s);' % (iface, guid(iface)))
lines.append('')
for iface in INTERFACES:
    lines.append('typedef struct %sVtbl {' % iface)
    for ret, name, params in methods(iface):
        p = dwordify(params)
        lines.append('  %s (STDMETHODCALLTYPE *%s)(%s *This%s);' % (dwordify(ret), name, iface, (', ' + p) if p else ''))
    lines.append('} %sVtbl;' % iface)
    lines.append('')
lines.append('typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT sdk_version);')
lines.append('')
lines.append('#endif')
open(OUT, 'w').write('\n'.join(lines) + '\n')
print('wrote', OUT)
