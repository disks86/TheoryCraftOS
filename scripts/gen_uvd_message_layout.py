#!/usr/bin/env python3
"""Generates AmdUvd6MessageLayout.inc: the byte offsets and sizes of the fields of the decode messages the UVD 6 firmware reads (the message header, the create and
decode messages and the H.264, H.265, MPEG-2 and VC-1 picture parameters), measured by compiling the structure definitions in Mesa's ac_uvd_dec.h with a C compiler, so none is typed in by hand.

  gen_uvd_message_layout.py out.inc ac_uvd_dec.h

The header is untrusted input: it is only compiled (never run on its own), in a temporary directory, and the program built from it is a few lines of offsetof.
"""
import os
import subprocess
import sys
import tempfile

# (name in the .inc, member path in struct ruvd_msg)
FIELDS = (
    [('Msg_' + f, f) for f in ('size', 'msg_type', 'stream_handle', 'status_report_feedback_number')]
    + [('Create_' + f, 'body.create.' + f) for f in ('stream_type', 'session_flags', 'asic_id', 'width_in_samples', 'height_in_samples', 'dpb_buffer', 'dpb_size', 'dpb_model', 'version_info')]
    + [('Decode_' + f, 'body.decode.' + f) for f in (
        'stream_type', 'decode_flags', 'width_in_samples', 'height_in_samples', 'dpb_buffer', 'dpb_size', 'dpb_model', 'dpb_reserved', 'db_offset_alignment', 'db_pitch',
        'db_tiling_mode', 'db_array_mode', 'db_field_mode', 'db_surf_tile_config', 'db_aligned_height', 'db_reserved', 'use_addr_macro', 'bsd_buffer', 'bsd_size',
        'pic_param_buffer', 'pic_param_size', 'mb_cntl_buffer', 'mb_cntl_size', 'dt_buffer', 'dt_pitch', 'dt_tiling_mode', 'dt_array_mode', 'dt_field_mode',
        'dt_luma_top_offset', 'dt_luma_bottom_offset', 'dt_chroma_top_offset', 'dt_chroma_bottom_offset', 'dt_surf_tile_config', 'dt_uv_surf_tile_config',
        'dt_wa_chroma_top_offset', 'dt_wa_chroma_bottom_offset', 'reserved', 'extension_support')]
    + [('H264_' + f, 'body.decode.codec.h264.' + f) for f in (
        'profile', 'level', 'sps_info_flags', 'pps_info_flags', 'chroma_format', 'bit_depth_luma_minus8', 'bit_depth_chroma_minus8', 'log2_max_frame_num_minus4',
        'pic_order_cnt_type', 'log2_max_pic_order_cnt_lsb_minus4', 'num_ref_frames', 'pic_init_qp_minus26', 'pic_init_qs_minus26', 'chroma_qp_index_offset',
        'second_chroma_qp_index_offset', 'num_slice_groups_minus1', 'slice_group_map_type', 'num_ref_idx_l0_active_minus1', 'num_ref_idx_l1_active_minus1',
        'slice_group_change_rate_minus1', 'scaling_list_4x4', 'scaling_list_8x8', 'frame_num', 'frame_num_list', 'curr_field_order_cnt_list', 'field_order_cnt_list',
        'decoded_pic_idx', 'curr_pic_ref_frame_num', 'ref_frame_list')]
    + [('H265_' + f, 'body.decode.codec.h265.' + f) for f in (
        'sps_info_flags', 'pps_info_flags', 'chroma_format', 'bit_depth_luma_minus8', 'bit_depth_chroma_minus8', 'log2_max_pic_order_cnt_lsb_minus4',
        'sps_max_dec_pic_buffering_minus1', 'log2_min_luma_coding_block_size_minus3', 'log2_diff_max_min_luma_coding_block_size', 'log2_min_transform_block_size_minus2',
        'log2_diff_max_min_transform_block_size', 'max_transform_hierarchy_depth_inter', 'max_transform_hierarchy_depth_intra', 'pcm_sample_bit_depth_luma_minus1',
        'pcm_sample_bit_depth_chroma_minus1', 'log2_min_pcm_luma_coding_block_size_minus3', 'log2_diff_max_min_pcm_luma_coding_block_size', 'num_extra_slice_header_bits',
        'num_short_term_ref_pic_sets', 'num_long_term_ref_pic_sps', 'num_ref_idx_l0_default_active_minus1', 'num_ref_idx_l1_default_active_minus1', 'pps_cb_qp_offset',
        'pps_cr_qp_offset', 'pps_beta_offset_div2', 'pps_tc_offset_div2', 'diff_cu_qp_delta_depth', 'num_tile_columns_minus1', 'num_tile_rows_minus1',
        'log2_parallel_merge_level_minus2', 'column_width_minus1', 'row_height_minus1', 'init_qp_minus26', 'num_delta_pocs_ref_rps_idx', 'curr_idx', 'curr_poc',
        'ref_pic_list', 'poc_list', 'ref_pic_set_st_curr_before', 'ref_pic_set_st_curr_after', 'ref_pic_set_lt_curr', 'ucScalingListDCCoefSizeID2',
        'ucScalingListDCCoefSizeID3', 'direct_reflist')]
    + [('Mpeg2_' + f, 'body.decode.codec.mpeg2.' + f) for f in (
        'decoded_pic_idx', 'ref_pic_idx', 'load_intra_quantiser_matrix', 'load_nonintra_quantiser_matrix', 'intra_quantiser_matrix', 'nonintra_quantiser_matrix',
        'profile_and_level_indication', 'chroma_format', 'picture_coding_type', 'f_code', 'intra_dc_precision', 'pic_structure', 'top_field_first', 'frame_pred_frame_dct',
        'concealment_motion_vectors', 'q_scale_type', 'intra_vlc_format', 'alternate_scan')]
    + [('Vc1_' + f, 'body.decode.codec.vc1.' + f) for f in ('profile', 'level', 'sps_info_flags', 'pps_info_flags', 'pic_structure', 'chroma_format')]
)
SIZES = [('Msg_sizeof', 'struct ruvd_msg'), ('H264_sizeof', 'struct ruvd_h264'), ('H265_sizeof', 'struct ruvd_h265'), ('Mpeg2_sizeof', 'struct ruvd_mpeg2'), ('Vc1_sizeof', 'struct ruvd_vc1')]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    out, header = sys.argv[1], os.path.abspath(sys.argv[2])
    lines = ['#include <stdio.h>', '#include <stddef.h>', '#include <stdint.h>', f'#include "{header}"', 'int main(void) {']
    for name, path in FIELDS:
        lines.append(f'  printf("constexpr uint32_t kUvd_{name} = %zu;\\n", offsetof(struct ruvd_msg, {path}));')
    for name, ctype in SIZES:
        lines.append(f'  printf("constexpr uint32_t kUvd_{name} = %zu;\\n", sizeof({ctype}));')
    lines += ['  return 0;', '}']
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = os.path.join(tmp, 'layout.c'), os.path.join(tmp, 'layout')
        open(src, 'w').write('\n'.join(lines) + '\n')
        subprocess.run(['cc', '-std=gnu11', '-w', '-o', exe, src], check=True)
        text = subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    with open(out, 'w') as f:
        f.write("// Generated by scripts/gen_uvd_message_layout.py from the structures in Mesa's ac_uvd_dec.h (Copyright Advanced Micro Devices, Inc.; MIT license).\n")
        f.write('// Byte offsets of the fields of the messages the UVD 6 firmware reads. Do not edit by hand; run the script again.\n\n')
        f.write(text)
    print(f'{len(FIELDS) + len(SIZES)} constants')


main()
