#!/usr/bin/env python3
# Regenerates src/Kernel/Dxgkrnl/ConformanceBlobs.hpp from the shader fixtures (DXBC and Direct3D 9 bytecode) in tests/data/dxbc.
import os
root = os.path.join(os.path.dirname(__file__), '..')
LICENSE = open(os.path.join(root, 'src/Kernel/Dxgkrnl/SelfTest.cpp')).read().split('*/')[0] + '*/\n'
names = ['vs.dxbc', 'ps_color.dxbc', 'ps_tex.dxbc', 'ps_math.dxbc', 'ps_texel.dxbc', 'ps_system.dxbc', 'ps_cbuf.dxbc', 'ps_mrt.dxbc', 'ps_scale.dxbc', 'vs_vol.dxbc', 'ps_tex3d.dxbc', 'ps_texcube.dxbc', 'ps_texel3d.dxbc', 'ps_texelarray.dxbc', 'ps_texelcube.dxbc', 'ps_texarray.dxbc', 'cs_fill.dxbc', 'cs_shared.dxbc', 'gs_copy.dxbc', 'gs_copy_inst2.dxbc', 'gs_streams.dxbc', 'gs_streams_draw1.dxbc', 'gs_tri_adj.dxbc', 'gs_line_adj.dxbc', 'hs_tri.dxbc', 'ds_dent.dxbc', 'hs_tri_cb.dxbc', 'hs_tri_ccw_cb.dxbc', 'hs_tri_pow2_cb.dxbc', 'hs_tri_point_cb.dxbc', 'hs_quad_cb.dxbc', 'hs_quad_ccw_cb.dxbc', 'hs_quad_point_cb.dxbc', 'hs_iso_cb.dxbc', 'hs_iso_point_cb.dxbc', 'hs_tri_odd_cb.dxbc', 'hs_tri_even_cb.dxbc', 'hs_quad_odd_cb.dxbc', 'hs_quad_even_cb.dxbc', 'hs_iso_odd_cb.dxbc', 'hs_iso_even_cb.dxbc', 'ds_noise.dxbc', 'ds_quad.dxbc', 'ds_iso.dxbc', 'ps_scale100.dxbc', 'ps_int_write.dxbc', 'ps_int_load.dxbc', 'ps_sint_write.dxbc', 'ps_uint_loadq.dxbc', 'ps_sint_loadq.dxbc', 'ps_snorm_show.dxbc', 'ps_show.dxbc', 'ps_ms.dxbc', 'vs_ids.dxbc', 'dxil_vs.dxil', 'dxil_ps_math.dxil', 'dxil_ps_flow.dxil', 'dxil_ps_loop.dxil', 'dxil_cs_fill.dxil', 'dxil_cs_shared.dxil', 'dxil_cs_srv.dxil', 'dxil_ps_more.dxil', 'dxil_gs_copy.dxil', 'dxil_ds_dent.dxil', 'dxil_hs_tri_cb.dxil', 'dxil_hs_quad_cb.dxil', 'dxil_ds_noise.dxil', 'dxil_ds_quad.dxil', 'vs_3_0.sm3', 'ps_color_3_0.sm3', 'vs_1_1.sm1', 'ps1_lerp_1_4.sm1']
out = [LICENSE + "// Generated from tests/data/dxbc by scripts/gen_conformance_blobs.py: DXBC shaders (Shader Model 4.0) built by Wine's d3dcompiler (see the .hlsl next to each).\n#pragma once\n#include <stdint.h>\nnamespace dxgk::conformance_blobs {\n"]
for n in names:
    b = open(os.path.join(root, 'tests/data/dxbc', n), 'rb').read()
    out.append(f"inline constexpr uint8_t k_{n.split('.')[0]}[{len(b)}] = {{")
    out.append(",".join(str(x) for x in b))
    out.append("};\n")
out.append("}  // namespace dxgk::conformance_blobs\n")
open(os.path.join(root, 'src/Kernel/Dxgkrnl/ConformanceBlobs.hpp'), 'w').write("\n".join(out))
