/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_internal.h —— 模块内部的小工具，不对外暴露
 * ================================================================== */
#ifndef SF_INTERNAL_H
#define SF_INTERNAL_H

#include <math.h>
#include "sf_data.h"

/* ------------------------------------------------------------------
 * 光行差 / 光行时
 *
 * 两个常数本身在 sf_data.h（`SF_ABERRATION` / `SF_LUNAR_ABERR`），但说明写在
 * 这里 —— sf_data.h 由 `make tables` 生成，文件头即写着「勿手改」。重新生成
 * 会抹掉写在该处的整段说明，连同下面这个 SF_LUNAR_ABERR_REF_KM，而
 * sf_series.c 有两处要用它，缺失时编译失败。
 *
 * 两个量的来历**不一样**：
 *
 *   SF_ABERRATION     太阳。光行时与年周光行差在这一条里**本来就分不开** ——
 *                     地球在动、光要走 499 s，两个效应是同一个 −τ·ρ̇。
 *                     9.933735e-5 rad = 20.4898″，即俗称的「20.49″/R」。
 *                     用法 −SF_ABERRATION/R（R 日地距离，AU）。
 *                     注意：必须带 1/R。这一项严格 ∝ 1/R，写成常数会在
 *                     近日点差 0.35″。
 *
 *   SF_LUNAR_ABERR    月球。**只有光行时那一半，年周光行差对月亮抵消。**
 *                     视位置 = ρ(t) − τ·v_月,质心 + τ·v_地，而
 *                     v_月,质心 = v_月,地心 + v_地，代进去 τ·v_地 两项相消：
 *                         ρ_apparent = ρ(t) − τ·v_月,地心
 *                     所以月亮这里**没有** 20.49″ 那一块，也不该有 ——
 *                     月亮和地球一起绕日，年周光行差是共模的。
 *
 * 为什么月亮也 ∝ 1/d：τ = d/c，而地心里的月亮基本是开普勒轨道，λ̇ = h/d²，
 * 相乘得 h/(dc) —— **严格 ∝ 1/d**。所以它和太阳那条一样必须带距离因子。
 *
 * 上游（fl_qishuo.c / JS fast 通道）把月亮这条字面写死 `-3.4e-6`，等于只取了
 * 中位数。实测（离线探针 lt_probe.c，未随库发布；真值取 −τ·λ̇ 的中心差分，
 * h 从 0.001 到 0.2 天结果不变；跨 ±8000 年窗口也稳定）：
 *     常数      残差 0.028″ rms / 0.061″ max   折成时刻 0.051 s / 0.095 s
 *     乘 DM0/d  残差 0.003″ rms / 0.007″ max   折成时刻 0.006 s / 0.014 s
 * 两条调用路径（sf_moon_ra_dec_budget 与 sf_elongation_budget）的距离都是
 * 现成的，所以这个 9 倍是零代价的。
 *
 * 二阶项 ½τ²λ̈ 实测 0.000″（τ 只有 1.28 s），不用管。
 *
 * 注意：该 ∝1/d 写法只对月亮成立，不得套用到行星。它靠的是「地心月亮
 * 近似开普勒」这一条：λ̇ = h/d² 里的 h 是绕地球的角动量，才让 τ·λ̇ 掉成
 * h/(dc)。水星金星是绕**太阳**转的，地心向量 ρ = r_行 − r_地 的 ṙ 由
 * 「行星自己的日心速度 − 地球的日心速度」决定，跟 1/|ρ| 没有关系 ——
 * 角修正量是 |ṙ_⊥|/c，内行星能到 24″(金星)~34″(水星)，且随相位大幅变。
 * 那时候要么老老实实做 −τ·ṙ（级数解析求导：泊松级数的导数用同一张 Q31 表
 * 移 1/4 相位就能拿到，代价远小于再求一次值），要么别做。
 *
 * DM0 取 385500 而不是习用的平均距离 384400：后者留 +0.002″ 的同向偏置
 * （常数的隐含基准比 1/d 的调和平均略大），385500 把它清零。
 * ------------------------------------------------------------------ */
#define SF_LUNAR_ABERR_REF_KM 385500.0

/* 日历归日专用的短路径：只求事件方程真正消费的经度，不组装完整 L/B/R、
 * 不算通用速率。实现留在 sf_series.c，避免把级数表结构暴露成公共 API。 */
double sf_sun_event_longitude_low(double jd_tt);
double sf_lunar_event_elongation_low(double jd_tt);

/* JS 的 wrap()：把角度折叠到 (-π, π]。用在级数求解器与定气残差上。
 *
 * 注意：与 sf_wrap_radians 数学等价但浮点结果不同，不可互换 ——
 * fl_qishuo.c 里两份分开，各自用于该用处。 */
static inline double sf_wrap(double a)
{
    return a - SF_TWO_PI * floor((a + SF_TWO_PI * 0.5) / SF_TWO_PI);
}

/* JS calendar-events.js 的 wrapRadians()：atan2 版。只用于归一化目标角。 */
static inline double sf_wrap_radians(double a)
{
    return atan2(sin(a), cos(a));
}

/* JS 的 Math.round 是 floor(x + 0.5)（向 +∞），C 的 round() 在 .5 处不同 */
static inline double sf_js_round(double x)
{
    return floor(x + 0.5);
}

/* 多项式的值；系数从低次到高次 */
static inline double sf_poly(const double *a, int n, double x)
{
    double v = 0.0;
    for (int i = n - 1; i >= 0; i--) v = v * x + a[i];
    return v;
}

#endif /* SF_INTERNAL_H */
