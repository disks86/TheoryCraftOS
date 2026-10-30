#!/usr/bin/env python3
# Regenerates src/Kernel/Dxgkrnl/ConformanceBlobs.hpp from the shader fixtures (DXBC and Direct3D 9 bytecode) in tests/data/dxbc.
import os
root = os.path.join(os.path.dirname(__file__), '..')
LICENSE = open(os.path.join(root, 'src/Kernel/Dxgkrnl/SelfTest.cpp')).read().split('*/')[0] + '*/\n'
names = ['vs.dxbc', 'ps_color.dxbc', 'ps_tex.dxbc', 'ps_math.dxbc', 'ps_texel.dxbc', 'ps_system.dxbc', 'ps_cbuf.dxbc', 'ps_mrt.dxbc', 'ps_scale.dxbc', 'vs_vol.dxbc', 'ps_tex3d.dxbc', 'ps_texcube.dxbc', 'ps_texarray.dxbc', 'cs_fill.dxbc', 'gs_copy.dxbc', 'hs_tri.dxbc', 'ds_dent.dxbc', 'ps_scale100.dxbc', 'ps_int_write.dxbc', 'ps_int_load.dxbc', 'vs_ids.dxbc', 'vs_3_0.sm3', 'ps_color_3_0.sm3', 'vs_1_1.sm1', 'ps1_lerp_1_4.sm1']
out = [LICENSE + "// Generated from tests/data/dxbc by scripts/gen_conformance_blobs.py: DXBC shaders (Shader Model 4.0) built by Wine's d3dcompiler (see the .hlsl next to each).\n#pragma once\n#include <stdint.h>\nnamespace dxgk::conformance_blobs {\n"]
for n in names:
    b = open(os.path.join(root, 'tests/data/dxbc', n), 'rb').read()
    out.append(f"inline constexpr uint8_t k_{n.split('.')[0]}[{len(b)}] = {{")
    out.append(",".join(str(x) for x in b))
    out.append("};\n")
out.append("}  // namespace dxgk::conformance_blobs\n")
open(os.path.join(root, 'src/Kernel/Dxgkrnl/ConformanceBlobs.hpp'), 'w').write("\n".join(out))
