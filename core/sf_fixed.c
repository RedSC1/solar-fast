/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * 派生自 js-ephemeris-lite v1.2.0（MPL-2.0），详见 NOTICE。
 */
/* ==================================================================
 * sf_fixed.c —— 定点相位 + Q31 查表三角实现（s3 内层）
 *
 * 查表函数内部**无 float/double**：象限折叠、索引、线性插值全是整数。
 * 中间乘法用 int64，避免 (y1-y0)*frac 溢出。
 * ================================================================== */
#include "sf_fixed.h"
#include "sf_lut.h"

#include <math.h>

/* 必须与 sf_series.c 的 wrap 用同一个 2π（逐位可比性依赖它） */
#define SF_TWO_PI 6.2831853071795862

/* 查表内部：u ∈ [0, 2^30] 的象限内偏移 -> Q31 sin 值（非负） */
static inline int32_t sf_lut_eval(uint32_t u)
{
    const uint32_t idx  = u >> SF_LUT_FRAC_BITS;
    const uint32_t frac = u & ((1u << SF_LUT_FRAC_BITS) - 1u);
    const int32_t  y0   = SF_LUT_TAB[idx];
    /* idx == SF_LUT_N 时 frac 必为 0，钳位以免越界读 */
    const uint32_t j    = idx + 1u;
    const int32_t  y1   = SF_LUT_TAB[j <= (uint32_t)SF_LUT_N ? j : (uint32_t)SF_LUT_N];
    const int64_t  num  = ((int64_t)(y1 - y0) * (int64_t)frac)
                        + ((int64_t)1 << (SF_LUT_FRAC_BITS - 1));
    return y0 + (int32_t)(num >> SF_LUT_FRAC_BITS);
}

int32_t sf_sin_q31(sf_phase32_t phase)
{
    const uint32_t q   = phase >> 30;            /* 0..3 象限 */
    const uint32_t off = phase & 0x3FFFFFFFu;    /* 象限内偏移 */
    const uint32_t u   = (q & 1u) ? (0x40000000u - off) : off;
    const int32_t  y   = sf_lut_eval(u);
    return (q >= 2u) ? -y : y;
}

int32_t sf_cos_q31(sf_phase32_t phase)
{
    return sf_sin_q31(phase + SF_PHASE32_QUARTER);  /* uint32 回绕 = mod 2π */
}

sf_phase32_t sf_angle_to_phase32(double radians)
{
    const double w = radians - SF_TWO_PI * floor((radians + SF_TWO_PI * 0.5) / SF_TWO_PI);
    const double t = w * (4294967296.0 / SF_TWO_PI);
    const int64_t k = (int64_t)floor(t + 0.5);
    return (sf_phase32_t)((uint64_t)k & 0xFFFFFFFFu);
}

double sf_phase32_to_angle(sf_phase32_t p)
{
    return (double)p * (SF_TWO_PI / 4294967296.0);
}
