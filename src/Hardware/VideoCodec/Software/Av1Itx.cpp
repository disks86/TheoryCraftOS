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

#include "Hardware/VideoCodec/Software/Av1Itx.hpp"

// Port of dav1d's src/itx_1d.c and src/itx_tmpl.c (BSD-2-Clause, Copyright (c) 2018-2019 VideoLAN and dav1d authors, Two Orioles LLC).
// The 1-D transforms are dav1d's, line for line (including its arithmetic tricks that keep every intermediate in 32 bits); the only
// change is that the strided int32_t pointer became a Lane, a bounds-checked strided window onto the block being transformed.

namespace hardware {
namespace av1 {

namespace {

#include "Hardware/VideoCodec/Software/Av1Scan.inc"

// Every 1-D transform of a block works on one row or one column of the intermediate buffer: element i is v[base + i * stride].
struct Lane {
  ArrayView<int32_t> v;
  int64_t base;
  int64_t stride;

  int32_t& operator[](int i) const {
    const int64_t index = base + static_cast<int64_t>(i) * stride;
    if (index < 0) bounds_violation("Lane::operator[]", 0, v.size());
    return v[static_cast<uint64_t>(index)];
  }
  // Every second element (dav1d's `stride << 1`).
  Lane doubled() const { return Lane{v, base, stride * 2}; }
  // The same elements in reverse order (dav1d's `&c[(n - 1) * stride], -stride`, used by the flipped ADST).
  Lane reversed(int n) const { return Lane{v, base + static_cast<int64_t>(n - 1) * stride, -stride}; }
};

inline int iclip(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

#define CLIP(a) iclip(a, min, max)

/*
 * In some places, we use the pattern like this:
 * t2 = ((in1 *  1567         - in3 * (3784 - 4096) + 2048) >> 12) - in3;
 * even though the reference code might use something like:
 * t2 =  (in1 *  1567         - in3 *  3784         + 2048) >> 12;
 *
 * The reason for this is that for 12 bits/component bitstreams (corrupt/invalid ones, but they are codable nonetheless), each
 * coefficient or input can be 19(+sign) bits, and therefore if the combination of the two multipliers (each 12 bits) is >= 4096,
 * the result of the add/sub after the pair of multiplies will exceed the 31+sign bit range. Signed integer overflows are UB, so
 * one of the two multipliers is inverted (or, if both are multiples of 2, reduced in magnitude by one bit).
 */

static void
inv_dct4_1d_internal(const Lane& c, const int min, const int max, const int tx64)
{
    const int in0 = c[0], in1 = c[1];

    int t0, t1, t2, t3;
    if (tx64) {
        t0 = t1 = (in0 * 181 + 128) >> 8;
        t2 = (in1 * 1567 + 2048) >> 12;
        t3 = (in1 * 3784 + 2048) >> 12;
    } else {
        const int in2 = c[2], in3 = c[3];

        t0 = ((in0 + in2) * 181 + 128) >> 8;
        t1 = ((in0 - in2) * 181 + 128) >> 8;
        t2 = ((in1 *  1567         - in3 * (3784 - 4096) + 2048) >> 12) - in3;
        t3 = ((in1 * (3784 - 4096) + in3 *  1567         + 2048) >> 12) + in1;
    }

    c[0] = CLIP(t0 + t3);
    c[1] = CLIP(t1 + t2);
    c[2] = CLIP(t1 - t2);
    c[3] = CLIP(t0 - t3);
}

static void inv_dct4_1d(const Lane& c, const int min, const int max)
{
    inv_dct4_1d_internal(c, min, max, 0);
}

static void
inv_dct8_1d_internal(const Lane& c, const int min, const int max, const int tx64)
{
    inv_dct4_1d_internal(c.doubled(), min, max, tx64);

    const int in1 = c[1], in3 = c[3];

    int t4a, t5a, t6a, t7a;
    if (tx64) {
        t4a = (in1 *   799 + 2048) >> 12;
        t5a = (in3 * -2276 + 2048) >> 12;
        t6a = (in3 *  3406 + 2048) >> 12;
        t7a = (in1 *  4017 + 2048) >> 12;
    } else {
        const int in5 = c[5], in7 = c[7];

        t4a = ((in1 *   799         - in7 * (4017 - 4096) + 2048) >> 12) - in7;
        t5a =  (in5 *  1703         - in3 *  1138         + 1024) >> 11;
        t6a =  (in5 *  1138         + in3 *  1703         + 1024) >> 11;
        t7a = ((in1 * (4017 - 4096) + in7 *  799          + 2048) >> 12) + in1;
    }

    const int t4  = CLIP(t4a + t5a);
              t5a = CLIP(t4a - t5a);
    const int t7  = CLIP(t7a + t6a);
              t6a = CLIP(t7a - t6a);

    const int t5  = ((t6a - t5a) * 181 + 128) >> 8;
    const int t6  = ((t6a + t5a) * 181 + 128) >> 8;

    const int t0 = c[0];
    const int t1 = c[2];
    const int t2 = c[4];
    const int t3 = c[6];

    c[0] = CLIP(t0 + t7);
    c[1] = CLIP(t1 + t6);
    c[2] = CLIP(t2 + t5);
    c[3] = CLIP(t3 + t4);
    c[4] = CLIP(t3 - t4);
    c[5] = CLIP(t2 - t5);
    c[6] = CLIP(t1 - t6);
    c[7] = CLIP(t0 - t7);
}

static void inv_dct8_1d(const Lane& c, const int min, const int max)
{
    inv_dct8_1d_internal(c, min, max, 0);
}

static void
inv_dct16_1d_internal(const Lane& c, const int min, const int max, const int tx64)
{
    inv_dct8_1d_internal(c.doubled(), min, max, tx64);

    const int in1 = c[1], in3 = c[3];
    const int in5 = c[5], in7 = c[7];

    int t8a, t9a, t10a, t11a, t12a, t13a, t14a, t15a;
    if (tx64) {
        t8a  = (in1 *   401 + 2048) >> 12;
        t9a  = (in7 * -2598 + 2048) >> 12;
        t10a = (in5 *  1931 + 2048) >> 12;
        t11a = (in3 * -1189 + 2048) >> 12;
        t12a = (in3 *  3920 + 2048) >> 12;
        t13a = (in5 *  3612 + 2048) >> 12;
        t14a = (in7 *  3166 + 2048) >> 12;
        t15a = (in1 *  4076 + 2048) >> 12;
    } else {
        const int in9  = c[9], in11 = c[11];
        const int in13 = c[13], in15 = c[15];

        t8a  = ((in1  *   401         - in15 * (4076 - 4096) + 2048) >> 12) - in15;
        t9a  =  (in9  *  1583         - in7  *  1299         + 1024) >> 11;
        t10a = ((in5  *  1931         - in11 * (3612 - 4096) + 2048) >> 12) - in11;
        t11a = ((in13 * (3920 - 4096) - in3  *  1189         + 2048) >> 12) + in13;
        t12a = ((in13 *  1189         + in3  * (3920 - 4096) + 2048) >> 12) + in3;
        t13a = ((in5  * (3612 - 4096) + in11 *  1931         + 2048) >> 12) + in5;
        t14a =  (in9  *  1299         + in7  *  1583         + 1024) >> 11;
        t15a = ((in1  * (4076 - 4096) + in15 *   401         + 2048) >> 12) + in1;
    }

    int t8  = CLIP(t8a  + t9a);
    int t9  = CLIP(t8a  - t9a);
    int t10 = CLIP(t11a - t10a);
    int t11 = CLIP(t11a + t10a);
    int t12 = CLIP(t12a + t13a);
    int t13 = CLIP(t12a - t13a);
    int t14 = CLIP(t15a - t14a);
    int t15 = CLIP(t15a + t14a);

    t9a  = ((  t14 *  1567         - t9  * (3784 - 4096)  + 2048) >> 12) - t9;
    t14a = ((  t14 * (3784 - 4096) + t9  *  1567          + 2048) >> 12) + t14;
    t10a = ((-(t13 * (3784 - 4096) + t10 *  1567)         + 2048) >> 12) - t13;
    t13a = ((  t13 *  1567         - t10 * (3784 - 4096)  + 2048) >> 12) - t10;

    t8a  = CLIP(t8   + t11);
    t9   = CLIP(t9a  + t10a);
    t10  = CLIP(t9a  - t10a);
    t11a = CLIP(t8   - t11);
    t12a = CLIP(t15  - t12);
    t13  = CLIP(t14a - t13a);
    t14  = CLIP(t14a + t13a);
    t15a = CLIP(t15  + t12);

    t10a = ((t13  - t10)  * 181 + 128) >> 8;
    t13a = ((t13  + t10)  * 181 + 128) >> 8;
    t11  = ((t12a - t11a) * 181 + 128) >> 8;
    t12  = ((t12a + t11a) * 181 + 128) >> 8;

    const int t0 = c[0];
    const int t1 = c[2];
    const int t2 = c[4];
    const int t3 = c[6];
    const int t4 = c[8];
    const int t5 = c[10];
    const int t6 = c[12];
    const int t7 = c[14];

    c[0] = CLIP(t0 + t15a);
    c[1] = CLIP(t1 + t14);
    c[2] = CLIP(t2 + t13a);
    c[3] = CLIP(t3 + t12);
    c[4] = CLIP(t4 + t11);
    c[5] = CLIP(t5 + t10a);
    c[6] = CLIP(t6 + t9);
    c[7] = CLIP(t7 + t8a);
    c[8] = CLIP(t7 - t8a);
    c[9] = CLIP(t6 - t9);
    c[10] = CLIP(t5 - t10a);
    c[11] = CLIP(t4 - t11);
    c[12] = CLIP(t3 - t12);
    c[13] = CLIP(t2 - t13a);
    c[14] = CLIP(t1 - t14);
    c[15] = CLIP(t0 - t15a);
}

static void inv_dct16_1d(const Lane& c, const int min, const int max)
{
    inv_dct16_1d_internal(c, min, max, 0);
}

static void
inv_dct32_1d_internal(const Lane& c, const int min, const int max, const int tx64)
{
    inv_dct16_1d_internal(c.doubled(), min, max, tx64);

    const int in1  = c[1], in3  = c[3];
    const int in5  = c[5], in7  = c[7];
    const int in9  = c[9], in11 = c[11];
    const int in13 = c[13], in15 = c[15];

    int t16a, t17a, t18a, t19a, t20a, t21a, t22a, t23a;
    int t24a, t25a, t26a, t27a, t28a, t29a, t30a, t31a;
    if (tx64) {
        t16a = (in1  *   201 + 2048) >> 12;
        t17a = (in15 * -2751 + 2048) >> 12;
        t18a = (in9  *  1751 + 2048) >> 12;
        t19a = (in7  * -1380 + 2048) >> 12;
        t20a = (in5  *   995 + 2048) >> 12;
        t21a = (in11 * -2106 + 2048) >> 12;
        t22a = (in13 *  2440 + 2048) >> 12;
        t23a = (in3  *  -601 + 2048) >> 12;
        t24a = (in3  *  4052 + 2048) >> 12;
        t25a = (in13 *  3290 + 2048) >> 12;
        t26a = (in11 *  3513 + 2048) >> 12;
        t27a = (in5  *  3973 + 2048) >> 12;
        t28a = (in7  *  3857 + 2048) >> 12;
        t29a = (in9  *  3703 + 2048) >> 12;
        t30a = (in15 *  3035 + 2048) >> 12;
        t31a = (in1  *  4091 + 2048) >> 12;
    } else {
        const int in17 = c[17], in19 = c[19];
        const int in21 = c[21], in23 = c[23];
        const int in25 = c[25], in27 = c[27];
        const int in29 = c[29], in31 = c[31];

        t16a = ((in1  *   201         - in31 * (4091 - 4096) + 2048) >> 12) - in31;
        t17a = ((in17 * (3035 - 4096) - in15 *  2751         + 2048) >> 12) + in17;
        t18a = ((in9  *  1751         - in23 * (3703 - 4096) + 2048) >> 12) - in23;
        t19a = ((in25 * (3857 - 4096) - in7  *  1380         + 2048) >> 12) + in25;
        t20a = ((in5  *   995         - in27 * (3973 - 4096) + 2048) >> 12) - in27;
        t21a = ((in21 * (3513 - 4096) - in11 *  2106         + 2048) >> 12) + in21;
        t22a =  (in13 *  1220         - in19 *  1645         + 1024) >> 11;
        t23a = ((in29 * (4052 - 4096) - in3  *   601         + 2048) >> 12) + in29;
        t24a = ((in29 *   601         + in3  * (4052 - 4096) + 2048) >> 12) + in3;
        t25a =  (in13 *  1645         + in19 *  1220         + 1024) >> 11;
        t26a = ((in21 *  2106         + in11 * (3513 - 4096) + 2048) >> 12) + in11;
        t27a = ((in5  * (3973 - 4096) + in27 *   995         + 2048) >> 12) + in5;
        t28a = ((in25 *  1380         + in7  * (3857 - 4096) + 2048) >> 12) + in7;
        t29a = ((in9  * (3703 - 4096) + in23 *  1751         + 2048) >> 12) + in9;
        t30a = ((in17 *  2751         + in15 * (3035 - 4096) + 2048) >> 12) + in15;
        t31a = ((in1  * (4091 - 4096) + in31 *   201         + 2048) >> 12) + in1;
    }

    int t16 = CLIP(t16a + t17a);
    int t17 = CLIP(t16a - t17a);
    int t18 = CLIP(t19a - t18a);
    int t19 = CLIP(t19a + t18a);
    int t20 = CLIP(t20a + t21a);
    int t21 = CLIP(t20a - t21a);
    int t22 = CLIP(t23a - t22a);
    int t23 = CLIP(t23a + t22a);
    int t24 = CLIP(t24a + t25a);
    int t25 = CLIP(t24a - t25a);
    int t26 = CLIP(t27a - t26a);
    int t27 = CLIP(t27a + t26a);
    int t28 = CLIP(t28a + t29a);
    int t29 = CLIP(t28a - t29a);
    int t30 = CLIP(t31a - t30a);
    int t31 = CLIP(t31a + t30a);

    t17a = ((  t30 *   799         - t17 * (4017 - 4096)  + 2048) >> 12) - t17;
    t30a = ((  t30 * (4017 - 4096) + t17 *   799          + 2048) >> 12) + t30;
    t18a = ((-(t29 * (4017 - 4096) + t18 *   799)         + 2048) >> 12) - t29;
    t29a = ((  t29 *   799         - t18 * (4017 - 4096)  + 2048) >> 12) - t18;
    t21a =  (  t26 *  1703         - t21 *  1138          + 1024) >> 11;
    t26a =  (  t26 *  1138         + t21 *  1703          + 1024) >> 11;
    t22a =  (-(t25 *  1138         + t22 *  1703        ) + 1024) >> 11;
    t25a =  (  t25 *  1703         - t22 *  1138          + 1024) >> 11;

    t16a = CLIP(t16  + t19);
    t17  = CLIP(t17a + t18a);
    t18  = CLIP(t17a - t18a);
    t19a = CLIP(t16  - t19);
    t20a = CLIP(t23  - t20);
    t21  = CLIP(t22a - t21a);
    t22  = CLIP(t22a + t21a);
    t23a = CLIP(t23  + t20);
    t24a = CLIP(t24  + t27);
    t25  = CLIP(t25a + t26a);
    t26  = CLIP(t25a - t26a);
    t27a = CLIP(t24  - t27);
    t28a = CLIP(t31  - t28);
    t29  = CLIP(t30a - t29a);
    t30  = CLIP(t30a + t29a);
    t31a = CLIP(t31  + t28);

    t18a = ((  t29  *  1567         - t18  * (3784 - 4096)  + 2048) >> 12) - t18;
    t29a = ((  t29  * (3784 - 4096) + t18  *  1567          + 2048) >> 12) + t29;
    t19  = ((  t28a *  1567         - t19a * (3784 - 4096)  + 2048) >> 12) - t19a;
    t28  = ((  t28a * (3784 - 4096) + t19a *  1567          + 2048) >> 12) + t28a;
    t20  = ((-(t27a * (3784 - 4096) + t20a *  1567)         + 2048) >> 12) - t27a;
    t27  = ((  t27a *  1567         - t20a * (3784 - 4096)  + 2048) >> 12) - t20a;
    t21a = ((-(t26  * (3784 - 4096) + t21  *  1567)         + 2048) >> 12) - t26;
    t26a = ((  t26  *  1567         - t21  * (3784 - 4096)  + 2048) >> 12) - t21;

    t16  = CLIP(t16a + t23a);
    t17a = CLIP(t17  + t22);
    t18  = CLIP(t18a + t21a);
    t19a = CLIP(t19  + t20);
    t20a = CLIP(t19  - t20);
    t21  = CLIP(t18a - t21a);
    t22a = CLIP(t17  - t22);
    t23  = CLIP(t16a - t23a);
    t24  = CLIP(t31a - t24a);
    t25a = CLIP(t30  - t25);
    t26  = CLIP(t29a - t26a);
    t27a = CLIP(t28  - t27);
    t28a = CLIP(t28  + t27);
    t29  = CLIP(t29a + t26a);
    t30a = CLIP(t30  + t25);
    t31  = CLIP(t31a + t24a);

    t20  = ((t27a - t20a) * 181 + 128) >> 8;
    t27  = ((t27a + t20a) * 181 + 128) >> 8;
    t21a = ((t26  - t21 ) * 181 + 128) >> 8;
    t26a = ((t26  + t21 ) * 181 + 128) >> 8;
    t22  = ((t25a - t22a) * 181 + 128) >> 8;
    t25  = ((t25a + t22a) * 181 + 128) >> 8;
    t23a = ((t24  - t23 ) * 181 + 128) >> 8;
    t24a = ((t24  + t23 ) * 181 + 128) >> 8;

    const int t0  = c[0];
    const int t1  = c[2];
    const int t2  = c[4];
    const int t3  = c[6];
    const int t4  = c[8];
    const int t5  = c[10];
    const int t6  = c[12];
    const int t7  = c[14];
    const int t8  = c[16];
    const int t9  = c[18];
    const int t10 = c[20];
    const int t11 = c[22];
    const int t12 = c[24];
    const int t13 = c[26];
    const int t14 = c[28];
    const int t15 = c[30];

    c[0] = CLIP(t0  + t31);
    c[1] = CLIP(t1  + t30a);
    c[2] = CLIP(t2  + t29);
    c[3] = CLIP(t3  + t28a);
    c[4] = CLIP(t4  + t27);
    c[5] = CLIP(t5  + t26a);
    c[6] = CLIP(t6  + t25);
    c[7] = CLIP(t7  + t24a);
    c[8] = CLIP(t8  + t23a);
    c[9] = CLIP(t9  + t22);
    c[10] = CLIP(t10 + t21a);
    c[11] = CLIP(t11 + t20);
    c[12] = CLIP(t12 + t19a);
    c[13] = CLIP(t13 + t18);
    c[14] = CLIP(t14 + t17a);
    c[15] = CLIP(t15 + t16);
    c[16] = CLIP(t15 - t16);
    c[17] = CLIP(t14 - t17a);
    c[18] = CLIP(t13 - t18);
    c[19] = CLIP(t12 - t19a);
    c[20] = CLIP(t11 - t20);
    c[21] = CLIP(t10 - t21a);
    c[22] = CLIP(t9  - t22);
    c[23] = CLIP(t8  - t23a);
    c[24] = CLIP(t7  - t24a);
    c[25] = CLIP(t6  - t25);
    c[26] = CLIP(t5  - t26a);
    c[27] = CLIP(t4  - t27);
    c[28] = CLIP(t3  - t28a);
    c[29] = CLIP(t2  - t29);
    c[30] = CLIP(t1  - t30a);
    c[31] = CLIP(t0  - t31);
}

static void inv_dct32_1d(const Lane& c, const int min, const int max)
{
    inv_dct32_1d_internal(c, min, max, 0);
}

static void inv_dct64_1d(const Lane& c, const int min, const int max)
{
    inv_dct32_1d_internal(c.doubled(), min, max, 1);

    const int in1  = c[1], in3  = c[3];
    const int in5  = c[5], in7  = c[7];
    const int in9  = c[9], in11 = c[11];
    const int in13 = c[13], in15 = c[15];
    const int in17 = c[17], in19 = c[19];
    const int in21 = c[21], in23 = c[23];
    const int in25 = c[25], in27 = c[27];
    const int in29 = c[29], in31 = c[31];

    int t32a = (in1  *   101 + 2048) >> 12;
    int t33a = (in31 * -2824 + 2048) >> 12;
    int t34a = (in17 *  1660 + 2048) >> 12;
    int t35a = (in15 * -1474 + 2048) >> 12;
    int t36a = (in9  *   897 + 2048) >> 12;
    int t37a = (in23 * -2191 + 2048) >> 12;
    int t38a = (in25 *  2359 + 2048) >> 12;
    int t39a = (in7  *  -700 + 2048) >> 12;
    int t40a = (in5  *   501 + 2048) >> 12;
    int t41a = (in27 * -2520 + 2048) >> 12;
    int t42a = (in21 *  2019 + 2048) >> 12;
    int t43a = (in11 * -1092 + 2048) >> 12;
    int t44a = (in13 *  1285 + 2048) >> 12;
    int t45a = (in19 * -1842 + 2048) >> 12;
    int t46a = (in29 *  2675 + 2048) >> 12;
    int t47a = (in3  *  -301 + 2048) >> 12;
    int t48a = (in3  *  4085 + 2048) >> 12;
    int t49a = (in29 *  3102 + 2048) >> 12;
    int t50a = (in19 *  3659 + 2048) >> 12;
    int t51a = (in13 *  3889 + 2048) >> 12;
    int t52a = (in11 *  3948 + 2048) >> 12;
    int t53a = (in21 *  3564 + 2048) >> 12;
    int t54a = (in27 *  3229 + 2048) >> 12;
    int t55a = (in5  *  4065 + 2048) >> 12;
    int t56a = (in7  *  4036 + 2048) >> 12;
    int t57a = (in25 *  3349 + 2048) >> 12;
    int t58a = (in23 *  3461 + 2048) >> 12;
    int t59a = (in9  *  3996 + 2048) >> 12;
    int t60a = (in15 *  3822 + 2048) >> 12;
    int t61a = (in17 *  3745 + 2048) >> 12;
    int t62a = (in31 *  2967 + 2048) >> 12;
    int t63a = (in1  *  4095 + 2048) >> 12;

    int t32 = CLIP(t32a + t33a);
    int t33 = CLIP(t32a - t33a);
    int t34 = CLIP(t35a - t34a);
    int t35 = CLIP(t35a + t34a);
    int t36 = CLIP(t36a + t37a);
    int t37 = CLIP(t36a - t37a);
    int t38 = CLIP(t39a - t38a);
    int t39 = CLIP(t39a + t38a);
    int t40 = CLIP(t40a + t41a);
    int t41 = CLIP(t40a - t41a);
    int t42 = CLIP(t43a - t42a);
    int t43 = CLIP(t43a + t42a);
    int t44 = CLIP(t44a + t45a);
    int t45 = CLIP(t44a - t45a);
    int t46 = CLIP(t47a - t46a);
    int t47 = CLIP(t47a + t46a);
    int t48 = CLIP(t48a + t49a);
    int t49 = CLIP(t48a - t49a);
    int t50 = CLIP(t51a - t50a);
    int t51 = CLIP(t51a + t50a);
    int t52 = CLIP(t52a + t53a);
    int t53 = CLIP(t52a - t53a);
    int t54 = CLIP(t55a - t54a);
    int t55 = CLIP(t55a + t54a);
    int t56 = CLIP(t56a + t57a);
    int t57 = CLIP(t56a - t57a);
    int t58 = CLIP(t59a - t58a);
    int t59 = CLIP(t59a + t58a);
    int t60 = CLIP(t60a + t61a);
    int t61 = CLIP(t60a - t61a);
    int t62 = CLIP(t63a - t62a);
    int t63 = CLIP(t63a + t62a);

    t33a = ((t33 * (4096 - 4076) + t62 *   401         + 2048) >> 12) - t33;
    t34a = ((t34 *  -401         + t61 * (4096 - 4076) + 2048) >> 12) - t61;
    t37a =  (t37 * -1299         + t58 *  1583         + 1024) >> 11;
    t38a =  (t38 * -1583         + t57 * -1299         + 1024) >> 11;
    t41a = ((t41 * (4096 - 3612) + t54 *  1931         + 2048) >> 12) - t41;
    t42a = ((t42 * -1931         + t53 * (4096 - 3612) + 2048) >> 12) - t53;
    t45a = ((t45 * -1189         + t50 * (3920 - 4096) + 2048) >> 12) + t50;
    t46a = ((t46 * (4096 - 3920) + t49 * -1189         + 2048) >> 12) - t46;
    t49a = ((t46 * -1189         + t49 * (3920 - 4096) + 2048) >> 12) + t49;
    t50a = ((t45 * (3920 - 4096) + t50 *  1189         + 2048) >> 12) + t45;
    t53a = ((t42 * (4096 - 3612) + t53 *  1931         + 2048) >> 12) - t42;
    t54a = ((t41 *  1931         + t54 * (3612 - 4096) + 2048) >> 12) + t54;
    t57a =  (t38 * -1299         + t57 *  1583         + 1024) >> 11;
    t58a =  (t37 *  1583         + t58 *  1299         + 1024) >> 11;
    t61a = ((t34 * (4096 - 4076) + t61 *   401         + 2048) >> 12) - t34;
    t62a = ((t33 *   401         + t62 * (4076 - 4096) + 2048) >> 12) + t62;

    t32a = CLIP(t32  + t35);
    t33  = CLIP(t33a + t34a);
    t34  = CLIP(t33a - t34a);
    t35a = CLIP(t32  - t35);
    t36a = CLIP(t39  - t36);
    t37  = CLIP(t38a - t37a);
    t38  = CLIP(t38a + t37a);
    t39a = CLIP(t39  + t36);
    t40a = CLIP(t40  + t43);
    t41  = CLIP(t41a + t42a);
    t42  = CLIP(t41a - t42a);
    t43a = CLIP(t40  - t43);
    t44a = CLIP(t47  - t44);
    t45  = CLIP(t46a - t45a);
    t46  = CLIP(t46a + t45a);
    t47a = CLIP(t47  + t44);
    t48a = CLIP(t48  + t51);
    t49  = CLIP(t49a + t50a);
    t50  = CLIP(t49a - t50a);
    t51a = CLIP(t48  - t51);
    t52a = CLIP(t55  - t52);
    t53  = CLIP(t54a - t53a);
    t54  = CLIP(t54a + t53a);
    t55a = CLIP(t55  + t52);
    t56a = CLIP(t56  + t59);
    t57  = CLIP(t57a + t58a);
    t58  = CLIP(t57a - t58a);
    t59a = CLIP(t56  - t59);
    t60a = CLIP(t63  - t60);
    t61  = CLIP(t62a - t61a);
    t62  = CLIP(t62a + t61a);
    t63a = CLIP(t63  + t60);

    t34a = ((t34  * (4096 - 4017) + t61  *   799         + 2048) >> 12) - t34;
    t35  = ((t35a * (4096 - 4017) + t60a *   799         + 2048) >> 12) - t35a;
    t36  = ((t36a *  -799         + t59a * (4096 - 4017) + 2048) >> 12) - t59a;
    t37a = ((t37  *  -799         + t58  * (4096 - 4017) + 2048) >> 12) - t58;
    t42a =  (t42  * -1138         + t53  *  1703         + 1024) >> 11;
    t43  =  (t43a * -1138         + t52a *  1703         + 1024) >> 11;
    t44  =  (t44a * -1703         + t51a * -1138         + 1024) >> 11;
    t45a =  (t45  * -1703         + t50  * -1138         + 1024) >> 11;
    t50a =  (t45  * -1138         + t50  *  1703         + 1024) >> 11;
    t51  =  (t44a * -1138         + t51a *  1703         + 1024) >> 11;
    t52  =  (t43a *  1703         + t52a *  1138         + 1024) >> 11;
    t53a =  (t42  *  1703         + t53  *  1138         + 1024) >> 11;
    t58a = ((t37  * (4096 - 4017) + t58  *   799         + 2048) >> 12) - t37;
    t59  = ((t36a * (4096 - 4017) + t59a *   799         + 2048) >> 12) - t36a;
    t60  = ((t35a *   799         + t60a * (4017 - 4096) + 2048) >> 12) + t60a;
    t61a = ((t34  *   799         + t61  * (4017 - 4096) + 2048) >> 12) + t61;

    t32  = CLIP(t32a + t39a);
    t33a = CLIP(t33  + t38);
    t34  = CLIP(t34a + t37a);
    t35a = CLIP(t35  + t36);
    t36a = CLIP(t35  - t36);
    t37  = CLIP(t34a - t37a);
    t38a = CLIP(t33  - t38);
    t39  = CLIP(t32a - t39a);
    t40  = CLIP(t47a - t40a);
    t41a = CLIP(t46  - t41);
    t42  = CLIP(t45a - t42a);
    t43a = CLIP(t44  - t43);
    t44a = CLIP(t44  + t43);
    t45  = CLIP(t45a + t42a);
    t46a = CLIP(t46  + t41);
    t47  = CLIP(t47a + t40a);
    t48  = CLIP(t48a + t55a);
    t49a = CLIP(t49  + t54);
    t50  = CLIP(t50a + t53a);
    t51a = CLIP(t51  + t52);
    t52a = CLIP(t51  - t52);
    t53  = CLIP(t50a - t53a);
    t54a = CLIP(t49  - t54);
    t55  = CLIP(t48a - t55a);
    t56  = CLIP(t63a - t56a);
    t57a = CLIP(t62  - t57);
    t58  = CLIP(t61a - t58a);
    t59a = CLIP(t60  - t59);
    t60a = CLIP(t60  + t59);
    t61  = CLIP(t61a + t58a);
    t62a = CLIP(t62  + t57);
    t63  = CLIP(t63a + t56a);

    t36  = ((t36a * (4096 - 3784) + t59a *  1567         + 2048) >> 12) - t36a;
    t37a = ((t37  * (4096 - 3784) + t58  *  1567         + 2048) >> 12) - t37;
    t38  = ((t38a * (4096 - 3784) + t57a *  1567         + 2048) >> 12) - t38a;
    t39a = ((t39  * (4096 - 3784) + t56  *  1567         + 2048) >> 12) - t39;
    t40a = ((t40  * -1567         + t55  * (4096 - 3784) + 2048) >> 12) - t55;
    t41  = ((t41a * -1567         + t54a * (4096 - 3784) + 2048) >> 12) - t54a;
    t42a = ((t42  * -1567         + t53  * (4096 - 3784) + 2048) >> 12) - t53;
    t43  = ((t43a * -1567         + t52a * (4096 - 3784) + 2048) >> 12) - t52a;
    t52  = ((t43a * (4096 - 3784) + t52a *  1567         + 2048) >> 12) - t43a;
    t53a = ((t42  * (4096 - 3784) + t53  *  1567         + 2048) >> 12) - t42;
    t54  = ((t41a * (4096 - 3784) + t54a *  1567         + 2048) >> 12) - t41a;
    t55a = ((t40  * (4096 - 3784) + t55  *  1567         + 2048) >> 12) - t40;
    t56a = ((t39  *  1567         + t56  * (3784 - 4096) + 2048) >> 12) + t56;
    t57  = ((t38a *  1567         + t57a * (3784 - 4096) + 2048) >> 12) + t57a;
    t58a = ((t37  *  1567         + t58  * (3784 - 4096) + 2048) >> 12) + t58;
    t59  = ((t36a *  1567         + t59a * (3784 - 4096) + 2048) >> 12) + t59a;

    t32a = CLIP(t32  + t47);
    t33  = CLIP(t33a + t46a);
    t34a = CLIP(t34  + t45);
    t35  = CLIP(t35a + t44a);
    t36a = CLIP(t36  + t43);
    t37  = CLIP(t37a + t42a);
    t38a = CLIP(t38  + t41);
    t39  = CLIP(t39a + t40a);
    t40  = CLIP(t39a - t40a);
    t41a = CLIP(t38  - t41);
    t42  = CLIP(t37a - t42a);
    t43a = CLIP(t36  - t43);
    t44  = CLIP(t35a - t44a);
    t45a = CLIP(t34  - t45);
    t46  = CLIP(t33a - t46a);
    t47a = CLIP(t32  - t47);
    t48a = CLIP(t63  - t48);
    t49  = CLIP(t62a - t49a);
    t50a = CLIP(t61  - t50);
    t51  = CLIP(t60a - t51a);
    t52a = CLIP(t59  - t52);
    t53  = CLIP(t58a - t53a);
    t54a = CLIP(t57  - t54);
    t55  = CLIP(t56a - t55a);
    t56  = CLIP(t56a + t55a);
    t57a = CLIP(t57  + t54);
    t58  = CLIP(t58a + t53a);
    t59a = CLIP(t59  + t52);
    t60  = CLIP(t60a + t51a);
    t61a = CLIP(t61  + t50);
    t62  = CLIP(t62a + t49a);
    t63a = CLIP(t63  + t48);

    t40a = ((t55  - t40 ) * 181 + 128) >> 8;
    t41  = ((t54a - t41a) * 181 + 128) >> 8;
    t42a = ((t53  - t42 ) * 181 + 128) >> 8;
    t43  = ((t52a - t43a) * 181 + 128) >> 8;
    t44a = ((t51  - t44 ) * 181 + 128) >> 8;
    t45  = ((t50a - t45a) * 181 + 128) >> 8;
    t46a = ((t49  - t46 ) * 181 + 128) >> 8;
    t47  = ((t48a - t47a) * 181 + 128) >> 8;
    t48  = ((t47a + t48a) * 181 + 128) >> 8;
    t49a = ((t46  + t49 ) * 181 + 128) >> 8;
    t50  = ((t45a + t50a) * 181 + 128) >> 8;
    t51a = ((t44  + t51 ) * 181 + 128) >> 8;
    t52  = ((t43a + t52a) * 181 + 128) >> 8;
    t53a = ((t42  + t53 ) * 181 + 128) >> 8;
    t54  = ((t41a + t54a) * 181 + 128) >> 8;
    t55a = ((t40  + t55 ) * 181 + 128) >> 8;

    const int t0  = c[0];
    const int t1  = c[2];
    const int t2  = c[4];
    const int t3  = c[6];
    const int t4  = c[8];
    const int t5  = c[10];
    const int t6  = c[12];
    const int t7  = c[14];
    const int t8  = c[16];
    const int t9  = c[18];
    const int t10 = c[20];
    const int t11 = c[22];
    const int t12 = c[24];
    const int t13 = c[26];
    const int t14 = c[28];
    const int t15 = c[30];
    const int t16 = c[32];
    const int t17 = c[34];
    const int t18 = c[36];
    const int t19 = c[38];
    const int t20 = c[40];
    const int t21 = c[42];
    const int t22 = c[44];
    const int t23 = c[46];
    const int t24 = c[48];
    const int t25 = c[50];
    const int t26 = c[52];
    const int t27 = c[54];
    const int t28 = c[56];
    const int t29 = c[58];
    const int t30 = c[60];
    const int t31 = c[62];

    c[0] = CLIP(t0  + t63a);
    c[1] = CLIP(t1  + t62);
    c[2] = CLIP(t2  + t61a);
    c[3] = CLIP(t3  + t60);
    c[4] = CLIP(t4  + t59a);
    c[5] = CLIP(t5  + t58);
    c[6] = CLIP(t6  + t57a);
    c[7] = CLIP(t7  + t56);
    c[8] = CLIP(t8  + t55a);
    c[9] = CLIP(t9  + t54);
    c[10] = CLIP(t10 + t53a);
    c[11] = CLIP(t11 + t52);
    c[12] = CLIP(t12 + t51a);
    c[13] = CLIP(t13 + t50);
    c[14] = CLIP(t14 + t49a);
    c[15] = CLIP(t15 + t48);
    c[16] = CLIP(t16 + t47);
    c[17] = CLIP(t17 + t46a);
    c[18] = CLIP(t18 + t45);
    c[19] = CLIP(t19 + t44a);
    c[20] = CLIP(t20 + t43);
    c[21] = CLIP(t21 + t42a);
    c[22] = CLIP(t22 + t41);
    c[23] = CLIP(t23 + t40a);
    c[24] = CLIP(t24 + t39);
    c[25] = CLIP(t25 + t38a);
    c[26] = CLIP(t26 + t37);
    c[27] = CLIP(t27 + t36a);
    c[28] = CLIP(t28 + t35);
    c[29] = CLIP(t29 + t34a);
    c[30] = CLIP(t30 + t33);
    c[31] = CLIP(t31 + t32a);
    c[32] = CLIP(t31 - t32a);
    c[33] = CLIP(t30 - t33);
    c[34] = CLIP(t29 - t34a);
    c[35] = CLIP(t28 - t35);
    c[36] = CLIP(t27 - t36a);
    c[37] = CLIP(t26 - t37);
    c[38] = CLIP(t25 - t38a);
    c[39] = CLIP(t24 - t39);
    c[40] = CLIP(t23 - t40a);
    c[41] = CLIP(t22 - t41);
    c[42] = CLIP(t21 - t42a);
    c[43] = CLIP(t20 - t43);
    c[44] = CLIP(t19 - t44a);
    c[45] = CLIP(t18 - t45);
    c[46] = CLIP(t17 - t46a);
    c[47] = CLIP(t16 - t47);
    c[48] = CLIP(t15 - t48);
    c[49] = CLIP(t14 - t49a);
    c[50] = CLIP(t13 - t50);
    c[51] = CLIP(t12 - t51a);
    c[52] = CLIP(t11 - t52);
    c[53] = CLIP(t10 - t53a);
    c[54] = CLIP(t9  - t54);
    c[55] = CLIP(t8  - t55a);
    c[56] = CLIP(t7  - t56);
    c[57] = CLIP(t6  - t57a);
    c[58] = CLIP(t5  - t58);
    c[59] = CLIP(t4  - t59a);
    c[60] = CLIP(t3  - t60);
    c[61] = CLIP(t2  - t61a);
    c[62] = CLIP(t1  - t62);
    c[63] = CLIP(t0  - t63a);
}

static void
inv_adst4_1d_internal(const Lane& in, const int min, const int max, const Lane& out)
{
    const int in0 = in[0], in1 = in[1];
    const int in2 = in[2], in3 = in[3];

    out[0] = (( 1321         * in0 + (3803 - 4096) * in2 +
                       (2482 - 4096) * in3 + (3344 - 4096) * in1 + 2048) >> 12) +
                     in2 + in3 + in1;
    out[1] = (((2482 - 4096) * in0 -  1321         * in2 -
                       (3803 - 4096) * in3 + (3344 - 4096) * in1 + 2048) >> 12) +
                     in0 - in3 + in1;
    out[2] = (209 * (in0 - in2 + in3) + 128) >> 8;
    out[3] = (((3803 - 4096) * in0 + (2482 - 4096) * in2 -
                        1321         * in3 - (3344 - 4096) * in1 + 2048) >> 12) +
                     in0 + in2 - in1;
}

static void
inv_adst8_1d_internal(const Lane& in, const int min, const int max, const Lane& out)
{
    const int in0 = in[0], in1 = in[1];
    const int in2 = in[2], in3 = in[3];
    const int in4 = in[4], in5 = in[5];
    const int in6 = in[6], in7 = in[7];

    const int t0a = (((4076 - 4096) * in7 +   401         * in0 + 2048) >> 12) + in7;
    const int t1a = ((  401         * in7 - (4076 - 4096) * in0 + 2048) >> 12) - in0;
    const int t2a = (((3612 - 4096) * in5 +  1931         * in2 + 2048) >> 12) + in5;
    const int t3a = (( 1931         * in5 - (3612 - 4096) * in2 + 2048) >> 12) - in2;
          int t4a =  ( 1299         * in3 +  1583         * in4 + 1024) >> 11;
          int t5a =  ( 1583         * in3 -  1299         * in4 + 1024) >> 11;
          int t6a = (( 1189         * in1 + (3920 - 4096) * in6 + 2048) >> 12) + in6;
          int t7a = (((3920 - 4096) * in1 -  1189         * in6 + 2048) >> 12) + in1;

    const int t0 = CLIP(t0a + t4a);
    const int t1 = CLIP(t1a + t5a);
          int t2 = CLIP(t2a + t6a);
          int t3 = CLIP(t3a + t7a);
    const int t4 = CLIP(t0a - t4a);
    const int t5 = CLIP(t1a - t5a);
          int t6 = CLIP(t2a - t6a);
          int t7 = CLIP(t3a - t7a);

    t4a = (((3784 - 4096) * t4 +  1567         * t5 + 2048) >> 12) + t4;
    t5a = (( 1567         * t4 - (3784 - 4096) * t5 + 2048) >> 12) - t5;
    t6a = (((3784 - 4096) * t7 -  1567         * t6 + 2048) >> 12) + t7;
    t7a = (( 1567         * t7 + (3784 - 4096) * t6 + 2048) >> 12) + t6;

    out[0] =  CLIP(t0  + t2 );
    out[7] = -CLIP(t1  + t3 );
    t2             =  CLIP(t0  - t2 );
    t3             =  CLIP(t1  - t3 );
    out[1] = -CLIP(t4a + t6a);
    out[6] =  CLIP(t5a + t7a);
    t6             =  CLIP(t4a - t6a);
    t7             =  CLIP(t5a - t7a);

    out[3] = -(((t2 + t3) * 181 + 128) >> 8);
    out[4] =   ((t2 - t3) * 181 + 128) >> 8;
    out[2] =   ((t6 + t7) * 181 + 128) >> 8;
    out[5] = -(((t6 - t7) * 181 + 128) >> 8);
}

static void
inv_adst16_1d_internal(const Lane& in, const int min, const int max, const Lane& out)
{
    const int in0  = in[0], in1  = in[1];
    const int in2  = in[2], in3  = in[3];
    const int in4  = in[4], in5  = in[5];
    const int in6  = in[6], in7  = in[7];
    const int in8  = in[8], in9  = in[9];
    const int in10 = in[10], in11 = in[11];
    const int in12 = in[12], in13 = in[13];
    const int in14 = in[14], in15 = in[15];

    int t0  = ((in15 * (4091 - 4096) + in0  *   201         + 2048) >> 12) + in15;
    int t1  = ((in15 *   201         - in0  * (4091 - 4096) + 2048) >> 12) - in0;
    int t2  = ((in13 * (3973 - 4096) + in2  *   995         + 2048) >> 12) + in13;
    int t3  = ((in13 *   995         - in2  * (3973 - 4096) + 2048) >> 12) - in2;
    int t4  = ((in11 * (3703 - 4096) + in4  *  1751         + 2048) >> 12) + in11;
    int t5  = ((in11 *  1751         - in4  * (3703 - 4096) + 2048) >> 12) - in4;
    int t6  =  (in9  *  1645         + in6  *  1220         + 1024) >> 11;
    int t7  =  (in9  *  1220         - in6  *  1645         + 1024) >> 11;
    int t8  = ((in7  *  2751         + in8  * (3035 - 4096) + 2048) >> 12) + in8;
    int t9  = ((in7  * (3035 - 4096) - in8  *  2751         + 2048) >> 12) + in7;
    int t10 = ((in5  *  2106         + in10 * (3513 - 4096) + 2048) >> 12) + in10;
    int t11 = ((in5  * (3513 - 4096) - in10 *  2106         + 2048) >> 12) + in5;
    int t12 = ((in3  *  1380         + in12 * (3857 - 4096) + 2048) >> 12) + in12;
    int t13 = ((in3  * (3857 - 4096) - in12 *  1380         + 2048) >> 12) + in3;
    int t14 = ((in1  *   601         + in14 * (4052 - 4096) + 2048) >> 12) + in14;
    int t15 = ((in1  * (4052 - 4096) - in14 *   601         + 2048) >> 12) + in1;

    int t0a  = CLIP(t0 + t8 );
    int t1a  = CLIP(t1 + t9 );
    int t2a  = CLIP(t2 + t10);
    int t3a  = CLIP(t3 + t11);
    int t4a  = CLIP(t4 + t12);
    int t5a  = CLIP(t5 + t13);
    int t6a  = CLIP(t6 + t14);
    int t7a  = CLIP(t7 + t15);
    int t8a  = CLIP(t0 - t8 );
    int t9a  = CLIP(t1 - t9 );
    int t10a = CLIP(t2 - t10);
    int t11a = CLIP(t3 - t11);
    int t12a = CLIP(t4 - t12);
    int t13a = CLIP(t5 - t13);
    int t14a = CLIP(t6 - t14);
    int t15a = CLIP(t7 - t15);

    t8   = ((t8a  * (4017 - 4096) + t9a  *   799         + 2048) >> 12) + t8a;
    t9   = ((t8a  *   799         - t9a  * (4017 - 4096) + 2048) >> 12) - t9a;
    t10  = ((t10a *  2276         + t11a * (3406 - 4096) + 2048) >> 12) + t11a;
    t11  = ((t10a * (3406 - 4096) - t11a *  2276         + 2048) >> 12) + t10a;
    t12  = ((t13a * (4017 - 4096) - t12a *   799         + 2048) >> 12) + t13a;
    t13  = ((t13a *   799         + t12a * (4017 - 4096) + 2048) >> 12) + t12a;
    t14  = ((t15a *  2276         - t14a * (3406 - 4096) + 2048) >> 12) - t14a;
    t15  = ((t15a * (3406 - 4096) + t14a *  2276         + 2048) >> 12) + t15a;

    t0   = CLIP(t0a + t4a);
    t1   = CLIP(t1a + t5a);
    t2   = CLIP(t2a + t6a);
    t3   = CLIP(t3a + t7a);
    t4   = CLIP(t0a - t4a);
    t5   = CLIP(t1a - t5a);
    t6   = CLIP(t2a - t6a);
    t7   = CLIP(t3a - t7a);
    t8a  = CLIP(t8  + t12);
    t9a  = CLIP(t9  + t13);
    t10a = CLIP(t10 + t14);
    t11a = CLIP(t11 + t15);
    t12a = CLIP(t8  - t12);
    t13a = CLIP(t9  - t13);
    t14a = CLIP(t10 - t14);
    t15a = CLIP(t11 - t15);

    t4a  = ((t4   * (3784 - 4096) + t5   *  1567         + 2048) >> 12) + t4;
    t5a  = ((t4   *  1567         - t5   * (3784 - 4096) + 2048) >> 12) - t5;
    t6a  = ((t7   * (3784 - 4096) - t6   *  1567         + 2048) >> 12) + t7;
    t7a  = ((t7   *  1567         + t6   * (3784 - 4096) + 2048) >> 12) + t6;
    t12  = ((t12a * (3784 - 4096) + t13a *  1567         + 2048) >> 12) + t12a;
    t13  = ((t12a *  1567         - t13a * (3784 - 4096) + 2048) >> 12) - t13a;
    t14  = ((t15a * (3784 - 4096) - t14a *  1567         + 2048) >> 12) + t15a;
    t15  = ((t15a *  1567         + t14a * (3784 - 4096) + 2048) >> 12) + t14a;

    out[0] =  CLIP(t0  + t2  );
    out[15] = -CLIP(t1  + t3  );
    t2a             =  CLIP(t0  - t2  );
    t3a             =  CLIP(t1  - t3  );
    out[3] = -CLIP(t4a + t6a );
    out[12] =  CLIP(t5a + t7a );
    t6              =  CLIP(t4a - t6a );
    t7              =  CLIP(t5a - t7a );
    out[1] = -CLIP(t8a + t10a);
    out[14] =  CLIP(t9a + t11a);
    t10             =  CLIP(t8a - t10a);
    t11             =  CLIP(t9a - t11a);
    out[2] =  CLIP(t12 + t14 );
    out[13] = -CLIP(t13 + t15 );
    t14a            =  CLIP(t12 - t14 );
    t15a            =  CLIP(t13 - t15 );

    out[7] = -(((t2a  + t3a)  * 181 + 128) >> 8);
    out[8] =   ((t2a  - t3a)  * 181 + 128) >> 8;
    out[4] =   ((t6   + t7)   * 181 + 128) >> 8;
    out[11] = -(((t6   - t7)   * 181 + 128) >> 8);
    out[6] =   ((t10  + t11)  * 181 + 128) >> 8;
    out[9] = -(((t10  - t11)  * 181 + 128) >> 8);
    out[5] = -(((t14a + t15a) * 181 + 128) >> 8);
    out[10] =   ((t14a - t15a) * 181 + 128) >> 8;
}


static void inv_identity4_1d(const Lane& c, const int min, const int max)
{
    for (int i = 0; i < 4; i++) {
        const int in = c[i];
        c[i] = in + ((in * 1697 + 2048) >> 12);
    }
}

static void inv_identity8_1d(const Lane& c, const int min, const int max)
{
    for (int i = 0; i < 8; i++)
        c[i] *= 2;
}

static void inv_identity16_1d(const Lane& c, const int min, const int max)
{
    for (int i = 0; i < 16; i++) {
        const int in = c[i];
        c[i] = 2 * in + ((in * 1697 + 1024) >> 11);
    }
}

static void inv_identity32_1d(const Lane& c, const int min, const int max)
{
    for (int i = 0; i < 32; i++)
        c[i] *= 4;
}
#undef CLIP

// dav1d_inv_wht4_1d_c
void inv_wht4_1d(const Lane& c) {
  const int in0 = c[0], in1 = c[1];
  const int in2 = c[2], in3 = c[3];

  const int t0 = in0 + in1;
  const int t2 = in2 - in3;
  const int t4 = (t0 - t2) >> 1;
  const int t3 = t4 - in3;
  const int t1 = t4 - in1;

  c[0] = t0 - t3;
  c[1] = t3;
  c[2] = t1;
  c[3] = t2 + t1;
}

enum Tx1dType { DCT, ADST, IDENTITY, FLIPADST };

// The (first = row, second = column) 1-D transform for each TxfmType, as dav1d's itxfm_add[tx][txtp] table dispatches it. Note that
// dav1d's init assigns the functions named after the other way round (itxfm_add[..][ADST_DCT] is its inv_txfm_add_dct_adst, i.e.
// dav1d_tx1d_types[DCT_ADST]); this table is that composition, so `txtp` here is the same value the caller would index dav1d's table with.
// ADST_DCT = ADST vertical (column pass), DCT horizontal (row pass), as the enumerator comments in Av1Types.hpp say.
constexpr uint8_t k_tx1d_types[N_TX_TYPES][2] = {
    /* DCT_DCT           */ {DCT, DCT},
    /* ADST_DCT          */ {DCT, ADST},
    /* DCT_ADST          */ {ADST, DCT},
    /* ADST_ADST         */ {ADST, ADST},
    /* FLIPADST_DCT      */ {DCT, FLIPADST},
    /* DCT_FLIPADST      */ {FLIPADST, DCT},
    /* FLIPADST_FLIPADST */ {FLIPADST, FLIPADST},
    /* ADST_FLIPADST     */ {FLIPADST, ADST},
    /* FLIPADST_ADST     */ {ADST, FLIPADST},
    /* IDTX              */ {IDENTITY, IDENTITY},
    /* V_DCT             */ {IDENTITY, DCT},
    /* H_DCT             */ {DCT, IDENTITY},
    /* V_ADST            */ {IDENTITY, ADST},
    /* H_ADST            */ {ADST, IDENTITY},
    /* V_FLIPADST        */ {IDENTITY, FLIPADST},
    /* H_FLIPADST        */ {FLIPADST, IDENTITY},
};

// log2(width / 4) and log2(height / 4) of each TxfmSize / RectTxfmSize (dav1d_txfm_dimensions .lw / .lh).
constexpr uint8_t k_tx_lw[N_RECT_TX_SIZES] = {0, 1, 2, 3, 4, 0, 1, 1, 2, 2, 3, 3, 4, 0, 2, 1, 3, 2, 4};
constexpr uint8_t k_tx_lh[N_RECT_TX_SIZES] = {0, 1, 2, 3, 4, 1, 0, 2, 1, 3, 2, 4, 3, 2, 0, 3, 1, 4, 2};

// The per-size shifts of dav1d's inv_txfm_fn* instantiations: the rounding shift between the row and column transforms.
constexpr uint8_t k_tx_shift[N_RECT_TX_SIZES] = {0, 1, 2, 2, 2, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2};

// One 1-D transform of length 4 << lw.
void run_1d(int lw, int type, const Lane& c, int min, int max) {
  switch (lw) {
    case 0:
      switch (type) {
        case DCT: inv_dct4_1d(c, min, max); break;
        case ADST: inv_adst4_1d_internal(c, min, max, c); break;
        case FLIPADST: inv_adst4_1d_internal(c, min, max, c.reversed(4)); break;
        default: inv_identity4_1d(c, min, max); break;
      }
      break;
    case 1:
      switch (type) {
        case DCT: inv_dct8_1d(c, min, max); break;
        case ADST: inv_adst8_1d_internal(c, min, max, c); break;
        case FLIPADST: inv_adst8_1d_internal(c, min, max, c.reversed(8)); break;
        default: inv_identity8_1d(c, min, max); break;
      }
      break;
    case 2:
      switch (type) {
        case DCT: inv_dct16_1d(c, min, max); break;
        case ADST: inv_adst16_1d_internal(c, min, max, c); break;
        case FLIPADST: inv_adst16_1d_internal(c, min, max, c.reversed(16)); break;
        default: inv_identity16_1d(c, min, max); break;
      }
      break;
    case 3:
      if (type == DCT) inv_dct32_1d(c, min, max);
      else inv_identity32_1d(c, min, max);
      break;
    default:
      inv_dct64_1d(c, min, max);
      break;
  }
}

// dav1d's last_nonzero_col_from_eob tables (scan.c init_tbl): for each scan position the highest row index touched so far.
template <uint32_t N>
struct ColTable {
  uint8_t v[N];
};

template <uint32_t N>
constexpr ColTable<N> make_col_table(const uint16_t (&scan)[N], uint32_t w, uint32_t h) {
  ColTable<N> t{};
  uint32_t max_col = 0;
  for (uint32_t n = 0; n < N && n < w * h; n++) {
    const uint32_t rcx = scan[n] & (h - 1);
    if (rcx > max_col) max_col = rcx;
    t.v[n] = static_cast<uint8_t>(max_col);
  }
  return t;
}

constexpr ColTable<16> k_col_4x4 = make_col_table(k_scan_4x4, 4, 4);
constexpr ColTable<64> k_col_8x8 = make_col_table(k_scan_8x8, 8, 8);
constexpr ColTable<256> k_col_16x16 = make_col_table(k_scan_16x16, 16, 16);
constexpr ColTable<1024> k_col_32x32 = make_col_table(k_scan_32x32, 32, 32);
constexpr ColTable<32> k_col_4x8 = make_col_table(k_scan_4x8, 4, 8);
constexpr ColTable<32> k_col_8x4 = make_col_table(k_scan_8x4, 8, 4);
constexpr ColTable<128> k_col_8x16 = make_col_table(k_scan_8x16, 8, 16);
constexpr ColTable<128> k_col_16x8 = make_col_table(k_scan_16x8, 16, 8);
constexpr ColTable<512> k_col_16x32 = make_col_table(k_scan_16x32, 16, 32);
constexpr ColTable<512> k_col_32x16 = make_col_table(k_scan_32x16, 32, 16);
constexpr ColTable<64> k_col_4x16 = make_col_table(k_scan_4x16, 4, 16);
constexpr ColTable<64> k_col_16x4 = make_col_table(k_scan_16x4, 16, 4);
constexpr ColTable<256> k_col_8x32 = make_col_table(k_scan_8x32, 8, 32);
constexpr ColTable<256> k_col_32x8 = make_col_table(k_scan_32x8, 32, 8);

template <uint32_t N>
ArrayView<const uint8_t> view_of(const ColTable<N>& t) {
  return ArrayView<const uint8_t>(t.v, N);
}

// dav1d_last_nonzero_col_from_eob[tx]
ArrayView<const uint8_t> last_nonzero_col_table(int tx) {
  switch (tx) {
    case TX_4X4: return view_of(k_col_4x4);
    case TX_8X8: return view_of(k_col_8x8);
    case TX_16X16: return view_of(k_col_16x16);
    case TX_32X32:
    case TX_64X64:
    case RTX_32X64:
    case RTX_64X32: return view_of(k_col_32x32);
    case RTX_4X8: return view_of(k_col_4x8);
    case RTX_8X4: return view_of(k_col_8x4);
    case RTX_8X16: return view_of(k_col_8x16);
    case RTX_16X8: return view_of(k_col_16x8);
    case RTX_16X32:
    case RTX_16X64: return view_of(k_col_16x32);
    case RTX_32X16:
    case RTX_64X16: return view_of(k_col_32x16);
    case RTX_4X16: return view_of(k_col_4x16);
    case RTX_16X4: return view_of(k_col_16x4);
    case RTX_8X32: return view_of(k_col_8x32);
    default: return view_of(k_col_32x8);  // RTX_32X8
  }
}

inline Pixel clip_pixel(int v, int bitdepth_max) { return static_cast<Pixel>(v < 0 ? 0 : v > bitdepth_max ? bitdepth_max : v); }

// dav1d's inv_txfm_add_c.
void inv_txfm_add_c(PixelView dst, ArrayView<int32_t> coeff, int eob, int tx, int shift, int txtp, int bitdepth_max) {
  const int lw = k_tx_lw[tx], lh = k_tx_lh[tx];
  const int w = 4 << lw, h = 4 << lh;
  const int has_dconly = txtp == DCT_DCT;

  const int is_rect2 = w * 2 == h || h * 2 == w;
  const int rnd = (1 << shift) >> 1;

  if (eob < has_dconly) {
    int dc = coeff[0];
    coeff[0] = 0;
    if (is_rect2) dc = (dc * 181 + 128) >> 8;
    dc = (dc * 181 + 128) >> 8;
    dc = (dc + rnd) >> shift;
    dc = (dc * 181 + 128 + 2048) >> 12;
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        Pixel& p = dst.at(x, y);
        p = clip_pixel(p + dc, bitdepth_max);
      }
    return;
  }

  const int first_type = k_tx1d_types[txtp][0];
  const int second_type = k_tx1d_types[txtp][1];
  const int sh = h < 32 ? h : 32, sw = w < 32 ? w : 32;
  // 8 bit streams clip to int16 in both directions (dav1d's 8bpc build); deeper ones to bitdepth + 8 and bitdepth + 6 (at least 16) bits.
  const int row_clip_min = bitdepth_max == 255 ? -32768 : static_cast<int>(static_cast<unsigned>(~bitdepth_max) << 7);
  const int col_clip_min = bitdepth_max == 255 ? -32768 : static_cast<int>(static_cast<unsigned>(~bitdepth_max) << 5);
  const int row_clip_max = ~row_clip_min;
  const int col_clip_max = ~col_clip_min;

  int32_t tmp_storage[64 * 64];
  const ArrayView<int32_t> tmp(tmp_storage, static_cast<uint64_t>(w) * h);
  int last_nonzero_col;  // in first 1d itx
  if (second_type == IDENTITY && first_type != IDENTITY) {
    last_nonzero_col = eob < sh - 1 ? eob : sh - 1;
  } else if (first_type == IDENTITY && second_type != IDENTITY) {
    last_nonzero_col = eob >> (lw + 2);
  } else {
    const ArrayView<const uint8_t> table = last_nonzero_col_table(tx);
    last_nonzero_col = table[static_cast<uint64_t>(eob) < table.size() ? static_cast<uint64_t>(eob) : table.size() - 1];
  }
  if (last_nonzero_col > sh - 1) last_nonzero_col = sh - 1;  // only an eob that dav1d's parser cannot produce gets here
  int row = 0;
  for (int y = 0; y <= last_nonzero_col; y++, row += w) {
    const Lane c{tmp, row, 1};
    if (is_rect2)
      for (int x = 0; x < sw; x++) c[x] = (coeff[static_cast<uint64_t>(y) + static_cast<uint64_t>(x) * sh] * 181 + 128) >> 8;
    else
      for (int x = 0; x < sw; x++) c[x] = coeff[static_cast<uint64_t>(y) + static_cast<uint64_t>(x) * sh];
    // dav1d leaves the rest of a 64-wide row undefined: the 64-point transform only reads the first 32 inputs.
    for (int x = sw; x < w; x++) c[x] = 0;
    run_1d(lw, first_type, c, row_clip_min, row_clip_max);
  }
  for (int i = row; i < w * sh; i++) tmp[static_cast<uint64_t>(i)] = 0;

  coeff.first(static_cast<uint64_t>(sw) * sh).fill(0);
  for (int i = 0; i < w * sh; i++) {
    int32_t& v = tmp[static_cast<uint64_t>(i)];
    v = iclip((v + rnd) >> shift, col_clip_min, col_clip_max);
  }

  for (int x = 0; x < w; x++) run_1d(lh, second_type, Lane{tmp, x, w}, col_clip_min, col_clip_max);

  int i = 0;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++, i++) {
      Pixel& p = dst.at(x, y);
      p = clip_pixel(p + ((tmp[static_cast<uint64_t>(i)] + 8) >> 4), bitdepth_max);
    }
}

// dav1d's inv_txfm_add_wht_wht_4x4_c (lossless).
void inv_txfm_add_wht_wht_4x4(PixelView dst, ArrayView<int32_t> coeff, int bitdepth_max) {
  int32_t tmp_storage[4 * 4];
  const ArrayView<int32_t> tmp(tmp_storage);
  for (int y = 0; y < 4; y++) {
    const Lane c{tmp, y * 4, 1};
    for (int x = 0; x < 4; x++) c[x] = coeff[static_cast<uint64_t>(y + x * 4)] >> 2;
    inv_wht4_1d(c);
  }
  coeff.first(16).fill(0);

  for (int x = 0; x < 4; x++) inv_wht4_1d(Lane{tmp, x, 4});

  int i = 0;
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++, i++) {
      Pixel& p = dst.at(x, y);
      p = clip_pixel(p + tmp[static_cast<uint64_t>(i)], bitdepth_max);
    }
}

}  // namespace

uint32_t itx_coef_count(int tx) {
  if (tx < 0 || tx >= N_RECT_TX_SIZES) return 0;
  const uint32_t w = 4u << k_tx_lw[tx], h = 4u << k_tx_lh[tx];
  return (w < 32 ? w : 32) * (h < 32 ? h : 32);
}

bool inv_txfm_add(PixelView dst, ArrayView<int32_t> coef, int tx, int txtp, int eob, int bitdepth_max) {
  if (tx < 0 || tx >= N_RECT_TX_SIZES || eob < 0) return false;
  if (coef.size() < itx_coef_count(tx)) return false;
  if (txtp == WHT_WHT) {
    if (tx != TX_4X4) return false;
    inv_txfm_add_wht_wht_4x4(dst, coef, bitdepth_max);
    return true;
  }
  if (txtp < 0 || txtp >= N_TX_TYPES) return false;
  // The sizes with a 64-point side only have the DCT; with a 32-point side only the DCT and the identity (dav1d has no other functions).
  const int big = k_tx_lw[tx] > k_tx_lh[tx] ? k_tx_lw[tx] : k_tx_lh[tx];
  if (big == 4 && txtp != DCT_DCT) return false;
  if (big == 3 && txtp != DCT_DCT && txtp != IDTX) return false;
  // The ADST/FLIPADST-with-identity types exist for 4x4 .. 16x8 only, not 16x16 (the sizes dav1d instantiates with inv_txfm_fn84).
  if (txtp >= V_ADST && k_tx_lw[tx] == 2 && k_tx_lh[tx] == 2) return false;
  inv_txfm_add_c(dst, coef, eob, tx, k_tx_shift[tx], txtp, bitdepth_max);
  return true;
}

}  // namespace av1
}  // namespace hardware
