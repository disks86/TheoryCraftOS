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

#include "Hardware/VideoCodec/Software/Av1Ipred.hpp"

// Port of dav1d's ipred_tmpl.c, ipred_prepare_tmpl.c and pal.c (BSD-2-Clause, Copyright (c) VideoLAN and dav1d authors), 16-bit samples.
// The arithmetic is dav1d's, step for step.

namespace hardware {
namespace av1 {

namespace {

#include "Hardware/VideoCodec/Software/Av1Tables.inc"

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
inline int iclip(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline int iclip_pixel(int v, int bitdepth_max) { return iclip(v, 0, bitdepth_max); }
inline int abs_int(int v) { return v < 0 ? -v : v; }
inline int ctz(unsigned v) { return __builtin_ctz(v); }

// A window onto an array that is indexed relative to an offset, like dav1d's `pixel *` pointers into the edge scratch. Negative results
// wrap to huge indices and stop at the bounds check of ArrayView.
template <class T>
struct Rel {
    ArrayView<T> view;
    int64_t offset = 0;
    T& operator[](int i) const { return view[static_cast<uint64_t>(offset + i)]; }
    Rel at(int i) const { return Rel{view, offset + i}; }
};
using ConstRel = Rel<const Pixel>;
using MutRel = Rel<Pixel>;

ConstRel edge_rel(const IntraEdge& edge, int relative) {
    return ConstRel{ArrayView<const Pixel>(edge.samples), static_cast<int64_t>(IntraEdge::kTopLeft) + relative};
}

void splat_dc(PixelView dst, int width, int height, int dc) {
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) dst.at(x, y) = static_cast<Pixel>(dc);
}

void cfl_pred_dc(PixelView dst, int width, int height, int dc, ArrayView<const int16_t> ac, int alpha, int bitdepth_max) {
    uint64_t i = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++, i++) {
            const int diff = alpha * ac[i];
            const int scaled = (abs_int(diff) + 32) >> 6;
            dst.at(x, y) = static_cast<Pixel>(iclip_pixel(dc + (diff < 0 ? -scaled : scaled), bitdepth_max));
        }
    }
}

unsigned dc_gen_top(const IntraEdge& edge, int width) {
    unsigned dc = static_cast<unsigned>(width >> 1);
    for (int i = 0; i < width; i++) dc += edge.at(1 + i);
    return dc >> ctz(static_cast<unsigned>(width));
}

unsigned dc_gen_left(const IntraEdge& edge, int height) {
    unsigned dc = static_cast<unsigned>(height >> 1);
    for (int i = 0; i < height; i++) dc += edge.at(-(1 + i));
    return dc >> ctz(static_cast<unsigned>(height));
}

unsigned dc_gen(const IntraEdge& edge, int width, int height) {
    constexpr unsigned kMultiplier1x2 = 0xAAAB;
    constexpr unsigned kMultiplier1x4 = 0x6667;
    constexpr int kBaseShift = 17;
    unsigned dc = static_cast<unsigned>((width + height) >> 1);
    for (int i = 0; i < width; i++) dc += edge.at(i + 1);
    for (int i = 0; i < height; i++) dc += edge.at(-(i + 1));
    dc >>= ctz(static_cast<unsigned>(width + height));
    if (width != height) {
        dc *= (width > height * 2 || height > width * 2) ? kMultiplier1x4 : kMultiplier1x2;
        dc >>= kBaseShift;
    }
    return dc;
}

int get_filter_strength(int wh, int angle, bool is_sm) {
    if (is_sm) {
        if (wh <= 8) {
            if (angle >= 64) return 2;
            if (angle >= 40) return 1;
        } else if (wh <= 16) {
            if (angle >= 48) return 2;
            if (angle >= 20) return 1;
        } else if (wh <= 24) {
            if (angle >= 4) return 3;
        } else {
            return 3;
        }
    } else {
        if (wh <= 8) {
            if (angle >= 56) return 1;
        } else if (wh <= 16) {
            if (angle >= 40) return 1;
        } else if (wh <= 24) {
            if (angle >= 32) return 3;
            if (angle >= 16) return 2;
            if (angle >= 8) return 1;
        } else if (wh <= 32) {
            if (angle >= 32) return 3;
            if (angle >= 4) return 2;
            return 1;
        } else {
            return 3;
        }
    }
    return 0;
}

void filter_edge(MutRel out, int sz, int lim_from, int lim_to, ConstRel in, int from, int to, int strength) {
    static constexpr uint8_t kernel[3][5] = {{0, 4, 8, 4, 0}, {0, 5, 6, 5, 0}, {2, 4, 4, 4, 2}};
    int i = 0;
    for (; i < imin(sz, lim_from); i++) out[i] = in[iclip(i, from, to - 1)];
    for (; i < imin(lim_to, sz); i++) {
        int s = 0;
        for (int j = 0; j < 5; j++) s += in[iclip(i - 2 + j, from, to - 1)] * kernel[strength - 1][j];
        out[i] = static_cast<Pixel>((s + 8) >> 4);
    }
    for (; i < sz; i++) out[i] = in[iclip(i, from, to - 1)];
}

bool get_upsample(int wh, int angle, bool is_sm) { return angle < 40 && wh <= (16 >> (is_sm ? 1 : 0)); }

void upsample_edge(MutRel out, int hsz, ConstRel in, int from, int to, int bitdepth_max) {
    static constexpr int8_t kernel[4] = {-1, 9, 9, -1};
    int i;
    for (i = 0; i < hsz - 1; i++) {
        out[i * 2] = in[iclip(i, from, to - 1)];
        int s = 0;
        for (int j = 0; j < 4; j++) s += in[iclip(i + j - 1, from, to - 1)] * kernel[j];
        out[i * 2 + 1] = static_cast<Pixel>(iclip_pixel((s + 8) >> 4, bitdepth_max));
    }
    out[i * 2] = in[iclip(i, from, to - 1)];
}

// ---- prepare_intra_edges --------------------------------------------------------------------------------------------------------------

struct EdgeNeeds {
    bool left, top, topleft, topright, bottomleft;
};

constexpr EdgeNeeds edge_needs(int mode) {
    switch (mode) {
        case DC_PRED: return {true, true, false, false, false};
        case VERT_PRED: return {false, true, false, false, false};
        case HOR_PRED: return {true, false, false, false, false};
        case LEFT_DC_PRED: return {true, false, false, false, false};
        case TOP_DC_PRED: return {false, true, false, false, false};
        case DC_128_PRED: return {false, false, false, false, false};
        case Z1_PRED: return {false, true, true, true, false};
        case Z2_PRED: return {true, true, true, false, false};
        case Z3_PRED: return {true, false, true, false, true};
        case SMOOTH_PRED:
        case SMOOTH_V_PRED:
        case SMOOTH_H_PRED: return {true, true, false, false, false};
        case PAETH_PRED: return {true, true, true, false, false};
        case FILTER_PRED: return {true, true, true, false, false};
        default: return {false, false, false, false, false};
    }
}

constexpr uint8_t av1_mode_to_angle_map[8] = {90, 180, 45, 135, 113, 157, 203, 67};

}  // namespace

IntraPredMode prepare_intra_edges(int x, bool have_left, int y, bool have_top, int w, int h, int edge_flags, PixelView dst,
                                  ArrayView<const Pixel> prefilter_toplevel_sb_edge, IntraPredMode mode, int& angle, int tw, int th,
                                  bool filter_edge, IntraEdge& topleft_out, int bitdepth_max) {
    const int bitdepth = 32 - __builtin_clz(static_cast<unsigned>(bitdepth_max));

    switch (mode) {
        case VERT_PRED:
        case HOR_PRED:
        case DIAG_DOWN_LEFT_PRED:
        case DIAG_DOWN_RIGHT_PRED:
        case VERT_RIGHT_PRED:
        case HOR_DOWN_PRED:
        case HOR_UP_PRED:
        case VERT_LEFT_PRED: {
            angle = av1_mode_to_angle_map[mode - VERT_PRED] + 3 * angle;
            if (angle <= 90)
                mode = angle < 90 && have_top ? Z1_PRED : VERT_PRED;
            else if (angle < 180)
                mode = Z2_PRED;
            else
                mode = angle > 180 && have_left ? Z3_PRED : HOR_PRED;
            break;
        }
        case DC_PRED:
            mode = have_left ? (have_top ? DC_PRED : LEFT_DC_PRED) : (have_top ? TOP_DC_PRED : DC_128_PRED);
            break;
        case PAETH_PRED:
            mode = have_left ? (have_top ? PAETH_PRED : HOR_PRED) : (have_top ? VERT_PRED : DC_128_PRED);
            break;
        default: break;
    }

    const EdgeNeeds needs = edge_needs(mode);

    // The row above the block: the saved unfiltered row of the superblock above, or the picture itself.
    const bool use_saved_top = !prefilter_toplevel_sb_edge.empty();
    auto top_at = [&](int i) -> Pixel {
        if (use_saved_top) return prefilter_toplevel_sb_edge[static_cast<uint64_t>(static_cast<int64_t>(x) * 4 + i)];
        return dst.at(i, -1);
    };

    if (needs.left) {
        const int sz = th << 2;
        if (have_left) {
            const int px_have = imin(sz, (h - y) << 2);
            for (int i = 0; i < px_have; i++) topleft_out.at(-1 - i) = dst.at(-1, i);
            for (int i = px_have; i < sz; i++) topleft_out.at(-1 - i) = topleft_out.at(-px_have);
        } else {
            const Pixel v = static_cast<Pixel>(have_top ? top_at(0) : ((1 << bitdepth) >> 1) + 1);
            for (int i = 0; i < sz; i++) topleft_out.at(-1 - i) = v;
        }

        if (needs.bottomleft) {
            const int have_bottomleft = (!have_left || y + th >= h) ? 0 : (edge_flags & EDGE_I444_LEFT_HAS_BOTTOM);
            if (have_bottomleft) {
                const int px_have = imin(sz, (h - y - th) << 2);
                for (int i = 0; i < px_have; i++) topleft_out.at(-sz - 1 - i) = dst.at(-1, sz + i);
                for (int i = px_have; i < sz; i++) topleft_out.at(-sz - 1 - i) = topleft_out.at(-sz - px_have);
            } else {
                const Pixel v = topleft_out.at(-sz);
                for (int i = 0; i < sz; i++) topleft_out.at(-sz - 1 - i) = v;
            }
        }
    }

    if (needs.top) {
        const int sz = tw << 2;
        if (have_top) {
            const int px_have = imin(sz, (w - x) << 2);
            for (int i = 0; i < px_have; i++) topleft_out.at(1 + i) = top_at(i);
            for (int i = px_have; i < sz; i++) topleft_out.at(1 + i) = topleft_out.at(px_have);
        } else {
            const Pixel v = static_cast<Pixel>(have_left ? dst.at(-1, 0) : ((1 << bitdepth) >> 1) - 1);
            for (int i = 0; i < sz; i++) topleft_out.at(1 + i) = v;
        }

        if (needs.topright) {
            const int have_topright = (!have_top || x + tw >= w) ? 0 : (edge_flags & EDGE_I444_TOP_HAS_RIGHT);
            if (have_topright) {
                const int px_have = imin(sz, (w - x - tw) << 2);
                for (int i = 0; i < px_have; i++) topleft_out.at(1 + sz + i) = top_at(sz + i);
                for (int i = px_have; i < sz; i++) topleft_out.at(1 + sz + i) = topleft_out.at(sz + px_have);
            } else {
                const Pixel v = topleft_out.at(sz);
                for (int i = 0; i < sz; i++) topleft_out.at(1 + sz + i) = v;
            }
        }
    }

    if (needs.topleft) {
        if (have_left)
            topleft_out.at(0) = have_top ? top_at(-1) : dst.at(-1, 0);
        else
            topleft_out.at(0) = static_cast<Pixel>(have_top ? top_at(0) : (1 << bitdepth) >> 1);

        if (mode == Z2_PRED && tw + th >= 6 && filter_edge)
            topleft_out.at(0) = static_cast<Pixel>(((topleft_out.at(-1) + topleft_out.at(1)) * 5 + topleft_out.at(0) * 6 + 8) >> 4);
    }

    return mode;
}

// ---- predictors -----------------------------------------------------------------------------------------------------------------------

void ipred_dc_top(PixelView dst, const IntraEdge& edge, int width, int height, int) {
    splat_dc(dst, width, height, static_cast<int>(dc_gen_top(edge, width)));
}
void ipred_dc_left(PixelView dst, const IntraEdge& edge, int width, int height, int) {
    splat_dc(dst, width, height, static_cast<int>(dc_gen_left(edge, height)));
}
void ipred_dc(PixelView dst, const IntraEdge& edge, int width, int height, int) {
    splat_dc(dst, width, height, static_cast<int>(dc_gen(edge, width, height)));
}
void ipred_dc_128(PixelView dst, int width, int height, int bitdepth_max) { splat_dc(dst, width, height, (bitdepth_max + 1) >> 1); }

void ipred_v(PixelView dst, const IntraEdge& edge, int width, int height) {
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) dst.at(x, y) = edge.at(1 + x);
}

void ipred_h(PixelView dst, const IntraEdge& edge, int width, int height) {
    for (int y = 0; y < height; y++) {
        const Pixel v = edge.at(-(1 + y));
        for (int x = 0; x < width; x++) dst.at(x, y) = v;
    }
}

void ipred_paeth(PixelView dst, const IntraEdge& edge, int width, int height) {
    const int topleft = edge.at(0);
    for (int y = 0; y < height; y++) {
        const int left = edge.at(-(y + 1));
        for (int x = 0; x < width; x++) {
            const int top = edge.at(1 + x);
            const int base = left + top - topleft;
            const int ldiff = abs_int(left - base);
            const int tdiff = abs_int(top - base);
            const int tldiff = abs_int(topleft - base);
            dst.at(x, y) = static_cast<Pixel>(ldiff <= tdiff && ldiff <= tldiff ? left : tdiff <= tldiff ? top : topleft);
        }
    }
}

void ipred_smooth(PixelView dst, const IntraEdge& edge, int width, int height) {
    const ArrayView<const uint8_t> weights_hor = ArrayView<const uint8_t>(k_sm_weights).subview(static_cast<uint64_t>(width));
    const ArrayView<const uint8_t> weights_ver = ArrayView<const uint8_t>(k_sm_weights).subview(static_cast<uint64_t>(height));
    const int right = edge.at(width), bottom = edge.at(-height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int pred = weights_ver[y] * edge.at(1 + x) + (256 - weights_ver[y]) * bottom + weights_hor[x] * edge.at(-(1 + y)) +
                             (256 - weights_hor[x]) * right;
            dst.at(x, y) = static_cast<Pixel>((pred + 256) >> 9);
        }
    }
}

void ipred_smooth_v(PixelView dst, const IntraEdge& edge, int width, int height) {
    const ArrayView<const uint8_t> weights_ver = ArrayView<const uint8_t>(k_sm_weights).subview(static_cast<uint64_t>(height));
    const int bottom = edge.at(-height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int pred = weights_ver[y] * edge.at(1 + x) + (256 - weights_ver[y]) * bottom;
            dst.at(x, y) = static_cast<Pixel>((pred + 128) >> 8);
        }
    }
}

void ipred_smooth_h(PixelView dst, const IntraEdge& edge, int width, int height) {
    const ArrayView<const uint8_t> weights_hor = ArrayView<const uint8_t>(k_sm_weights).subview(static_cast<uint64_t>(width));
    const int right = edge.at(width);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int pred = weights_hor[x] * edge.at(-(y + 1)) + (256 - weights_hor[x]) * right;
            dst.at(x, y) = static_cast<Pixel>((pred + 128) >> 8);
        }
    }
}

void ipred_z1(PixelView dst, const IntraEdge& edge, int width, int height, int angle, int bitdepth_max) {
    const bool is_sm = (angle >> 9) & 0x1;
    const bool enable_intra_edge_filter = (angle >> 10) != 0;
    angle &= 511;
    int dx = k_dr_intra_derivative[angle >> 1];
    Pixel top_out_storage[64 + 64] = {};
    const MutRel top_out{ArrayView<Pixel>(top_out_storage), 0};
    ConstRel top{};
    int max_base_x;
    const ConstRel topleft_in = edge_rel(edge, 0);
    const bool upsample_above = enable_intra_edge_filter ? get_upsample(width + height, 90 - angle, is_sm) : false;
    if (upsample_above) {
        upsample_edge(top_out, width + height, topleft_in.at(1), -1, width + imin(width, height), bitdepth_max);
        top = ConstRel{ArrayView<const Pixel>(top_out_storage), 0};
        max_base_x = 2 * (width + height) - 2;
        dx <<= 1;
    } else {
        const int filter_strength = enable_intra_edge_filter ? get_filter_strength(width + height, 90 - angle, is_sm) : 0;
        if (filter_strength) {
            filter_edge(top_out, width + height, 0, width + height, topleft_in.at(1), -1, width + imin(width, height), filter_strength);
            top = ConstRel{ArrayView<const Pixel>(top_out_storage), 0};
            max_base_x = width + height - 1;
        } else {
            top = topleft_in.at(1);
            max_base_x = width + imin(width, height) - 1;
        }
    }
    const int base_inc = 1 + (upsample_above ? 1 : 0);
    for (int y = 0, xpos = dx; y < height; y++, xpos += dx) {
        const int frac = xpos & 0x3E;
        for (int x = 0, base = xpos >> 6; x < width; x++, base += base_inc) {
            if (base < max_base_x) {
                const int v = top[base] * (64 - frac) + top[base + 1] * frac;
                dst.at(x, y) = static_cast<Pixel>((v + 32) >> 6);
            } else {
                const Pixel fill = top[max_base_x];
                for (; x < width; x++) dst.at(x, y) = fill;
                break;
            }
        }
    }
}

void ipred_z2(PixelView dst, const IntraEdge& edge, int width, int height, int angle, int max_width, int max_height, int bitdepth_max) {
    const bool is_sm = (angle >> 9) & 0x1;
    const bool enable_intra_edge_filter = (angle >> 10) != 0;
    angle &= 511;
    int dy = k_dr_intra_derivative[(angle - 90) >> 1];
    int dx = k_dr_intra_derivative[(180 - angle) >> 1];
    const bool upsample_left = enable_intra_edge_filter ? get_upsample(width + height, 180 - angle, is_sm) : false;
    const bool upsample_above = enable_intra_edge_filter ? get_upsample(width + height, angle - 90, is_sm) : false;
    Pixel edge_storage[64 + 64 + 1] = {};
    const MutRel topleft{ArrayView<Pixel>(edge_storage), 64};
    const ConstRel topleft_in = edge_rel(edge, 0);

    if (upsample_above) {
        upsample_edge(topleft, width + 1, topleft_in, 0, width + 1, bitdepth_max);
        dx <<= 1;
    } else {
        const int filter_strength = enable_intra_edge_filter ? get_filter_strength(width + height, angle - 90, is_sm) : 0;
        if (filter_strength) {
            filter_edge(topleft.at(1), width, 0, max_width, topleft_in.at(1), -1, width, filter_strength);
        } else {
            for (int i = 0; i < width; i++) topleft.at(1)[i] = topleft_in.at(1)[i];
        }
    }
    if (upsample_left) {
        upsample_edge(topleft.at(-height * 2), height + 1, topleft_in.at(-height), 0, height + 1, bitdepth_max);
        dy <<= 1;
    } else {
        const int filter_strength = enable_intra_edge_filter ? get_filter_strength(width + height, 180 - angle, is_sm) : 0;
        if (filter_strength) {
            filter_edge(topleft.at(-height), height, height - max_height, height, topleft_in.at(-height), 0, height + 1, filter_strength);
        } else {
            for (int i = 0; i < height; i++) topleft.at(-height)[i] = topleft_in.at(-height)[i];
        }
    }
    topleft[0] = topleft_in[0];

    const int up_above = upsample_above ? 1 : 0, up_left = upsample_left ? 1 : 0;
    const int base_inc_x = 1 + up_above;
    const ConstRel topleft_c{ArrayView<const Pixel>(edge_storage), 64};
    const ConstRel left = topleft_c.at(-(1 + up_left));
    for (int y = 0, xpos = ((1 + up_above) << 6) - dx; y < height; y++, xpos -= dx) {
        int base_x = xpos >> 6;
        const int frac_x = xpos & 0x3E;
        for (int x = 0, ypos = (y << (6 + up_left)) - dy; x < width; x++, base_x += base_inc_x, ypos -= dy) {
            int v;
            if (base_x >= 0) {
                v = topleft_c[base_x] * (64 - frac_x) + topleft_c[base_x + 1] * frac_x;
            } else {
                const int base_y = ypos >> 6;
                const int frac_y = ypos & 0x3E;
                v = left[-base_y] * (64 - frac_y) + left[-(base_y + 1)] * frac_y;
            }
            dst.at(x, y) = static_cast<Pixel>((v + 32) >> 6);
        }
    }
}

void ipred_z3(PixelView dst, const IntraEdge& edge, int width, int height, int angle, int bitdepth_max) {
    const bool is_sm = (angle >> 9) & 0x1;
    const bool enable_intra_edge_filter = (angle >> 10) != 0;
    angle &= 511;
    int dy = k_dr_intra_derivative[(270 - angle) >> 1];
    Pixel left_out_storage[64 + 64] = {};
    const MutRel left_out{ArrayView<Pixel>(left_out_storage), 0};
    const ConstRel topleft_in = edge_rel(edge, 0);
    ConstRel left{};
    int max_base_y;
    const bool upsample_left = enable_intra_edge_filter ? get_upsample(width + height, angle - 180, is_sm) : false;
    if (upsample_left) {
        upsample_edge(left_out, width + height, topleft_in.at(-(width + height)), imax(width - height, 0), width + height + 1, bitdepth_max);
        left = ConstRel{ArrayView<const Pixel>(left_out_storage), 2 * (width + height) - 2};
        max_base_y = 2 * (width + height) - 2;
        dy <<= 1;
    } else {
        const int filter_strength = enable_intra_edge_filter ? get_filter_strength(width + height, angle - 180, is_sm) : 0;
        if (filter_strength) {
            filter_edge(left_out, width + height, 0, width + height, topleft_in.at(-(width + height)), imax(width - height, 0),
                        width + height + 1, filter_strength);
            left = ConstRel{ArrayView<const Pixel>(left_out_storage), width + height - 1};
            max_base_y = width + height - 1;
        } else {
            left = topleft_in.at(-1);
            max_base_y = height + imin(width, height) - 1;
        }
    }
    const int base_inc = 1 + (upsample_left ? 1 : 0);
    for (int x = 0, ypos = dy; x < width; x++, ypos += dy) {
        const int frac = ypos & 0x3E;
        for (int y = 0, base = ypos >> 6; y < height; y++, base += base_inc) {
            if (base < max_base_y) {
                const int v = left[-base] * (64 - frac) + left[-(base + 1)] * frac;
                dst.at(x, y) = static_cast<Pixel>((v + 32) >> 6);
            } else {
                const Pixel fill = left[-max_base_y];
                for (; y < height; y++) dst.at(x, y) = fill;
                break;
            }
        }
    }
}

void ipred_filter(PixelView dst, const IntraEdge& edge, int width, int height, int filt_idx, int bitdepth_max) {
    filt_idx &= 511;
    if (filt_idx >= 5) return;  // not a valid filter index
    const ArrayView<const int8_t> filter(k_filter_intra_taps[filt_idx]);
    // The row above the 2-row strip that starts at row y: the edge for the first strip, the last row of the previous strip afterwards.
    for (int y = 0; y < height; y += 2) {
        auto top_row = [&](int i) -> int { return y == 0 ? edge.at(1 + i) : dst.at(i, y - 1); };
        for (int x = 0; x < width; x += 4) {
            const int p0 = x == 0 ? edge.at(-y) : top_row(x - 1);
            const int p1 = top_row(x), p2 = top_row(x + 1), p3 = top_row(x + 2), p4 = top_row(x + 3);
            const int p5 = x == 0 ? edge.at(-y - 1) : dst.at(x - 1, y);
            const int p6 = x == 0 ? edge.at(-y - 2) : dst.at(x - 1, y + 1);
            for (int n = 0; n < 8; n++) {
                const int acc = filter[static_cast<uint64_t>(n)] * p0 + filter[static_cast<uint64_t>(n + 8)] * p1 +
                                filter[static_cast<uint64_t>(n + 16)] * p2 + filter[static_cast<uint64_t>(n + 24)] * p3 +
                                filter[static_cast<uint64_t>(n + 32)] * p4 + filter[static_cast<uint64_t>(n + 40)] * p5 +
                                filter[static_cast<uint64_t>(n + 48)] * p6;
                dst.at(x + (n & 3), y + (n >> 2)) = static_cast<Pixel>(iclip_pixel((acc + 8) >> 4, bitdepth_max));
            }
        }
    }
}

bool intra_pred(IntraPredMode mode, PixelView dst, const IntraEdge& edge, int width, int height, int angle, int max_width, int max_height,
                int bitdepth_max) {
    switch (mode) {
        case DC_PRED: ipred_dc(dst, edge, width, height, bitdepth_max); return true;
        case DC_128_PRED: ipred_dc_128(dst, width, height, bitdepth_max); return true;
        case TOP_DC_PRED: ipred_dc_top(dst, edge, width, height, bitdepth_max); return true;
        case LEFT_DC_PRED: ipred_dc_left(dst, edge, width, height, bitdepth_max); return true;
        case HOR_PRED: ipred_h(dst, edge, width, height); return true;
        case VERT_PRED: ipred_v(dst, edge, width, height); return true;
        case PAETH_PRED: ipred_paeth(dst, edge, width, height); return true;
        case SMOOTH_PRED: ipred_smooth(dst, edge, width, height); return true;
        case SMOOTH_V_PRED: ipred_smooth_v(dst, edge, width, height); return true;
        case SMOOTH_H_PRED: ipred_smooth_h(dst, edge, width, height); return true;
        case Z1_PRED: ipred_z1(dst, edge, width, height, angle, bitdepth_max); return true;
        case Z2_PRED: ipred_z2(dst, edge, width, height, angle, max_width, max_height, bitdepth_max); return true;
        case Z3_PRED: ipred_z3(dst, edge, width, height, angle, bitdepth_max); return true;
        case FILTER_PRED: ipred_filter(dst, edge, width, height, angle, bitdepth_max); return true;
        default: return false;
    }
}

// ---- chroma from luma -----------------------------------------------------------------------------------------------------------------

namespace {

void cfl_ac_impl(ArrayView<int16_t> ac, PixelView ypx, int w_pad, int h_pad, int width, int height, int ss_hor, int ss_ver) {
    int y, x;
    for (y = 0; y < height - 4 * h_pad; y++) {
        const uint64_t row = static_cast<uint64_t>(y) * static_cast<uint64_t>(width);
        const int ysrc = y << ss_ver;
        for (x = 0; x < width - 4 * w_pad; x++) {
            int ac_sum = ypx.at(x << ss_hor, ysrc);
            if (ss_hor) ac_sum += ypx.at(x * 2 + 1, ysrc);
            if (ss_ver) {
                ac_sum += ypx.at(x << ss_hor, ysrc + 1);
                if (ss_hor) ac_sum += ypx.at(x * 2 + 1, ysrc + 1);
            }
            ac[row + static_cast<uint64_t>(x)] = static_cast<int16_t>(ac_sum << (1 + (ss_ver ? 0 : 1) + (ss_hor ? 0 : 1)));
        }
        for (; x < width; x++) ac[row + static_cast<uint64_t>(x)] = ac[row + static_cast<uint64_t>(x - 1)];
    }
    for (; y < height; y++) {
        const uint64_t row = static_cast<uint64_t>(y) * static_cast<uint64_t>(width);
        for (x = 0; x < width; x++) ac[row + static_cast<uint64_t>(x)] = ac[row - static_cast<uint64_t>(width) + static_cast<uint64_t>(x)];
    }

    const int log2sz = ctz(static_cast<unsigned>(width)) + ctz(static_cast<unsigned>(height));
    int sum = (1 << log2sz) >> 1;
    for (const int16_t v : ac.first(static_cast<uint64_t>(width) * static_cast<uint64_t>(height))) sum += v;
    sum >>= log2sz;
    for (int16_t& v : ac.first(static_cast<uint64_t>(width) * static_cast<uint64_t>(height))) v = static_cast<int16_t>(v - sum);
}

}  // namespace

bool cfl_ac(PixelLayout layout, ArrayView<int16_t> ac, PixelView y, int w_pad, int h_pad, int cw, int ch) {
    if (cw < 4 || ch < 4 || cw > 32 || ch > 32 || (cw & (cw - 1)) || (ch & (ch - 1))) return false;
    if (w_pad < 0 || h_pad < 0 || w_pad * 4 >= cw || h_pad * 4 >= ch) return false;
    if (ac.size() < static_cast<uint64_t>(cw) * static_cast<uint64_t>(ch)) return false;
    switch (layout) {
        case PIXEL_LAYOUT_I420: cfl_ac_impl(ac, y, w_pad, h_pad, cw, ch, 1, 1); return true;
        case PIXEL_LAYOUT_I422: cfl_ac_impl(ac, y, w_pad, h_pad, cw, ch, 1, 0); return true;
        case PIXEL_LAYOUT_I444: cfl_ac_impl(ac, y, w_pad, h_pad, cw, ch, 0, 0); return true;
        default: return false;
    }
}

bool cfl_pred(IntraPredMode mode, PixelView dst, const IntraEdge& edge, int width, int height, ArrayView<const int16_t> ac, int alpha,
              int bitdepth_max) {
    if (ac.size() < static_cast<uint64_t>(width) * static_cast<uint64_t>(height)) return false;
    int dc;
    switch (mode) {
        case DC_PRED: dc = static_cast<int>(dc_gen(edge, width, height)); break;
        case DC_128_PRED: dc = (bitdepth_max + 1) >> 1; break;
        case TOP_DC_PRED: dc = static_cast<int>(dc_gen_top(edge, width)); break;
        case LEFT_DC_PRED: dc = static_cast<int>(dc_gen_left(edge, height)); break;
        default: return false;
    }
    cfl_pred_dc(dst, width, height, dc, ac, alpha, bitdepth_max);
    return true;
}

// ---- palette --------------------------------------------------------------------------------------------------------------------------

void pal_pred(PixelView dst, ArrayView<const Pixel> pal, ArrayView<const uint8_t> idx, int w, int h) {
    uint64_t n = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x += 2) {
            const int i = idx[n++];
            dst.at(x, y) = pal[static_cast<uint64_t>(i & 7)];
            dst.at(x + 1, y) = pal[static_cast<uint64_t>((i >> 4) & 7)];
        }
    }
}

bool pal_idx_finish(ArrayView<uint8_t> dst, ArrayView<const uint8_t> src, int bw, int bh, int w, int h) {
    auto valid_block = [](int v) { return v >= 4 && v <= 64 && !(v & (v - 1)); };
    if (!valid_block(bw) || !valid_block(bh) || w < 4 || w > bw || (w & 3) || h < 4 || h > bh || (h & 3)) return false;
    const uint64_t dst_w = static_cast<uint64_t>(w) / 2;
    const uint64_t dst_bw = static_cast<uint64_t>(bw) / 2;
    if (src.size() < static_cast<uint64_t>(bw) * static_cast<uint64_t>(bh) || dst.size() < dst_bw * static_cast<uint64_t>(bh)) return false;

    for (int y = 0; y < h; y++) {
        const uint64_t srow = static_cast<uint64_t>(y) * static_cast<uint64_t>(bw);
        const uint64_t drow = static_cast<uint64_t>(y) * dst_bw;
        for (uint64_t x = 0; x < dst_w; x++) dst[drow + x] = static_cast<uint8_t>(src[srow + x * 2] | (src[srow + x * 2 + 1] << 4));
        const uint8_t pad = static_cast<uint8_t>(src[srow + static_cast<uint64_t>(w) - 1] * 0x11);
        for (uint64_t x = dst_w; x < dst_bw; x++) dst[drow + x] = pad;
    }
    for (int y = h; y < bh; y++) {
        const uint64_t drow = static_cast<uint64_t>(y) * dst_bw;
        for (uint64_t x = 0; x < dst_bw; x++) dst[drow + x] = dst[drow - dst_bw + x];
    }
    return true;
}

}  // namespace av1
}  // namespace hardware
