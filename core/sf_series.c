/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * 派生自 js-ephemeris-lite v1.2.0（MPL-2.0），详见 NOTICE。
 */
/* ==================================================================
 * sf_series.c —— 级数求值（定点内层）+ 帧投影 + 章动 + 位置输出
 *
 * 结构：只有 term inner loop 是定点（s3），其余全 double。
 *
 *   Earth 一项：A·cos(B + C·τ)·τ^power        τ = (jd − J2000)/365250
 *   Moon  一项：A·cos(Horner(arg[7..0], x)·x + phase)   x = (jd − J2000)/2922000
 *
 *   求值顺序**必须按幂次分块**：块内先求和，再乘 τ^power / x^power 加到总值。
 *   这不是风格问题 —— 改成逐项加权求和会改变浮点末位（fl_qishuo.c 里
 *   明确警告过，也是与 JS 逐位一致的前提）。
 *
 *   定点部分：相位 phase32 + Q31 查表余弦 + Q52 幅值 + int64 组内累加，
 *   每组边界转一次 double，之后走 double Horner。
 *
 * 数值口径与 fl_qishuo.c 的关系
 *   黄经路径刻意保持与 fl_qishuo.c 可比：帧投影的前 12 行逐位相同，
 *   块累加顺序相同，章动倒序累加相同。差别只在 term inner loop 的定点化
 *   （实测增量 max 0.0069″ / rms 0.0029″）和默认预算档。
 * ================================================================== */
#include "solar_fast.h"
#include "sf_fixed.h"
#include "sf_internal.h"

#include <math.h>

/* ==================================================================
 * 时间量：整数天 D + Q32 小数 fq，外加 Moon 需要的 x^m 的 Q32
 * ================================================================== */
typedef struct { int64_t D; uint64_t fq; int64_t Pow[9]; } sf_time_t;

static void sf_time_prep(double jd, sf_time_t *T)
{
    const double d = jd - SF_J2000;
    int64_t D = (int64_t)floor(d);
    double frac = d - (double)D;
    if (frac < 0.0) { frac += 1.0; D -= 1; }
    T->D = D;
    /* frac ∈ [0,1)，所以 fq ∈ [0, 2^32]。frac 极接近 1 时 llround 会给出
     * 恰好 2^32 —— 下面掩一次，保证 fq < 2^32（溢出预算的前提）。 */
    T->fq = (uint64_t)llround(frac * 4294967296.0) & 0xFFFFFFFFu;

    {
        /* x = X/2922000 的 Q32：X = D + fq·2^-32，两级拆分，无 64 位除法 */
        int64_t dq = (int64_t)SF_MOON_K2D * D + (((int64_t)SF_MOON_K3D * D) >> 31);
        int64_t fq32 = (((int64_t)T->fq * SF_MOON_RF) + (1LL << (SF_MOON_RSH - 1)))
                       >> SF_MOON_RSH;
        int64_t Xq = dq + fq32;
        /* |x| ≤ 1 的替身钳位（表在 |x| > 1 之外本来也无效） */
        if (Xq >  4294967296LL) Xq =  4294967296LL;
        if (Xq < -4294967296LL) Xq = -4294967296LL;
        const int64_t Ah = Xq >> 16, Bl = Xq & 0xFFFF;
        int64_t p = Ah * Ah + ((2 * Ah * Bl + (1 << 15)) >> 16);
        T->Pow[2] = p;
        for (int m = 3; m <= 8; m++) {
            p = ((p * Ah + (1 << 15)) >> 16) + ((p * Bl + (1LL << 31)) >> 32);
            T->Pow[m] = p;
        }
    }
}

/* ==================================================================
 * 相位
 *
 * ω·(D + fq·2^-32)·2^32 = (k2 + k3·2^-31)·D + (k2 + k3·2^-31)·fq·2^-32
 *
 * 溢出（|D| ≤ 2922000，fq < 2^32；|k2| ≤ 0.2501·2^32 = 1.074e9）：
 *   |k2·D| ≤ 3.14e15  |k3·D| ≤ 3.14e15
 *   |k2·fq| ≤ 4.61e18 |k3·fq| ≤ 4.61e18  （INT64_MAX = 9.22e18，约 2× 余量）
 *
 * 四个右移**各自带舍入常数**。改成截断会让每一相都偏 −0.5 LSB，几百项
 * 叠起来就是一个同向偏置，表现为"查表越细误差越大"的假象。
 * ================================================================== */
static inline sf_phase32_t sf_phase_lin(int64_t D, uint64_t fq,
                                        uint32_t phase0, int32_t k2, int32_t k3)
{
    const int64_t kd = (int64_t)k2 * D;
    const int64_t kf = ((int64_t)k2 * (int64_t)fq + (1LL << 31)) >> 32;
    const int64_t rd = ((int64_t)k3 * D + (1LL << 30)) >> 31;
    const int64_t rf = (((int64_t)k3 * (int64_t)fq + (1LL << 30)) >> 31) >> 32;
    int64_t acc = (int64_t)phase0 + kd + kf + rd + rf;
    return (sf_phase32_t)acc;       /* uint32 回绕 = mod 2π */
}

static inline sf_phase32_t sf_ephase32(const sf_eterm_t *t, const sf_time_t *T)
{
    return sf_phase_lin(T->D, T->fq, t->phase0, t->k2, t->k3);
}

/* Moon：线性主项走 phase_lin，非线性部分 arg[1..7]·x^{2..8} 走定点多项式 */
static inline sf_phase32_t sf_mphase32(const sf_mterm_t *t, const sf_time_t *T)
{
    int64_t s = 0;
    for (int j = 0; j < 7; j++) s += (int64_t)t->b[j] * T->Pow[j + 2];
    sf_phase32_t base = sf_phase_lin(T->D, T->fq, t->phase0, t->k2, t->k3);
    return (sf_phase32_t)((int64_t)base + ((s + (1 << (SF_POLY_K - 1))) >> SF_POLY_K));
}

/* ==================================================================
 * Earth 一个坐标（s3）
 *
 * counts 是按**组索引**（不是 power）给的上限；传 NULL 表示不截断。
 * 组索引与 power 相同是这批表的性质（power 从 0 连续无缺口），
 * 换级数时要保证这一点，否则语义会变。
 *
 * gr->scale 里已经含了 2^31/(2^31−1)，所以这里**不要**再乘 SF_Q31_TO_DBL。
 * ================================================================== */
#define SF_MAXGROUPS 16          /* 当前表 ng ≤ 12；超过会静默爆栈，留个断言 */

static double sf_earth_coord(int n, const sf_eterm_t *terms,
                             const sf_group_t *G, int ng,
                             const int *counts, const sf_time_t *T,
                             double tau)
{
    double S[SF_MAXGROUPS];
    if (n <= 0 || ng > SF_MAXGROUPS) return 0.0;

    for (int g = 0; g < ng; g++) {
        const sf_group_t *gr = &G[g];
        const int lim = counts ? counts[g] : -1;
        const int eff = (lim < 0 || lim > gr->n) ? gr->n : lim;
        const int end = gr->start + eff;
        int64_t sum = 0;
        for (int i = gr->start; i < end; i++) {
            const sf_phase32_t p = sf_ephase32(&terms[i], T);
            const int32_t c = sf_cos_q31(p);
            sum += sf_amp_term(terms[i].ahi, terms[i].alo, c);
        }
        S[g] = (double)sum * gr->scale;
    }
    {
        double r = 0.0;
        for (int g = ng - 1; g >= 0; g--) r = S[g] + tau * r;
        return r;
    }
}

/* Moon 一个坐标（s3）。counts 总是非 NULL（B/R 的档位表里没有 -1，
 * 顶档就是导出上限，负数由调用方折算成顶档索引）。 */
static double sf_moon_coord(int n, const sf_mterm_t *terms,
                            const sf_group_t *G, int ng,
                            const int *counts, const sf_time_t *T, double x)
{
    double S[SF_MAXGROUPS];
    if (n <= 0 || ng > SF_MAXGROUPS) return 0.0;

    for (int g = 0; g < ng; g++) {
        const sf_group_t *gr = &G[g];
        const int lim = counts ? counts[g] : -1;
        const int eff = (lim < 0 || lim > gr->n) ? gr->n : lim;
        const int end = gr->start + eff;
        int64_t sum = 0;
        for (int i = gr->start; i < end; i++) {
            const sf_phase32_t p = sf_mphase32(&terms[i], T);
            const int32_t c = sf_cos_q31(p);
            sum += sf_amp_term(terms[i].ahi, terms[i].alo, c);
        }
        S[g] = (double)sum * gr->scale;
    }
    {
        double r = 0.0;
        for (int g = ng - 1; g >= 0; g--) r = S[g] + x * r;
        return r;
    }
}

/* ==================================================================
 * 一组坐标
 * ================================================================== */
static void sf_earth_values(double jd, int li, int bi, int ri, double out[3])
{
    const double tau = (jd - SF_J2000) / SF_MILLENNIUM;
    sf_time_t T;
    sf_time_prep(jd, &T);

    /* 注意：负数也要当"全量"。头文件写的是「传 -1 表示不截断」，而 *_BUD_FULL
     * 是最后一行的索引（11/6/11），两者不等价 —— 只判 FULL_IDX 时传 -1 会直接
     * PC[-1] 越界（UBSan 抓到过），且因读到的是相邻内存，会静默返回一个错值
     * 而不是崩溃。 */
    const int *lc = (li < 0 || li == SF_EARTH_L_FULL_IDX) ? 0 : SF_EARTH_L_PC[li];
    const int *bc = (bi < 0 || bi == SF_EARTH_B_FULL_IDX) ? 0 : SF_EARTH_B_PC[bi];
    const int *rc = (ri < 0 || ri == SF_EARTH_R_FULL_IDX) ? 0 : SF_EARTH_R_PC[ri];

    out[0] = sf_earth_coord(SF_EARTH_L_N, SF_EARTH_L, SF_EARTH_L_G, SF_EARTH_L_NG, lc, &T, tau);
    out[1] = sf_earth_coord(SF_EARTH_B_N, SF_EARTH_B, SF_EARTH_B_G, SF_EARTH_B_NG, bc, &T, tau);
    out[2] = sf_earth_coord(SF_EARTH_R_N, SF_EARTH_R, SF_EARTH_R_G, SF_EARTH_R_NG, rc, &T, tau);
}

static void sf_moon_values(double jd, int ml, int mb, int mr, double out[3])
{
    const double x = (jd - SF_J2000) / SF_SCALE_DAYS;
    sf_time_t T;
    sf_time_prep(jd, &T);

    const int *lc = (ml < 0) ? SF_MOON_L_PC[SF_MOON_L_FULL_IDX] : SF_MOON_L_PC[ml];
    const int *bc = (mb < 0) ? SF_MOON_B_PC[SF_MOON_B_BUD_FULL] : SF_MOON_B_PC[mb];
    /* MOON_R 的档位表里没有 -1（顶档 138 就是导出上限），负数走顶档 */
    const int *rc = (mr < 0) ? SF_MOON_R_PC[SF_MOON_R_BUD_FULL] : SF_MOON_R_PC[mr];

    out[0] = sf_moon_coord(SF_MOON_L_N, SF_MOON_L, SF_MOON_L_G, SF_MOON_L_NG, lc, &T, x);
    out[0] += sf_poly(SF_MOON_W1, SF_MOON_W1_N, x);
    out[1] = sf_moon_coord(SF_MOON_B_N, SF_MOON_B, SF_MOON_B_G, SF_MOON_B_NG, bc, &T, x);
    out[2] = sf_moon_coord(SF_MOON_R_N, SF_MOON_R, SF_MOON_R_G, SF_MOON_R_NG, rc, &T, x);
}

/* ==================================================================
 * 帧投影
 *
 * z = [cosλ, sinλ, tanβ]（cosβ 在 atan2 里约掉）：
 *     λ = atan2(row1·z, row0·z)
 *     β = atan2(row2·z, hypot(row0·z, row1·z))
 *
 * 地球行组从 offset 0 起（行 0/1 在 0..5，行 2 在 12..14）
 * 月亮行组从 offset 6 起（行 0/1 在 6..11，行 2 在 15..17）
 *
 * ------------------------------------------------------------------
 * SF_FRAME_PROJ 是什么：**Vondrák 2011 岁差的多项式替身**，不是原公式
 * ------------------------------------------------------------------
 * 这张表由离线探针 gen_frames.mjs（未随库发布）生成，源头是上游
 * coordinates.js 的 meanEclipticOfDateMatrix（月亮那条另有 MOON_PRECESSION
 * _P/Q，本身即原式，没有替身问题）：
 *
 *     Vondrák–Capitaine–Wallace (2011)
 *       8 个黄道周期项 + 14 个赤道周期项
 *       + PA/QA/XA/YA 长期多项式 + IAU2006 平交角 + 帧偏置
 *             ↓  256 节点 × 16 次 Chebyshev，x ∈ [−1,1]
 *       SF_FRAME_PROJ（18 行 × 17 系数）
 *
 * 所以**运行时执行的不是论文原公式**。对外说明要写成
 * "Vondrák 2011-derived polynomial surrogate over ±8000 years"，
 * 不能光写 "Vondrák 2011"。实测替身 vs 原公式：黄经最大差 1.04e-6″。
 *
 * 这里适合用替身而章动不适合，是因为**谱不一样**：Vondrák 的周期项最短
 * 157.87 世纪（15787 年），在 ±8000 年的窗口里连一个整周期都走不完，
 * 是条慢弧，和多项式项数学上不可区分；章动最短的 Ω 是 18.6 年，窗口里转
 * 860 圈，多项式根本表示不了。这不是风格选择，是函数的性质。
 *
 * 不得把它换成「原公式 + Q31 查表」：ESP32-S3 / QEMU 实测（README §六 有表）
 * 替身 42.7 µs / 1.04e-6″，原公式 + 查表 66.8 µs / 7.19e-4″，原公式 + libm
 * 196.0 µs。帧投影的成本不在三角函数上（42.7 = 25.4 多项式 + 21.6 三角），
 * 而在直算版必须每次重做的叉积 / 归一化 / 2 次 3×3 乘法 / 平交角 —— 这些
 * double 线性代数比替身那 102 层 Horner 还贵。替身是把整圈线性代数都烘进
 * 系数了，不只是躲开三角函数。
 *
 * 注意：换掉帧投影不能解除 ±8000 年窗口：同一个窗口还用在级数排序窗口、
 * 速率多项式、SF_EVENT_JD_LIMIT、sf_time_prep 的 x 钳位、月球 Pow[2..8]。
 * ================================================================== */
#define SF_FRAME_ROW2_EARTH 12
#define SF_FRAME_ROW2_MOON  15

static void sf_frame_xyz(const double vals[2], double x, int offset, int row2,
                         double *X, double *Y, double *Z)
{
    const double cl = cos(vals[0]);
    const double sl = sin(vals[0]);
    const double tl = tan(vals[1]);
    const double *r0 = SF_FRAME_PROJ[offset + 0];
    const double *r1 = SF_FRAME_PROJ[offset + 3];
    const double *r2 = SF_FRAME_PROJ[row2];
    *X = sf_poly(r0, SF_FRAME_DEG + 1, x) * cl
       + sf_poly(SF_FRAME_PROJ[offset + 1], SF_FRAME_DEG + 1, x) * sl
       + sf_poly(SF_FRAME_PROJ[offset + 2], SF_FRAME_DEG + 1, x) * tl;
    *Y = sf_poly(r1, SF_FRAME_DEG + 1, x) * cl
       + sf_poly(SF_FRAME_PROJ[offset + 4], SF_FRAME_DEG + 1, x) * sl
       + sf_poly(SF_FRAME_PROJ[offset + 5], SF_FRAME_DEG + 1, x) * tl;
    *Z = sf_poly(r2, SF_FRAME_DEG + 1, x) * cl
       + sf_poly(SF_FRAME_PROJ[row2 + 1], SF_FRAME_DEG + 1, x) * sl
       + sf_poly(SF_FRAME_PROJ[row2 + 2], SF_FRAME_DEG + 1, x) * tl;
}

/* 只要经度时用这个 —— 与 fl_qishuo.c 的 frame_longitude 逐位可比 */
static double sf_frame_longitude(const double vals[2], double x, int offset)
{
    const double cl = cos(vals[0]);
    const double sl = sin(vals[0]);
    const double tl = tan(vals[1]);
    const double c0 = sf_poly(SF_FRAME_PROJ[offset + 0], SF_FRAME_DEG + 1, x);
    const double c1 = sf_poly(SF_FRAME_PROJ[offset + 1], SF_FRAME_DEG + 1, x);
    const double c2 = sf_poly(SF_FRAME_PROJ[offset + 2], SF_FRAME_DEG + 1, x);
    const double c3 = sf_poly(SF_FRAME_PROJ[offset + 3], SF_FRAME_DEG + 1, x);
    const double c4 = sf_poly(SF_FRAME_PROJ[offset + 4], SF_FRAME_DEG + 1, x);
    const double c5 = sf_poly(SF_FRAME_PROJ[offset + 5], SF_FRAME_DEG + 1, x);
    return atan2(c3 * cl + c4 * sl + c5 * tl,
                 c0 * cl + c1 * sl + c2 * tl);
}

/* ==================================================================
 * 章动（IAU 2000B）
 *
 * 基本引数先 fmod 到 1296000″（= 360°）再转弧度。
 * 累加**倒序**（i = n-1 → 0），与 JS 一致 —— 改顺序会动末位。
 * 返回 Δψ 与 Δε（弧度）。
 * ================================================================== */
static const double SF_FA_A[5] = {
    485868.249036, 1287104.79305, 335779.526232, 1072260.70369, 450160.398036
};
static const double SF_FA_B[5] = {
    1717915923.2178, 129596581.0481, 1739527262.8478, 1602961601.2090, -6962890.5431
};

/* 两条实现共用：项数夹紧 */
static int nut_clamp(int term_count)
{
    int m = term_count;
    if (m < 0) m = 0;
    if (m > SF_IAU2000B_N) m = SF_IAU2000B_N;
    return m;
}

/* Q31 满量程是 **2^31−1**（表在 π/2 处正好是 INT32_MAX，误差 0），
 * 不是 2^31 —— 用错常数是一个 4.66e-10 的系统性相对偏差。 */
#define SF_Q31_UNIT (1.0 / 2147483647.0)

/* 基本引数 → phase32。5 个多项式，每次求值只算一次，不涉及项循环，
 * 所以这一步留在 double 里（正是「最后乘多项式」的那一层）。 */
static void nut_fa_phase(double jd, sf_phase32_t fa[5])
{
    const double t = (jd - SF_J2000) / SF_CENTURY;
    for (int i = 0; i < 5; i++) {
        const double arcsec = fmod(SF_FA_A[i] + t * SF_FA_B[i], 1296000.0);
        fa[i] = sf_angle_to_phase32(arcsec * SF_ARCSEC_TO_RAD);
    }
}

/* ==================================================================
 * 章动（IAU 2000B）—— 与级数求值同一个套路
 *
 *   内层：相位累加、三角函数、求和**全是整数**（int64 + Q31 查表），
 *   出循环才转 double 乘时间多项式 t。
 *
 * 关键是**把 t 提到循环外**：
 *     Σ (ps + pst·t)·s = Σ ps·s + t·Σ pst·s
 *     Σ (ec + ect·t)·c = Σ ec·c + t·Σ ect·c
 * 于是循环体内只剩 int32×int32→int64，一次 double 运算都没有。
 * 不提出来的话，每项要做 6 次 double 乘法 + 6 次 double→int（表是整型，
 * 但系数是 double），在**没有硬件 FP64** 的 Xtensa 上这是主成本 ——
 * 实测每项 5.5 µs 里大部分都在那儿。
 *
 * 溢出：六个累加器里最大的是 Σ|ps|·2^31 = 1.94e8 × 2.15e9 = 4.18e17，
 * INT64_MAX = 9.22e18，**余量 22 倍**。相位累加 |Σ r[k]·fa[k]| ≤ 8.6e10，
 * 取低 32 位即 mod 2π（与 sf_phase_lin 同一手法）。
 * ================================================================== */
void sf_nutation_iau2000b(double jd, int term_count, double *dpsi, double *deps)
{
    const double t = (jd - SF_J2000) / SF_CENTURY;
    sf_phase32_t fa[5];
    nut_fa_phase(jd, fa);

    int64_t sp = 0, spt = 0, cp = 0;      /* Σ ps·s、 Σ pst·s、 Σ pc·c */
    int64_t ce = 0, cet = 0, se = 0;      /* Σ ec·c、 Σ ect·c、 Σ es·s */

    const int m = nut_clamp(term_count);
    for (int i = m - 1; i >= 0; i--) {
        const int32_t *r = SF_IAU2000B[i];
        int64_t a = 0;
        for (int k = 0; k < 5; k++) a += (int64_t)r[k] * (int64_t)fa[k];
        const sf_phase32_t ph = (sf_phase32_t)a;     /* uint32 回绕 = mod 2π */
        const int32_t s = sf_sin_q31(ph);
        const int32_t c = sf_cos_q31(ph);
        sp  += (int64_t)r[5]  * s;
        spt += (int64_t)r[6]  * s;
        cp  += (int64_t)r[7]  * c;
        ce  += (int64_t)r[8]  * c;
        cet += (int64_t)r[9]  * c;
        se  += (int64_t)r[10] * s;
    }

    const double dp = ((double)sp + (double)cp + t * (double)spt) * SF_Q31_UNIT;
    const double de = ((double)ce + (double)se + t * (double)cet) * SF_Q31_UNIT;
    *dpsi = (-0.000135 + dp * 1e-7) * SF_ARCSEC_TO_RAD;
    *deps = ( 0.000388 + de * 1e-7) * SF_ARCSEC_TO_RAD;
}

/* libm 参照实现。不参与计算路径，仅留给验证当对照 ——
 * 保留独立参照物，避免实现出错时与自身错法一致。 */
void sf_nutation_iau2000b_dbl(double jd, int term_count, double *dpsi, double *deps)
{
    const double t = (jd - SF_J2000) / SF_CENTURY;
    double fa[5];
    for (int i = 0; i < 5; i++)
        fa[i] = fmod(SF_FA_A[i] + t * SF_FA_B[i], 1296000.0) * SF_ARCSEC_TO_RAD;

    double dp = 0.0, de = 0.0;
    const int m = nut_clamp(term_count);
    for (int i = m - 1; i >= 0; i--) {
        const int32_t *r = SF_IAU2000B[i];
        const double a = r[0] * fa[0] + r[1] * fa[1] + r[2] * fa[2]
                       + r[3] * fa[3] + r[4] * fa[4];
        const double s = sin(a), c = cos(a);
        dp += (r[5] + r[6] * t) * s + r[7] * c;      /* ps + pst·t, pc */
        de += (r[8] + r[9] * t) * c + r[10] * s;     /* ec + ect·t, es */
    }
    *dpsi = (-0.000135 + dp * 1e-7) * SF_ARCSEC_TO_RAD;
    *deps = ( 0.000388 + de * 1e-7) * SF_ARCSEC_TO_RAD;
}

/* IAU 2006 平黄赤交角（弧度） */
double sf_mean_obliquity(double jd)
{
    const double t = (jd - SF_J2000) / SF_CENTURY;
    return sf_poly(SF_MEAN_OBLQ, 6, t) * SF_ARCSEC_TO_RAD;
}

/* ==================================================================
 * 位置
 * ================================================================== */

double sf_sun_apparent_longitude_budget(double jd, int l_bud, int nut_terms,
                                        int b_bud, int r_bud)
{
    double earth[3];
    sf_earth_values(jd, l_bud, b_bud, r_bud, earth);
    const double x = (jd - SF_J2000) / SF_SCALE_DAYS;
    double dpsi, deps;
    sf_nutation_iau2000b(jd, nut_terms, &dpsi, &deps);
    return sf_wrap(sf_frame_longitude(earth, x, 0) + M_PI)
         + dpsi
         - SF_ABERRATION / earth[2];
}

double sf_sun_apparent_longitude(double jd)
{
    return sf_sun_apparent_longitude_budget(jd, SF_DEF_L_BUD, SF_DEF_NUT,
                                            SF_DEF_B_BUD, SF_DEF_R_BUD);
}

/* 几何黄经：帧投影到 of-date 之后就停，不加章动、不做光行差/光行时。
 * 这是与 DE441 可比的量（真值 CSV 的 ty_geom_deg 列）。 */
double sf_sun_geometric_longitude_budget(double jd, int l_bud, int b_bud, int r_bud)
{
    double earth[3];
    sf_earth_values(jd, l_bud, b_bud, r_bud, earth);
    const double x = (jd - SF_J2000) / SF_SCALE_DAYS;
    return sf_wrap(sf_frame_longitude(earth, x, 0) + M_PI);
}

double sf_sun_geometric_longitude(double jd)
{
    return sf_sun_geometric_longitude_budget(jd, SF_DEF_L_BUD, SF_DEF_B_BUD, SF_DEF_R_BUD);
}

double sf_sun_distance_budget(double jd, int r_bud)
{
    double earth[3];
    /* L/B 不影响 R（R 是独立坐标），传默认档只为让时间量化路径一致 */
    sf_earth_values(jd, SF_DEF_L_BUD, SF_DEF_B_BUD, r_bud, earth);
    return earth[2];
}

double sf_sun_distance(double jd)
{
    return sf_sun_distance_budget(jd, SF_DEF_R_BUD);
}

double sf_moon_distance_budget(double jd, int r_bud)
{
    double moon[3];
    /* L/B 与 R 无关 —— 三个坐标在 sf_moon_values 里各自独立求和，
     * 档位对 R 的影响是零。所以这里传 0 档，避免为了一个距离白算
     * 639 项 L（那会是 4.6 倍的无用功）。 */
    sf_moon_values(jd, 0, 0, r_bud, moon);
    return moon[2];
}

double sf_moon_distance(double jd)
{
    return sf_moon_distance_budget(jd, SF_DEF_MOON_R_BUD);
}

/* 地心月球的视赤经 / 视赤纬（弧度）与月心距（km）。
 *
 * 走的是与太阳同一条链，只是帧投影用月亮那组行（offset 6 / row2 15）：
 *     帧投影 → 几何 of-date (λ, β) → +Δψ +月球光行差 → 真交角转赤道
 * 光行差是 SF_LUNAR_ABERR × (DM0/d) —— 与 sf_elongation 里那一项同一个式子，
 * 只是那边为了省算力用的是 RANK:7 的距离、这边用调用方给的档，两者相差
 * 0.06% 的距离误差即 0.0004″ 的光行差，可以忽略。这样月球的位置和定朔用的
 * 距角自洽（朔已经验到 0.19 s rms）。
 * 黄纬不带章动，与太阳同理（二阶小量）。 */
void sf_moon_ra_dec_budget(double jd, double *ra, double *dec, double *dist_km,
                           int l_bud, int b_bud, int r_bud)
{
    double moon[3];
    sf_moon_values(jd, l_bud, b_bud, r_bud, moon);
    const double x = (jd - SF_J2000) / SF_SCALE_DAYS;

    double X, Y, Z;
    sf_frame_xyz(moon, x, 6, SF_FRAME_ROW2_MOON, &X, &Y, &Z);
    double lam = atan2(Y, X) + SF_LUNAR_ABERR * (SF_LUNAR_ABERR_REF_KM / moon[2]);
    const double bet = atan2(Z, hypot(X, Y));

    double dpsi, deps;
    sf_nutation_iau2000b(jd, SF_DEF_NUT, &dpsi, &deps);
    lam += dpsi;
    const double eps = sf_mean_obliquity(jd) + deps;

    const double sl = sin(lam), cl = cos(lam);
    const double sb = sin(bet), cb = cos(bet);
    const double se = sin(eps), ce = cos(eps);

    if (dec) *dec = asin(sb * ce + cb * se * sl);
    if (ra) {
        double a = atan2(sl * ce - (sb / cb) * se, cl);
        if (a < 0.0) a += SF_TWO_PI;
        *ra = a;
    }
    if (dist_km) *dist_km = moon[2];
}

void sf_moon_ra_dec(double jd, double *ra, double *dec, double *dist_km)
{
    sf_moon_ra_dec_budget(jd, ra, dec, dist_km, SF_DEF_MOON_L_BUD,
                          SF_MOON_B_BUD_FULL, SF_DEF_MOON_R_BUD);
}

/* 地心太阳的 of-date 黄纬（弧度）。
 * 地球级数给的是日心向量，(λ,β) → (λ+π, −β) 就是地心太阳方向。 */
double sf_sun_apparent_latitude_budget(double jd, int l_bud, int nut_terms,
                                       int b_bud, int r_bud)
{
    double earth[3];
    (void)nut_terms;   /* 黄纬不带章动：那是一阶以上的小量 */
    sf_earth_values(jd, l_bud, b_bud, r_bud, earth);
    const double x = (jd - SF_J2000) / SF_SCALE_DAYS;
    double X, Y, Z;
    sf_frame_xyz(earth, x, 0, SF_FRAME_ROW2_EARTH, &X, &Y, &Z);
    return -atan2(Z, hypot(X, Y));
}

double sf_sun_apparent_latitude(double jd)
{
    return sf_sun_apparent_latitude_budget(jd, SF_DEF_L_BUD, SF_DEF_NUT,
                                           SF_DEF_B_BUD, SF_DEF_R_BUD);
}

/* 视赤经 / 视赤纬。
 * λ_app 已经含章动与光行差；黄纬不带章动（那是二阶量）；
 * 交角用真交角 ε = ε_mean + Δε。标准黄道→赤道转换。 */
void sf_sun_ra_dec_budget(double jd_tt, double *ra, double *dec,
                          int l_bud, int nut_terms, int b_bud, int r_bud)
{
    const double lam = sf_sun_apparent_longitude_budget(jd_tt, l_bud, nut_terms,
                                                        b_bud, r_bud);
    const double bet = sf_sun_apparent_latitude_budget(jd_tt, l_bud, nut_terms,
                                                       b_bud, r_bud);
    double dpsi, deps;
    sf_nutation_iau2000b(jd_tt, nut_terms, &dpsi, &deps);
    (void)dpsi;
    const double eps = sf_mean_obliquity(jd_tt) + deps;

    const double sl = sin(lam), cl = cos(lam);
    const double sb = sin(bet), cb = cos(bet);
    const double se = sin(eps), ce = cos(eps);

    if (dec) *dec = asin(sb * ce + cb * se * sl);
    if (ra) {
        double a = atan2(sl * ce - (sb / cb) * se, cl);
        if (a < 0.0) a += SF_TWO_PI;
        *ra = a;
    }
}

void sf_sun_ra_dec(double jd_tt, double *ra, double *dec)
{
    sf_sun_ra_dec_budget(jd_tt, ra, dec, SF_DEF_L_BUD, SF_DEF_NUT,
                         SF_DEF_B_BUD, SF_DEF_R_BUD);
}

/* 一次地球级数求值同时给出视赤经、视赤纬、日地距离。
 *
 * sf_sun_ra_dec() 内部会算**两遍** sf_earth_values（经度那路一遍、纬度那路
 * 一遍），外面再调一次 sf_sun_distance() 就是第三遍 —— 每遍 911 项。
 * 出没求解一天要调它几百次，这个冗余是主要开销。三者在这里共用一次求值。
 *
 * 数值与那三个公开函数逐位一致：同一预算、同一个 sf_frame_xyz、同一个 wrap。 */
void sf_sun_ra_dec_dist(double jd_tt, double *ra, double *dec, double *dist_au)
{
    double earth[3];
    sf_earth_values(jd_tt, SF_DEF_L_BUD, SF_DEF_B_BUD, SF_DEF_R_BUD, earth);
    const double x = (jd_tt - SF_J2000) / SF_SCALE_DAYS;

    double X, Y, Z;
    sf_frame_xyz(earth, x, 0, SF_FRAME_ROW2_EARTH, &X, &Y, &Z);
    const double bet = -atan2(Z, hypot(X, Y));

    double dpsi, deps;
    sf_nutation_iau2000b(jd_tt, SF_DEF_NUT, &dpsi, &deps);
    const double lam = sf_wrap(atan2(Y, X) + M_PI) + dpsi - SF_ABERRATION / earth[2];
    const double eps = sf_mean_obliquity(jd_tt) + deps;

    const double sl = sin(lam), cl = cos(lam);
    const double sb = sin(bet), cb = cos(bet);
    const double se = sin(eps), ce = cos(eps);

    if (dec) *dec = asin(sb * ce + cb * se * sl);
    if (ra) {
        double a = atan2(sl * ce - (sb / cb) * se, cl);
        if (a < 0.0) a += SF_TWO_PI;
        *ra = a;
    }
    if (dist_au) *dist_au = earth[2];
}

double sf_elongation_budget(double jd, int moon_l, int earth_l, int moon_b,
                            int earth_b, int earth_r)
{
    double earth[3], moon[3];
    sf_earth_values(jd, earth_l, earth_b, earth_r, earth);
    /* R 档此处要用 —— 月球光行差要乘 DM0/out[2]（理由见 sf_data.h），
     * 故不能传 0。取 RANK:7（7 项）足够：距离误差 229 km rms（0.06%），
     * 对应光行差残差 0.003″，与全量 325 项同值；RANK:3（2152 km）会留 0.006″。
     * 多这 4 项在 L639+B277+R7 里量不出来。
     * （B 档另说：实测把 B 档从 7 项换成全量 277 项，距角变化恰好 0.00″ ——
     * 月球那组帧投影行里 tanβ 的系数是 0（纯黄道面内旋转），β 完全不进距角。） */
    sf_moon_values(jd, moon_l, moon_b, SF_MOON_R_BUD_7, moon);
    const double x = (jd - SF_J2000) / SF_SCALE_DAYS;
    /* 注意符号：SF_LUNAR_ABERR 的值是 −3.4e-6（与 fl_qishuo.c 里
     * 字面写的 `- 3.4e-6` 等价），所以这里是**加**不是减。
     * 写成 `- SF_LUNAR_ABERR` 会翻号，距角整体偏 +1.40″（2×3.4e-6 rad）。 */
    return sf_wrap(sf_frame_longitude(moon, x, 6)
                 - sf_frame_longitude(earth, x, 0)
                 - M_PI + SF_LUNAR_ABERR * (SF_LUNAR_ABERR_REF_KM / moon[2])
                 + SF_ABERRATION / earth[2]);
}

/* ==================================================================
 * 归日专用事件方程
 *
 * 这不是完整位置 API 的另一个 cache，而是寿星 qi_low/so_low 那种“只算事件
 * 方程”的短路径：一次准备时间量，只取事件需要的经度主项；不求距离、赤纬、
 * 通用拟合速度。气保留 9 项章动，朔在同一原生黄道系直接相减；20 分钟日界
 * 护栏外完全够用。
 * ================================================================== */
double sf_sun_event_longitude_low(double jd_tt)
{
    const double tau = (jd_tt - SF_J2000) / SF_MILLENNIUM;
    const double x = (jd_tt - SF_J2000) / SF_SCALE_DAYS;
    sf_time_t T;
    sf_time_prep(jd_tt, &T);

    double earth[2];
    earth[0] = sf_earth_coord(SF_EARTH_L_N, SF_EARTH_L, SF_EARTH_L_G,
                              SF_EARTH_L_NG, SF_EARTH_L_PC[SF_EARTH_L_BUD_32],
                              &T, tau);
    earth[1] = sf_earth_coord(SF_EARTH_B_N, SF_EARTH_B, SF_EARTH_B_G,
                              SF_EARTH_B_NG, SF_EARTH_B_PC[SF_EARTH_B_BUD_3],
                              &T, tau);
    double dpsi, deps;
    sf_nutation_iau2000b(jd_tt, 9, &dpsi, &deps);
    (void)deps;
    return sf_wrap(sf_frame_longitude(earth, x, 0) + M_PI)
         + dpsi - SF_ABERRATION;
}

double sf_lunar_event_elongation_low(double jd_tt)
{
    const double tau = (jd_tt - SF_J2000) / SF_MILLENNIUM;
    const double x = (jd_tt - SF_J2000) / SF_SCALE_DAYS;
    sf_time_t T;
    sf_time_prep(jd_tt, &T);

    /* 朔只看两个天体的经度差。共同的岁差/章动在差里消掉，没必要像通用位置
     * 路径那样各做一次 6×17 阶帧投影；直接在同一原生黄道系相减。16+16 项
     * 配三次常速修正，完整窗口最坏仍小于 20 分钟保护带。 */
    const double earth_l = sf_earth_coord(
        SF_EARTH_L_N, SF_EARTH_L, SF_EARTH_L_G, SF_EARTH_L_NG,
        SF_EARTH_L_PC[SF_EARTH_L_BUD_16], &T, tau);
    double moon_l = sf_moon_coord(
        SF_MOON_L_N, SF_MOON_L, SF_MOON_L_G, SF_MOON_L_NG,
        SF_MOON_L_PC[SF_MOON_L_BUD_16], &T, x);
    moon_l += sf_poly(SF_MOON_W1, SF_MOON_W1_N, x);
    return sf_wrap(moon_l - earth_l - M_PI
                 + SF_LUNAR_ABERR + SF_ABERRATION);
}

double sf_elongation(double jd)
{
    /* 地球那一路与 sf_sun_apparent_longitude 用同一套预算，保证两者自洽 */
    return sf_elongation_budget(jd, SF_MOON_L_BUD_FULL, SF_DEF_L_BUD,
                                SF_MOON_B_BUD_FULL, SF_DEF_B_BUD, SF_DEF_R_BUD);
}

const char *const SF_SOLAR_TERM_NAMES[24] = {
    "春分", "清明", "谷雨", "立夏", "小满", "芒种",
    "夏至", "小暑", "大暑", "立秋", "处暑", "白露",
    "秋分", "寒露", "霜降", "立冬", "小雪", "大雪",
    "冬至", "小寒", "大寒", "立春", "雨水", "惊蛰",
};
