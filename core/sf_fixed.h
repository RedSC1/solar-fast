/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * 派生自 js-ephemeris-lite v1.2.0（MPL-2.0），详见 NOTICE。
 */
/* ==================================================================
 * sf_fixed.h —— 定点相位 + Q31 查表三角（s3 内层）
 *
 * 这一层是 solar-fast 唯一「不是 double」的地方。它只服务级数的 term
 * inner loop：相位定点、三角函数查表、幅值定点、组内 int64 累加。
 * 组边界一次 int64 -> double，之后的外层多项式 / 帧投影 / 章动 / 速率 /
 * 求解器全是 double。
 *
 * 约定（与 qfix s3 实验一致，改动会导致数值不再可比）
 * ---------------------------------------------------------------
 *   phase32 (uint32) 表示一圈 = 2π
 *     0x00000000 = 0
 *     0x40000000 = π/2
 *     0x80000000 = π
 *     0xC0000000 = 3π/2
 *     uint32 自然回绕就是 mod 2π —— 运行期没有 fmod，没有 64 位除法
 *
 *   Q31 定点：0.0 -> 0，1.0 -> INT32_MAX (2147483647)
 *     注意不是 2^31。这个「差 1」由组边界的 scale 里那个
 *     2^31/(2^31−1) 因子吸收，不能省（省了是 4.66e-10 的相对偏差）。
 *
 *   查表是四分之一波 [0, π/2]，N+1 个 entry（含两端点），线性插值。
 *   查表函数内部**不使用任何 float/double**。
 * ================================================================== */
#ifndef SF_FIXED_H
#define SF_FIXED_H

#include <stdint.h>

typedef uint32_t sf_phase32_t;

#define SF_PHASE32_QUARTER 0x40000000u
#define SF_PHASE32_HALF    0x80000000u
#define SF_PHASE32_FULL    0xFFFFFFFFu

/* 表长。qfix 的消融结论：N=1024 不够（3.15 s 节气误差，超过模型自身
 * 的 1.812 s 档次），N=4096 是最低可接受（0.134 s），N=8192 更宽裕。
 * 默认 4096 = 16 KB flash。 */
#ifndef SF_LUT_N
#define SF_LUT_N 4096
#endif

#if   SF_LUT_N == 1024
#  define SF_LUT_LOG2N 10
#  define SF_LUT_TAB   SF_SIN_LUT_1024
#elif SF_LUT_N == 2048
#  define SF_LUT_LOG2N 11
#  define SF_LUT_TAB   SF_SIN_LUT_2048
#elif SF_LUT_N == 4096
#  define SF_LUT_LOG2N 12
#  define SF_LUT_TAB   SF_SIN_LUT_4096
#elif SF_LUT_N == 8192
#  define SF_LUT_LOG2N 13
#  define SF_LUT_TAB   SF_SIN_LUT_8192
#elif SF_LUT_N == 16384
#  define SF_LUT_LOG2N 14
#  define SF_LUT_TAB   SF_SIN_LUT_16384
#else
#  error "SF_LUT_N 必须是 1024/2048/4096/8192/16384"
#endif

/* 相位低 30 位地址一个象限，所以象限内小数位宽 = 30 − log2N */
#define SF_LUT_FRAC_BITS (30 - SF_LUT_LOG2N)

/* ---- 核心：Q31 sin / cos ---- */
int32_t sf_sin_q31(sf_phase32_t phase);
int32_t sf_cos_q31(sf_phase32_t phase);

/* ---- 角度 <-> phase32（只在测试和标定路径上用，不进 term loop）---- */
sf_phase32_t sf_angle_to_phase32(double radians);
double       sf_phase32_to_angle(sf_phase32_t p);

/* ---- Q31 <-> double ---- */
static inline double sf_q31_to_double(int32_t v)
{
    return (double)v * (1.0 / 2147483647.0);
}

/* phase32 的 LSB 对应 2π·2^-32 rad */
#define SF_PHASE32_RAD_PER_LSB 1.4629180792671596e-09

/* ==================================================================
 * 幅值：term ≈ AF·c/2^31，其中 AF = ahi·2^32 + alo，|c| ≤ 2^31
 *
 * 恒等式（生成器保证）：
 *     ahi·c·2 + (alo·c)>>31  =  (ahi·2^32 + alo)·c / 2^31
 *
 * 两个必须知道的约束：
 *   1) (alo·c) 当 alo·c < 0 时是负数的右移，C99 里是实现定义。实际
 *      编译器一律算术右移。**不要加宽 alo 或 c 的类型** ——
 *      (2^32−1)·(2^31−1) = 2^63 − 2^32 − 2^31 + 1 距 int64 溢出只有
 *      0 个 ULP 的余量。
 *   2) 这里已经吸收了 Q31 的量纲，调用方不得再乘 SF_Q31_TO_DBL。
 *      （多乘一次会使位置误差变成 1.7e-7 rad。）
 * ================================================================== */
static inline int64_t sf_amp_term(int32_t ahi, uint32_t alo, int32_t c)
{
    return (int64_t)ahi * (int64_t)c * 2 + (((int64_t)alo * (int64_t)c) >> 31);
}

#endif /* SF_FIXED_H */
