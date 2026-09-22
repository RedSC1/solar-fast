/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * solar_fast.h —— 可移植的太阳位置 + 气朔求解（定点内层 / double 外层）
 *
 * 算法来源
 *   taiyin-lite 的快速气朔管线（event-fast-values.js / event-rates.js /
 *   calendar-events.js），经 fastlite_c/fl_qishuo.c 移植验证，内层改用
 *   fastlite_c/qfix 的 s3 定点方案（QEMU 相对提速 18.53×，精度代价
 *   0.0069″，flash 省 19.1 KB）。
 *
 * 时间尺度
 *   全部按 JD(TT)。UT 换算由调用方负责，本模块不涉及 ΔT。
 *
 * 有效范围
 *   ±8000 年（τ = ±8，|jd − J2000| ≤ 2922000 天）。该窗口为全库共用：
 *   级数排序窗口、速率多项式、帧投影表、sf_time_prep 的 x 钳位均按它定义。
 *   注意：该窗口只用于排序/选系数，不是返回 NAN 的判据 —— 出窗口即外推，
 *   求解器仍返回值（与上游一致）。
 *   范围外精度不属于上述有效范围保证。
 *
 * 岁差口径
 *   运行时不求值 Vondrák 2011 原公式。SF_FRAME_PROJ 是该原模型
 *   （8 黄道周期项 + 14 赤道周期项 + PA/QA/XA/YA + IAU2006 平交角）经离线
 *   Chebyshev 拟合得到的多项式替身，对外应写成
 *   "Vondrák 2011-derived polynomial surrogate over ±8000 years"。
 *   实测替身 vs 原公式黄经最大差 1.04e-6″。
 *   章动不同：那里是真截断（77 项全量），因此可查表全量化。
 *
 * 精度
 *   默认档 = EarthL 386 项 / EarthB 50 项 / EarthR 192 项 / 章动 77 项（全量）。
 *   对 DE441 真值（J2000 ± 1000 年逐日 730501 点）：
 *     几何黄经 0.0153″ rms / 0.078″ max   （几何口径无章动，与章动档无关）
 *     视黄经   0.0267″ rms / 0.136″ max
 *     赤经     0.0261″ rms / 0.126″ max
 *     赤纬     0.0235″ rms / 0.107″ max
 *     日地距离 15.2 km rms / 90.5 km max
 *   气朔对 DE441（1900–2100）：
 *     节气 rms 0.267 s / max 0.988 s   朔 rms 0.190 s / max 0.888 s
 *   远端（taiyin-ephemeris 直读 DE441，−6000..+10000 年每 25 年，641 点）：
 *     几何黄经 0.119″ rms / 0.565″ max，距离 24 km rms；最远的 ±8000 年
 *     仍在 0.19″ 量级。
 *   独立裁判 JPL Horizons（视黄经口径，−2000..+9000）：≤16″，
 *     多出的部分是章动 + 光行差修正链的代价，不是几何量。
 *   README.md 列有 DE441 精度摘要。
 *
 * 可移植性
 *   纯 C99 + libm。无 ESP-IDF、无动态内存、无全局可变状态（表全为
 *   static const），可直接加入 MCU 工程。不得使用 float：实验证明系数表
 *   存 float32 会固定损失 18.5″ rms，与项数无关。
 * ================================================================== */
#ifndef SOLAR_FAST_H
#define SOLAR_FAST_H

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 预算档位
 *
 * 值是 sf_data.h 里 *_PC 表的行索引。每行存「第 k 个幂次块保留前几项」，
 * 因此行和即实际项数 —— 实测（离线探针 budget_counts.c，未随库发布）各表
 * 每一行的和都正好等于标签上的数：
 *
 *     EARTH_L  3 7 16 32 48 59 64 96 129 192 386
 *     EARTH_B  3 7 16 32 48 50
 *     EARTH_R  3 7 16 32 48 59 64 96 129 192 475
 *     MOON_L   3 7 16 32 48 59 64 96 129 192 639
 *     MOON_B   3 7 16 32 48 59 64 96 129 192 277
 *     MOON_R   3 7 16 32 48 59 64 96 129 192 325
 *
 * 注意：不得在表外手工推算项数，项数以离线探针实测为准。
 *
 * 注意：各张表的第二维不同（10 / 7 / 12 / 3），手工遍历时不得拉平成一个
 * 指针加固定步长 —— MOON_R 是 [12][3]，按 12 走会读到相邻行的数据，读出的
 * 是看似合理的整数，不会崩溃也不报警。
 *
 * 用 -1（即 *_BUD_FULL）表示该坐标不截断。
 * ------------------------------------------------------------------ */
enum {
    SF_EARTH_L_BUD_3 = 0, SF_EARTH_L_BUD_7, SF_EARTH_L_BUD_16, SF_EARTH_L_BUD_32,
    SF_EARTH_L_BUD_48, SF_EARTH_L_BUD_59, SF_EARTH_L_BUD_64, SF_EARTH_L_BUD_96,
    SF_EARTH_L_BUD_129, SF_EARTH_L_BUD_192, SF_EARTH_L_BUD_386,
    SF_EARTH_L_BUD_FULL,          /* = 386 项，走 NULL 路径 */
};

enum {
    SF_EARTH_B_BUD_3 = 0, SF_EARTH_B_BUD_7, SF_EARTH_B_BUD_16, SF_EARTH_B_BUD_32,
    SF_EARTH_B_BUD_48, SF_EARTH_B_BUD_50,
    SF_EARTH_B_BUD_FULL,          /* = 50 项 */
};

enum {
    SF_EARTH_R_BUD_3 = 0, SF_EARTH_R_BUD_7, SF_EARTH_R_BUD_16, SF_EARTH_R_BUD_32,
    SF_EARTH_R_BUD_48, SF_EARTH_R_BUD_59, SF_EARTH_R_BUD_64, SF_EARTH_R_BUD_96,
    SF_EARTH_R_BUD_129, SF_EARTH_R_BUD_192, SF_EARTH_R_BUD_475,
    SF_EARTH_R_BUD_FULL,          /* = 475 项 */
};

enum {
    SF_MOON_L_BUD_3 = 0, SF_MOON_L_BUD_7, SF_MOON_L_BUD_16, SF_MOON_L_BUD_32,
    SF_MOON_L_BUD_48, SF_MOON_L_BUD_59, SF_MOON_L_BUD_64, SF_MOON_L_BUD_96,
    SF_MOON_L_BUD_129, SF_MOON_L_BUD_192, SF_MOON_L_BUD_639,
    SF_MOON_L_BUD_FULL,           /* = 639 项 */
};

/* 月黄纬 B，原作全量 277 项。
 *
 * 注意：不得把 B 压到 10 项。10 项对定朔够用（β 只通过帧投影的 tanβ 项影响
 * 框架黄经，是一阶小量），但对月出月落不够 —— 出没直接用 β：
 *     10 项 → β rms 32.7″ / 最大 146″ → 出没时刻差 2.3 s，最坏上百秒
 * 而 1″ 赤纬 ≈ 0.07 s 出没时刻（月球升高约 15″/s）。故默认全量。
 * 代价：277 项 × 36 B ≈ 9.7 KB rodata，求值从 2.47 µs 涨到约 3.5 µs。 */
enum {
    SF_MOON_B_BUD_3 = 0, SF_MOON_B_BUD_7, SF_MOON_B_BUD_16, SF_MOON_B_BUD_32,
    SF_MOON_B_BUD_48, SF_MOON_B_BUD_59, SF_MOON_B_BUD_64, SF_MOON_B_BUD_96,
    SF_MOON_B_BUD_129, SF_MOON_B_BUD_192, SF_MOON_B_BUD_277,
    SF_MOON_B_BUD_FULL,           /* = 277 项（原作全量） */
};

/* 月心距 R，km，原作全量 325 项。定朔只需黄经，但月球地心视差约 57′
 * （比蒙气差 34′ 还大），没有距离无法做月出，故导出整条 R。
 * 代价：325 项 × 36 B ≈ 11.4 KB rodata。 */
enum {
    SF_MOON_R_BUD_3 = 0, SF_MOON_R_BUD_7, SF_MOON_R_BUD_16, SF_MOON_R_BUD_32,
    SF_MOON_R_BUD_48, SF_MOON_R_BUD_59, SF_MOON_R_BUD_64, SF_MOON_R_BUD_96,
    SF_MOON_R_BUD_129, SF_MOON_R_BUD_192, SF_MOON_R_BUD_325,
    SF_MOON_R_BUD_FULL,           /* = 325 项（原作全量） */
};

/* ------------------------------------------------------------------
 * 档位
 *
 * 表整套编入，并已按贡献名次做过组内重排（离线探针 rank_series.mjs，
 * 未随库发布，逆向贪心）。因此「取前 N 项」即该 N 下的最优子集，档位天然
 * 嵌套；上游手调的「每幂次配额」语义依赖表内顺序，一经重排即整排失效。
 *
 * 换档是纯运行期行为：不改变 flash 占用，只改速度与精度。ESP32-S3 上这些
 * rodata 约 78 KB，占 8 MB flash 的 1%；真正稀缺的是周期数（无硬件 FP64）。
 *
 * 注意：名次按库的完整有效窗口（τ = ±8，即 ±8000 年）排序，不是只按 J2000
 * 附近。EARTH_R 的 4 次幂项为 A·τ⁴，τ=8 时 A=2.68e-3 需贡献 11 AU，靠全量
 * 项的相消才压回 1 AU；只按 ±1000 年排的名次会剔掉这些相消项，使截断档在
 * 远端使日地距离接近 0；视黄经中的 −20.49″/R 项随后产生约 6.4° 的偏差。
 *
 * 两条截断预算的实测结论：
 *   1) 距离要准就得给 R 加项：RANK:96 → 46/64 km（近/远），RANK:192 →
 *      15/24 km，R475 → 2.8/13 km。R 只通过 20.49″/R 的光行差项影响经度
 *      （1.2e-6 的相对误差对应 2.5e-5″），只求节气则不必付这个代价。
 *   2) 黄纬可以砍，但要按贡献砍：上游「每幂次配额」的 B7 给 4.25″ 赤纬
 *      误差，名次档的 B7 给 0.185″（差 23 倍）—— 上游把 5 个名额浪费在
 *      max|A| 只有 2.8e-6 的幂 0 组上。
 *
 * 1″ ≈ 24 s 的节气时刻误差，可用于把位置误差折算成时刻误差。
 * ------------------------------------------------------------------ */
#define SF_TIER_ACCURATE_L_BUD SF_EARTH_L_BUD_FULL    /* 386 项 */
#define SF_TIER_ACCURATE_B_BUD SF_EARTH_B_BUD_FULL    /* 50 项 */
#define SF_TIER_ACCURATE_R_BUD SF_EARTH_R_BUD_192     /* 192 项 */

/* 对齐 NREL SPA 的项数：L 129 / B 7 / R 59。
 * 注意：此处只是「项数相同」，两边的级数来源与参考系都不同 —— 同项数下
 * 本库的级数明显更好（实验实测 129 项时 0.060″ vs SPA 1.745″）。 */
#define SF_TIER_SPA_L_BUD SF_EARTH_L_BUD_129
#define SF_TIER_SPA_B_BUD SF_EARTH_B_BUD_7
#define SF_TIER_SPA_R_BUD SF_EARTH_R_BUD_59

#ifndef SF_DEF_L_BUD
#define SF_DEF_L_BUD SF_TIER_ACCURATE_L_BUD
#endif
#ifndef SF_DEF_B_BUD
#define SF_DEF_B_BUD SF_TIER_ACCURATE_B_BUD
#endif
#ifndef SF_DEF_R_BUD
#define SF_DEF_R_BUD SF_TIER_ACCURATE_R_BUD
#endif
/* 月心距档位，默认全量。注意这是月球的 R（km），与上面
 * SF_DEF_R_BUD（日地距离，AU）不是一回事。
 * 往下砍的代价（对上游全量，出没时刻 rms）：RANK:129 → 0.6 s，
 * RANK:96 → 1.0 s。全量是 0。 */
#ifndef SF_DEF_MOON_R_BUD
#define SF_DEF_MOON_R_BUD SF_MOON_R_BUD_FULL
#endif
#ifndef SF_DEF_MOON_L_BUD
#define SF_DEF_MOON_L_BUD SF_MOON_L_BUD_FULL
#endif
/* 章动项数。IAU2000B 共 77 项，默认全量。
 *
 * 内层全为整数（int64 相位累加 + Q31 查表 + int64 求和，出循环才转 double），
 * 每项成本仅 0.37 µs（ESP32-S3/QEMU 实测）：
 *
 *     libm 参照   20 项 227.2 µs   77 项 865.6 µs
 *     全整数查表  20 项  19.5 µs   77 项  28.9 µs
 *
 * 即全量 77 项比 20 项快 7.9 倍，20 → 77 的增量仅 +9.4 µs。截断无收益：
 * 77 项对 DE441 的节气 rms 为 0.2142 s，20 项为 0.2646 s。
 *
 * 上游 fl_qishuo.c / JS fastSolarLongitude 硬写 10 项，本库有意偏离，因此
 * 不与 fl_qishuo.c 逐位一致；该基线的章动本身有误。 */
#ifndef SF_DEF_NUT
#define SF_DEF_NUT 77
#endif

/* ------------------------------------------------------------------
 * 太阳位置（of-date 视位置）
 * ------------------------------------------------------------------ */

/* 视黄经，弧度，缠绕到 (-π, π]。
 * 注意：缠绕之后又加了章动与光行差，所以结果可能略微越出 (-π, π]。 */
double sf_sun_apparent_longitude(double jd_tt);

/* 几何黄经，弧度，缠绕到 (-π, π]。
 *
 * = 帧投影到「平黄道与平春分点 of-date」之后的太阳方向，**不加章动、
 * 不做光行差、不做光行时修正**。这是和 DE441 直接比的那个量
 * （真值 CSV 的 ty_geom_deg 列）。
 *
 * 与视黄经的关系：apparent = geometric + Δψ − 20.4898″/R。
 * 两者都受 B 档影响（β 经帧投影的 tanβ 项进经度），R 档对几何黄经
 * **完全无影响**（R 只通过光行差项进视黄经）。 */
double sf_sun_geometric_longitude(double jd_tt);

/* 视黄纬，弧度（太阳的 β 极小，约 1″ 量级） */
double sf_sun_apparent_latitude(double jd_tt);

/* 日地距离，AU */
double sf_sun_distance(double jd_tt);

/* 视赤经 / 视赤纬，弧度。ra ∈ [0, 2π)。任一指针可为 NULL。 */
void sf_sun_ra_dec(double jd_tt, double *ra, double *dec);

/* 同上，但只求值一次地球级数就同时给出距离（sf_sun_ra_dec + sf_sun_distance
 * 会把同一批 911 项算三遍）。三者数值与那两个函数一致。 */
void sf_sun_ra_dec_dist(double jd_tt, double *ra, double *dec, double *dist_au);

/* 日月视距角，弧度，缠绕到 (-π, π]。朔 = 0，望 = ±π */
double sf_elongation(double jd_tt);

/* ------------------------------------------------------------------
 * 月球位置
 *
 * 月球需 L、B、R 三个坐标。B 默认全量（见上）。R 是月出月落的前提：
 * 月球地心视差 ~57′ 比蒙气差还大，没有距离就无法做站心改正。
 *
 * 月心距 R 的档位表顶档 138 项，见上面 SF_MOON_R_BUD_*。
 * ------------------------------------------------------------------ */

/* 地心月心距，km。范围约 356400（近地）…406700（远地）。
 *
 * 注意：单位是 km，不是 AU —— 与 sf_sun_distance 不同。上游 ELP/MPP02 的
 * R 序列原生就是 km，改单位只引入一次没必要的乘除。 */
double sf_moon_distance(double jd_tt);

/* 同上，指定档位 */
double sf_moon_distance_budget(double jd_tt, int r_bud);

/* 地心月球的视赤经 / 视赤纬（弧度）与月心距（km）。任一指针可为 NULL。
 *
 * 链与太阳同构：帧投影 → 几何 of-date (λ,β) → +章动Δψ +月球光行差 → 赤道。
 *
 * 注意：月球光行差为 `SF_LUNAR_ABERR × (DM0/d)`，带距离 —— 行星光行差
 * `−τ·λ̇` 在地心开普勒近似下严格 ∝ 1/d。上游写死常数，差 0.028″ rms。
 * 详见 sf_data.h。传低 R 档会使该距离变粗，进而影响光行差（RANK:3 → 残差
 * 0.006″，RANK:7 起与全量同值）。
 *
 * 本组为地心坐标；月出月落要站心，见 sf_rise.h 的 sf_moon_rise_set()。 */
void sf_moon_ra_dec(double jd_tt, double *ra, double *dec, double *dist_km);
void sf_moon_ra_dec_budget(double jd_tt, double *ra, double *dec, double *dist_km,
                           int l_bud, int b_bud, int r_bud);

/* ------------------------------------------------------------------
 * 章动与交角（恒星时 / 日出日落要用）
 *
 * Δψ 与 Δε 在 RA/Dec 路径中已算出（原以 `(void)dpsi` 丢弃）。此处暴露给
 * sf_rise.c 使用，不额外增加计算。
 * ------------------------------------------------------------------ */

/* IAU 2000B 章动。terms = 用多少项（全量 77）；负值或超界会被夹住。
 * dpsi = 黄经章动 Δψ，deps = 交角章动 Δε，单位弧度。任一指针可为 NULL。 */
void sf_nutation_iau2000b(double jd_tt, int terms, double *dpsi, double *deps);

/* libm 参照实现，不参与计算路径，仅供验证对照。
 *
 * 正常实现（见上）内层全为整数：相位 int64 累加 + Q31 查表 + int64 求和，
 * 出循环才转 double 乘时间多项式 —— 与级数求值同一套路。保留 libm 版作为
 * 独立参照物。 */
void sf_nutation_iau2000b_dbl(double jd_tt, int terms, double *dpsi, double *deps);

/* IAU 2006 平黄赤交角 ε_mean，弧度。真交角 = 它 + Δε。 */
double sf_mean_obliquity(double jd_tt);

/* 显式指定预算的版本（诊断 / 复现精度表用）。
 * 传 -1 表示该坐标用全量。 */
double sf_sun_apparent_longitude_budget(double jd_tt, int l_bud, int nut_terms,
                                        int b_bud, int r_bud);
double sf_sun_geometric_longitude_budget(double jd_tt, int l_bud, int b_bud, int r_bud);
double sf_sun_apparent_latitude_budget(double jd_tt, int l_bud, int nut_terms,
                                       int b_bud, int r_bud);
double sf_sun_distance_budget(double jd_tt, int r_bud);
void   sf_sun_ra_dec_budget(double jd_tt, double *ra, double *dec,
                            int l_bud, int nut_terms, int b_bud, int r_bud);
double sf_elongation_budget(double jd_tt, int moon_l, int earth_l,
                            int moon_b, int earth_b, int earth_r);

/* ------------------------------------------------------------------
 * 气朔快速求解
 *
 * 全部返回 JD(TT)；越界或非有限输入返回 NAN。
 *
 * 主路径为固定步数牛顿（气两段 / 朔三段），不做逐次收敛判定。外层为护栏：
 * 主路径最后一步（= 修正前还差多少，不需额外求值）超过
 * SF_EVENT_FALLBACK_STEP_DAYS 即改走带括根的兜底；兜底再失败返回 NAN，
 * 不返回貌似有效之值。
 *
 * 默认阈值下护栏从不触发，主路径与 fl_qishuo.c 逐位一致。护栏的作用只有
 * 一件事：种子落进相邻回归/朔望月、速率反号、窗口边缘时返回 NAN 而非错值。
 *
 * 注意：阈值不是精度旋钮。残差 ≈ 0.01×step，调紧它会让 84% 的事件走兜底，
 * 且残差仍卡在 0.09 s 下不去。主路径本身也未收敛（朔最多留 0.5 s），但补
 * 一段固定牛顿也无用：对 DE441 实测，节气 rms 0.8040 → 0.8034 s，朔 rms
 * 0.3069 → 0.3081 s —— 模型误差大于这部分求解器残差。
 * ------------------------------------------------------------------ */

/* 未缠绕黄经（2πk 选择第几个回归）→ JD(TT) */
double sf_solar_term_time(double longitude_rad);

/* 同上，但显式指定**第二段**的预算（诊断 / 复现精度表用）。
 * 第一段是粗定位，其档位由第二段推导：
 *   第二段档本身不细于 L96 时 → 第一段就用同一个档（= 同档两次牛顿迭代）
 *   否则                    → 一个约 1/2 项数的便宜种子
 * 默认档下第一段仍为 L96/B3/R16，默认路径逐位不变。 */
double sf_solar_term_time_budget(double longitude_rad, int l_bud, int b_bud, int r_bud);

/* 未缠绕距角（2πk 选择第几个朔望月）→ JD(TT) */
double sf_lunar_phase_time(double elongation_rad);

/* 同上，但**档可传**（朔那边的 `sf_solar_term_time_budget`）。
 * 六元组与 sf_elongation_budget 的参数一一对应；传 SF_DEF_* 就是原行为。
 * 借它做「可调精度」：往下调就是截断级数。 */
double sf_lunar_phase_time_budget(double elongation_rad, int moon_l, int earth_l,
                                  int moon_b, int earth_b, int earth_r);

/* 诊断：解出来的同时报告走了哪条路。
 *   *path = 0  定步长（正常快路径）
 *   *path = 1  括根回退（最后一步超过 SF_EVENT_FALLBACK_STEP_DAYS）
 *   *path = -1 失败（返回值一定是 NAN）
 * path 可为 NULL。上层 sf_solve_* 的返回值与这里逐位相同。 */
double sf_solar_term_time_budget_path(double longitude_rad, int l_bud, int b_bud,
                                      int r_bud, int *path);
double sf_lunar_phase_time_path(double elongation_rad, int *path);

/* 带周期选择的封装：给一个已缠绕的目标角 + 一个"大致日期"，
 * 返回离它最近的那一次。 */
double sf_solve_solar_term(double target_longitude_rad, double near_jd_tt);
double sf_solve_lunar_phase(double target_elongation_rad, double near_jd_tt);

/* 日历归日专用的低项数气朔（寿星 qi_low / so_low 的思路，但按本库的
 * -6000..10000 年窗口重新做过）：平均运动取种子，气做两次、朔做三次
 * 短方程修正。
 *
 * 这两个结果是“分钟级粗定位”，用途是判断事件离**历法采用的日界**是否足够
 * 远；若落在日界附近，调用方应再用上面的完整求解器。日界可能由历法经度
 * 决定，并不等于显示时区的当地午夜。它们不是精确事件时刻接口。
 * 范围外会自动退回完整求解器，不拿低项数模型继续外推。
 *
 * *_time_low 收展开后的目标角（与 sf_*_time 相同）；sf_solve_*_low 收缠绕
 * 目标角 + 大致日期，并负责选择最近一圈。核心无日期结果缓存。 */
double sf_solar_term_time_low(double longitude_rad);
double sf_lunar_phase_time_low(double elongation_rad);
double sf_solve_solar_term_low(double target_longitude_rad, double near_jd_tt);
double sf_solve_lunar_phase_low(double target_elongation_rad, double near_jd_tt);

double sf_solve_lunar_phase_budget(double target_elongation_rad, double near_jd_tt,
                                   int moon_l, int earth_l, int moon_b,
                                   int earth_b, int earth_r);
double sf_solve_solar_term_budget(double target_longitude_rad, double near_jd_tt,
                                  int l_bud, int b_bud, int r_bud);

/* 按公历年汇总节气与月相事件的入口是 sf_qishuo.h 的 sf_qishuo_year_run()。
 * 不得用线性种子的历元年编号代替公历年：那种编号不是 1 月 1 日–12 月 31 日。 */

/* 速率（弧度/天）。调度器内部用，单独暴露便于核对与复用。
 * 注意这是**拟合的近似斜率**，不是物理角速度。 */
double sf_solar_rate(double jd_tt);
double sf_elongation_rate(double jd_tt);

/* ------------------------------------------------------------------
 * 二十四节气名。index 0 = 春分（视黄经 0°），第 i 项对应 i·15°。
 * 顺序严格按黄经递增，不是从冬至或立春起。
 * ------------------------------------------------------------------ */
extern const char *const SF_SOLAR_TERM_NAMES[24];

#ifdef __cplusplus
}
#endif

#endif /* SOLAR_FAST_H */
