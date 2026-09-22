/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_rise.c —— 恒星时、出没、蒙气差、均时差
 *
 * 出没这一层复刻 taiyin-lite 的 solar-visibility.js / body-visibility.js，
 * 不采用 Meeus 第 15 章那套线性化解法；原因见下面「日期归属」。
 *
 * 收 JD(UT)：出没现象活在 UT 里。TT 由本库的 ΔT 补上。
 * ================================================================== */
#include <math.h>
#include <stddef.h>

#include "sf_rise.h"
#include "sf_calendar.h"    /* sf_ut_to_tt / sf_tt_to_ut */
#include "solar_fast.h"
#include "sf_internal.h"    /* SF_TWO_PI */

#define SF_DEG (M_PI / 180.0)

/* 以下两个日速率分别用于 ERA 和恒星时，不能互换；在所述比较中误差约为
 * 0.3°（≈71 s）：
 *
 *   1.00273781191135448 = IAU 2000 定义的**地球自转角 ERA** 的速率
 *                         （360.9856123°/天，从 CIO 量起，不含岁差）
 *   1.00273790935       = GMST / GAST / 地方恒星时的速率
 *                         （360.98564736°/天，含岁差）
 *
 * 两者相差 9.7438e-8 /天，正是 GMST 表达式里 4612.156534″/世纪 那一项的速率
 * （4612.156534/1296000/36525 = 9.7438e-8）。
 *
 * sf_gast() 必须用 ERA 速率：那里做完 ERA 还要**再叠**一遍岁差多项式，
 * 用 GAST 速率会重复计入岁差，使恒星时偏 +0.3°、出没时刻偏 +71 s。
 *
 * 该误差在 J2000 附近趋零（t=0 时岁差多项式为 0.0145″≈0），且正比于 t；
 * 只在 J2000 单点自检无法发现。 */
#define SF_ERA_RATE      1.00273781191135448   /* ERA：sf_gast 用 */
#define SF_SIDEREAL_RATE 1.00273790935         /* 恒星时：小时角推演用 */

/* ==================================================================
 * 日期归属
 *
 * 不使用 Meeus 15.1–15.3 的角度方程 t = (α − H0 − lst0)/rate：分子是一个
 * **角度**，而 α 与 lst0 各自折在 [0, 2π)，使 t 带任意 2πk，日期归属会落到
 * 相邻日。实测（120 组 日期×纬度×经度）：
 *
 *     日出 102/117 落在 [day0, day0+1) 之外（78 个差 1 天，24 个差 2 天）
 *     日落  66/117（12 个差 1 天，54 个差 2 天）
 *     月出/月落 0/109
 *
 * 实现改为在**显式的 UT 区间 [day_start, day_start+1) 上对
 * 「上边缘 + 蒙气差 − 地平」求根**；根的归属由区间定义，不存在挑分支的问题，
 * 这也是 JS 不采用角度方程的原因。
 * ================================================================== */

/* ------------------------------------------------------------------
 * 恒星时
 * ------------------------------------------------------------------ */
double sf_gast(double jd_ut, double jd_tt)
{
    /* 地球自转角。系数来自 IAU2000：ERA 在 J2000 起点是 0.7790572732640 圈，
     * 速率用 SF_ERA_RATE（**不是** SF_SIDEREAL_RATE，理由见文件顶部注释）。 */
    const double era = SF_TWO_PI * (0.7790572732640
                      + SF_ERA_RATE * (jd_ut - SF_J2000));

    /* IAU2006 GMST 的岁差部分，相对 ERA，单位角秒 */
    const double t = (jd_tt - SF_J2000) / SF_CENTURY;
    const double prec_arcsec = 0.014506 + t * (4612.156534 + t * (1.3915817
        + t * (-0.00000044 + t * (-0.000029956 + t * -0.0000000368))));

    /* 分点差 = Δψ·cos(ε_true)。Δψ 由 sf_sun_ra_dec 计算但不对外返回。 */
    double dpsi = 0.0, deps = 0.0;
    sf_nutation_iau2000b(jd_tt, SF_DEF_NUT, &dpsi, &deps);
    const double eps = sf_mean_obliquity(jd_tt) + deps;

    double g = era + prec_arcsec * (SF_DEG / 3600.0) + dpsi * cos(eps);
    g -= SF_TWO_PI * floor(g / SF_TWO_PI);
    return g;
}

double sf_last(double jd_ut, double jd_tt, double lon_east_deg)
{
    double v = sf_gast(jd_ut, jd_tt) + lon_east_deg * SF_DEG;
    v -= SF_TWO_PI * floor(v / SF_TWO_PI);
    return v;
}

/* ==================================================================
 * 蒙气差 —— hybrid（Bennett / Smart 混合）
 *
 * 复刻 JS solar-visibility.js 的 hybridAtmosphericRefraction：
 *     h ≤ 14°  Bennett:  1.02 / tan(h + 10.3/(h+5.11))   [角分]
 *     h ≥ 16°  Smart:    (58.276·tanz − 0.0824·tanz³)/60 [角分]
 *     14–16°   两者按 (h−14)/2 线性混合
 *     再乘气压温度因子 pressure/1010 · 283/(273+T)
 *
 * 命名：`1.02/tan(h + 10.3/(h+5.11))` 是 **Sæmundsson (1986)**；
 * Bennett (1982) 是 `1/tan(h + 7.31/(h+4.4))`。上游注释写 "Bennett"，
 * 但式子是 Sæmundsson 的。这里按式子归类，注释里两个名字都留。
 * ================================================================== */
/* 默认大气有两套，**不一样**，取自上游口径：
 *   - 蒙气差函数自身的签名默认：1010 mbar / 10 °C
 *   - 出没路径（resolveObserver / observerSettings）：1013.25 mbar / 15 °C
 * 不得将两者"统一"。 */
#define SF_ATMO_MBAR   1013.25
#define SF_ATMO_C      15.0
#define SF_REFR_MBAR   1010.0
#define SF_REFR_C      10.0

#define SF_REFRACTION_CUTOFF (-1.0 * SF_DEG)   /* 视高度 < −1° 直接返回 0 */

double sf_refraction(double apparent_alt_rad, double pressure_mbar,
                     double temperature_c)
{
    if (!isfinite(apparent_alt_rad) || !isfinite(pressure_mbar)
        || !isfinite(temperature_c)) return 0.0;

    const double tk = 273.0 + temperature_c;
    if (pressure_mbar <= 0.0 || tk <= 0.0) return 0.0;
    if (apparent_alt_rad < SF_REFRACTION_CUTOFF) return 0.0;

    const double h = apparent_alt_rad / SF_DEG;      /* 度 */
    if (h < -2.0) return 0.0;

    const double tanz = tan((90.0 - h) * SF_DEG);
    /* Smart，角分。58.276·tanz 是角秒，除以 60 得角分。 */
    const double smart = (58.276 * tanz - 0.0824 * tanz * tanz * tanz) / 60.0;
    /* Sæmundsson，角分 */
    const double bennett = 1.02 / tan((h + 10.3 / (h + 5.11)) * SF_DEG);

    double arcmin;
    if (h >= 16.0)      arcmin = smart;
    else if (h <= 14.0) arcmin = bennett;
    else {
        const double w = (h - 14.0) / 2.0;
        arcmin = bennett * (1.0 - w) + smart * w;
    }
    if (!(arcmin > 0.0)) return 0.0;

    const double scale = pressure_mbar / 1010.0 * (283.0 / tk);
    return arcmin * scale * SF_DEG / 60.0;
}

double sf_refraction_default(double apparent_alt_rad)
{
    return sf_refraction(apparent_alt_rad, SF_REFR_MBAR, SF_REFR_C);
}

/* ==================================================================
 * 站心位置
 *
 * JS 的做法是精确向量相减：把观测者在地心赤道真分点系里的位置向量
 * （WGS84 椭球 + 海拔）从天体的地心向量里减掉，再转回球坐标。
 *
 * 该向量法计入椭球扁率与海拔（视差随地心半径变，纬度上有 ±0.3% 的起伏
 * ≈ 10″ ≈ 0.7 s），也计入太阳 8.8″ 的视差。
 * ================================================================== */
#define SF_WGS84_A_M     6378137.0
#define SF_WGS84_E2      6.69437999014e-3
#define SF_AU_KM         149597870.7
#define SF_AU_M          (SF_AU_KM * 1000.0)
/* 注意：这两个数必须与 taiyin-lite 的 BODY_DISC_RADIUS_KM
 * (src/phenomena.js) 一致：太阳 696000、月球 1737.5。
 * 相对 IAU 2015 标称值 695700 / 1737.4 差 300 km / 0.1 km，
 * 折到视半径是 0.41″ / 0.05″，在出没判据里被放大成约 0.06 s 的系统偏差。 */
#define SF_SUN_RADIUS_KM 696000.0
#define SF_MOON_RADIUS_KM 1737.5

typedef struct {
    double ra, dec;        /* 弧度 */
    double dist_au;
    double radius_km;      /* 视半径用的天体半径 */
    int    ok;
} sf_topo_t;

/* 观测者在地心赤道真分点系里的位置（AU）。lon_east_deg 与 GAST 相加。 */
static void site_vector_au(double lat_deg, double lst, double height_m, double site[3])
{
    const double phi = lat_deg * SF_DEG;
    const double sp = sin(phi), cp = cos(phi);
    const double n = SF_WGS84_A_M / sqrt(1.0 - SF_WGS84_E2 * sp * sp);
    const double rho = (n + height_m) * cp / SF_AU_M;
    const double z   = (n * (1.0 - SF_WGS84_E2) + height_m) * sp / SF_AU_M;
    site[0] = rho * cos(lst);
    site[1] = rho * sin(lst);
    site[2] = z;
}

/* 天体编号 */
#define SF_BODY_SUN  0
#define SF_BODY_MOON 1

/* 地心位置 -> 站心位置。返回 0 成功。 */
/* jd_tt 与 lst 由调用方传入：一次采样里本就要用，重复计算
 * sf_ut_to_tt（含日历换算）与 sf_last（含 77 项章动）是浪费。 */
static int topocentric(int body, double jd_tt, double lst, double lat_deg,
                       double height_m, int moon_l_bud, int moon_b_bud,
                       int moon_r_bud, sf_topo_t *out)
{
    double ra = 0.0, dec = 0.0;
    double dist_au = 0.0, radius_km = 0.0;

    if (body == SF_BODY_SUN) {
        /* 用合并版：sf_sun_ra_dec + sf_sun_distance 会重复计算地球级数三遍 */
        sf_sun_ra_dec_dist(jd_tt, &ra, &dec, &dist_au);
        radius_km = SF_SUN_RADIUS_KM;
    } else {
        double km = 0.0;
        sf_moon_ra_dec_budget(jd_tt, &ra, &dec, &km,
                              moon_l_bud, moon_b_bud, moon_r_bud);
        dist_au = km / SF_AU_KM;
        radius_km = SF_MOON_RADIUS_KM;
    }
    if (!isfinite(ra) || !isfinite(dec) || !(dist_au > 0.0)) return -1;

    /* 地心向量（AU，赤道真分点系） */
    const double cd = cos(dec);
    const double p[3] = { dist_au * cd * cos(ra), dist_au * cd * sin(ra),
                          dist_au * sin(dec) };

    double site[3];
    site_vector_au(lat_deg, lst, height_m, site);

    const double v[3] = { p[0] - site[0], p[1] - site[1], p[2] - site[2] };
    const double rxy = hypot(v[0], v[1]);
    out->ra = atan2(v[1], v[0]);
    out->dec = atan2(v[2], rxy);
    out->dist_au = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    out->radius_km = radius_km;
    out->ok = 1;
    return 0;
}

/* ==================================================================
 * 判据：上边缘 + 蒙气差 − 地平 ，返回弧度。
 *
 * JS bodyRiseSetForDay 里那个 altitude(t)，逐项对应：
 *     geometricLimb = 站心几何高度 + limbSign·视半径
 *     refraction    = hybrid(geometricLimb)      ← 用**边缘的**高度求蒙气差
 *     residual      = geometricLimb + refraction − horizon
 *
 * 注意：蒙气差对**边缘**那根光线求，不是先给中心加蒙气差再取边缘；
 * 地平线附近两者差异可观。
 * ================================================================== */
typedef struct {
    int         body;
    double      lat_deg, lon_east_deg, height_m;
    int         limb_sign;        /* +1 上边缘 / 0 中心 / −1 下边缘 */
    int         refraction;
    double      pressure_mbar, temperature_c;
    double      horizon_rad;
    /* 只供 FAST 的私有副本覆盖；ACCURATE 和公开位置接口始终传全量。 */
    int         moon_l_bud, moon_b_bud, moon_r_bud;
} sf_rise_ctx_t;

/* 一次采样的全部中间量。判据要 residual，牛顿法还要 slope；
 * slope 所用的 ra/dec/lst 与 residual 相同，合并计算可避免天体级数跑两遍。 */
typedef struct {
    double ra, dec;        /* **站心**赤经赤纬，弧度 */
    double lst;            /* 地方视恒星时，弧度 */
    double alt;            /* 站心几何高度（中心），弧度 */
    double radius;         /* 视半径，弧度 */
    double residual;       /* 边缘 + 蒙气差 − 地平，弧度 */
    double slope;          /* d(residual)/dt，单位「弧度每天」 */
} sf_sample_t;

/* 折到 (−π, π] */
static double wrap_pi(double a)
{
    a = fmod(a, SF_TWO_PI);
    if (a >  M_PI) a -= SF_TWO_PI;
    if (a < -M_PI) a += SF_TWO_PI;
    return a;
}

/* 采样一次。返回 0 成功，-1 表示天体位置不可用。 */
static int event_sample(double jd_ut, const sf_rise_ctx_t *c, sf_sample_t *out)
{
    /* 采样点上百个，这两个量在此各算一次即可，下方不再重复计算 */
    const double jd_tt = sf_ut_to_tt(jd_ut);
    const double lst = sf_last(jd_ut, jd_tt, c->lon_east_deg);

    sf_topo_t t;
    if (topocentric(c->body, jd_tt, lst, c->lat_deg, c->height_m,
                    c->moon_l_bud, c->moon_b_bud, c->moon_r_bud, &t)) return -1;
    if (!t.ok || !(t.dist_au > 0.0)) return -1;

    const double phi = c->lat_deg * SF_DEG;
    const double sp = sin(phi), cp = cos(phi);
    const double H = wrap_pi(lst - t.ra);

    double s = sp * sin(t.dec) + cp * cos(t.dec) * cos(H);
    if (s > 1.0) s = 1.0;
    if (s < -1.0) s = -1.0;
    const double alt = asin(s);

    const double sinr = t.radius_km / (t.dist_au * SF_AU_KM);
    const double radius = asin(sinr > 1.0 ? 1.0 : sinr);
    const double limb = alt + c->limb_sign * radius;

    const double refr = c->refraction
        ? sf_refraction(limb, c->pressure_mbar, c->temperature_c) : 0.0;

    out->ra = t.ra;
    out->dec = t.dec;
    out->lst = lst;
    out->alt = alt;
    out->radius = radius;
    out->residual = limb + refr - c->horizon_rad;

    /* 斜率：小时角每天转 2π·SF_SIDEREAL_RATE 圈，天体自身赤经赤纬的变化
     * 被略去 —— 相对 361°/天，太阳的 1°/天是 0.3%，月球的 13°/天是 3.6%。
     * 牛顿法对斜率误差只**线性**敏感（收敛因子 ≈ 相对误差本身），3.6% 只是
     * 多迭代一两次，不改变根的位置（不动点仍解 residual = 0）。
     * 精确导数需再求一次位置并做差分，代价高。JS 的 centerSlope 同式同近似。 */
    double cos_alt = cos(alt);
    if (cos_alt < 1e-12) cos_alt = 1e-12;    /* 天体在头顶时分子也趋零 */
    double slope = -SF_TWO_PI * cp * cos(t.dec) * sin(H) / cos_alt;

    if (c->refraction) {
        /* 蒙气差对高度的链式法则，且必须对**边缘**那根光线求，与判据同口径
         * （先给中心加蒙气差再取边缘是另一回事）。步长 1e-5 与 JS 一致。 */
        const double h = 1e-5;
        const double up = alt + h + c->limb_sign * radius;
        const double dn = alt - h + c->limb_sign * radius;
        const double ea =
            (up + sf_refraction(up, c->pressure_mbar, c->temperature_c))
          - (dn + sf_refraction(dn, c->pressure_mbar, c->temperature_c));
        slope *= ea / (2.0 * h);
    }
    out->slope = slope;
    return 0;
}

static double altitude_residual(double jd_ut, const sf_rise_ctx_t *c)
{
    sf_sample_t s;
    if (event_sample(jd_ut, c, &s)) return NAN;
    return s.residual;
}

/* ==================================================================
 * 区间求根（复刻 JS event-search.js 的 searchCrossings）
 *
 * 定步长扫符号变化 + 二分。toleranceDays = 1e-8。
 * ================================================================== */
#define SF_ROOT_TOL_DAYS 1e-8
#define SF_DAY_SAMPLES   144

/* 求根/定位驻点的收敛判据，天。
 * 注意：不得取得比双精度 ULP 还小。JD ≈ 2.46e6 处的 ULP 是 4.7e-10 天，
 * 逼近到该量级时 next − t 会抖动，永远判不出「收敛」。
 * 2e-9 天 = 0.17 ms，比测试容差 0.5 s 小三个数量级。 */
#define SF_NEWTON_TOL_DAYS 2e-9

static int sign_of(double v) { return v > 0.0 ? 1 : (v < 0.0 ? -1 : 0); }

/* 在 [start,end) 上找 NAN 安全的符号变化根，最多收 max 个。
 * 返回找到的个数；根写进 roots。 */
static int find_roots(const sf_rise_ctx_t *c, double start, double end,
                      double step, double *roots, int max)
{
    int n = 0;
    if (!(step > SF_ROOT_TOL_DAYS) || !(end > start)) return 0;

    double left = start;
    double fleft = altitude_residual(left, c);
    if (!isfinite(fleft)) return 0;
    if (fleft == 0.0 && n < max) roots[n++] = left;

    const int count = (int)ceil((end - start) / step);
    for (int i = 1; i <= count; i++) {
        const double right = (start + i * step < end) ? start + i * step : end;
        const double fright = altitude_residual(right, c);
        if (!isfinite(fright)) { left = right; fleft = fright; continue; }

        if (fright == 0.0) {
            if (n < max && !(n && right - roots[n - 1] <= SF_ROOT_TOL_DAYS * 2.0))
                roots[n++] = right;
        } else if (fleft != 0.0 && sign_of(fleft) != sign_of(fright)) {
            double a = left, b = right, fa = fleft;
            for (int k = 0; k < 80 && (b - a) > SF_ROOT_TOL_DAYS; k++) {
                const double mid = a + (b - a) * 0.5;
                if (mid <= a || mid >= b) break;
                const double fm = altitude_residual(mid, c);
                if (!isfinite(fm)) break;
                if (fm == 0.0) { a = b = mid; break; }
                if (sign_of(fm) == sign_of(fa)) { a = mid; fa = fm; } else b = mid;
            }
            const double root = a + (b - a) * 0.5;
            if (n < max && !(n && root - roots[n - 1] <= SF_ROOT_TOL_DAYS * 2.0))
                roots[n++] = root;
        }
        left = right;
        fleft = fright;
        if (n >= max) break;
    }
    return n;
}

/* 三分法细化一个局部极值（JS 用 40 轮两点收缩，这里同构）
 *
 * 注意：`sign` 必须由**检出极值那三个采样点**决定（b 比 a 高 = 极大 = +1），
 * 与 JS 一致。若改为比较区间两个端点 f(a) vs f(c) 则错误：极大值处两端点
 * 谁高谁低是任意的，会有一半概率把收缩方向搞反。 */
static double refine_extremum(const sf_rise_ctx_t *c, double a, double b, int sign)
{
    double left = a, right = b;
    for (int i = 0; i < 40 && (right - left) > 1e-8; i++) {
        const double x = left + (right - left) / 3.0;
        const double y = right - (right - left) / 3.0;
        const double fx = altitude_residual(x, c) * sign;
        const double fy = altitude_residual(y, c) * sign;
        if (fx < fy) left = x; else right = y;
    }
    return (left + right) * 0.5;
}

/* ==================================================================
 * 驻点分割法 —— 求解出没的**主算法**
 *
 * 判据 residual(t) = 站心边缘几何高度 + 蒙气差 − 地平 对时间的导数是
 *
 *     d(residual)/dt = −2π·cosφ·cosδ·sin(H)/cos(alt) × 蒙气差链式因子,
 *     H = 地方视恒星时 − 站心赤经
 *
 * 后两个因子在物理区间内都不变号（cos(alt) > 0；蒙气差对高度单调增），
 * 所以**导数的零点恰好是 sin H = 0**，也就是上中天（H = 0）和下中天（H = π）。
 *
 * 于是：把窗口里的中天解出来，连同窗口两端一起排序，**相邻两点之间
 * residual 必然单调** —— 这是定理，不是「采样够密就行」的经验假设。
 * 数符号变化就是数穿越次数，漏根在数学上不可能发生。
 *
 * 相比 144 点采样网格（一天要评估几百次天体位置；月球在 ESP32-S3/QEMU 上
 * 一次 ~600 us，一天就是 400+ ms），这里只要两次中天定位 + 两三次求根，
 * 二十来次求值 —— 而且**与纬度无关**：极区不再是「回退到慢路径」，
 * 走的是同一条代码。
 * ================================================================== */

/* 一个关键点：时刻 + 该时刻的判据值 */
typedef struct {
    double t, r;
} sf_key_t;

/* H(t) − target，折到 (−π, π]。target 取 0（上中天）或 π（下中天）。 */
static double h_err(const sf_sample_t *s, double target)
{
    return wrap_pi(wrap_pi(s->lst - s->ra) - target);
}

/* 把 t 推到驻点 H = target 上，写下该处的判据值。
 * 牛顿法，dH/dt 用 86 秒的割线估 —— H 一天走 2π 圈，天体自身的赤经速率
 * （月球 13°/天）也含在里面，所以割线估出来的是真速率，两三步就收敛。
 *
 * 返回值刻意分三态，调用方必须区分：
 *    0 成功
 *    1 这个候选点**本来就不挨着**目标驻点（|H−target| 太大），跳过即可
 *   -1 挨着但没收敛 —— 这是**硬失败**，说明驻点定位不可靠。
 *     分区间的单调性靠「驻点都找到了」，漏一个就可能漏根，
 *     所以调用方必须放弃整个分割法退回网格，不能只跳过这一个。 */
#define SF_STATIONARY_SKIP 1

static int stationary_at(const sf_rise_ctx_t *c, double t, double target,
                         sf_key_t *out)
{
    sf_sample_t a, b;
    if (event_sample(t, c, &a)) return -1;

    /* 离目标驻点超过 0.6 rad（约 1.4 小时）就不是「这个」驻点的候选 */
    const double e0 = h_err(&a, target);
    if (!isfinite(e0) || fabs(e0) > 0.6) return SF_STATIONARY_SKIP;

    const double h = 1e-3;                       /* ~86 秒 */
    if (event_sample(t + h, c, &b)) return -1;
    const double rate = (h_err(&b, target) - e0) / h;
    /* H 每天至少走 1 rad 才合理；再小说明天体几乎不动或参数异常 */
    if (!isfinite(rate) || fabs(rate) < 1.0) return -1;

    double e = e0;
    for (int i = 0; i < 6; i++) {
        const double next = t - e / rate;
        if (!isfinite(next) || fabs(next - t) > 0.2) return -1;
        t = next;
        if (event_sample(t, c, &a)) return -1;
        e = h_err(&a, target);
        if (fabs(e) < 1e-10) break;
    }
    /* 收敛不到就是定位失败 —— 不能拿一个偏掉的「驻点」去分割区间，
     * 那会把单调性破坏掉，正是漏根的来源。 */
    if (fabs(e) > 1e-6) return -1;
    out->t = t;
    out->r = a.residual;
    return 0;
}

/* 注意：判据对时间的导数为
 *     −cosφ·cosδ·sin H·Ḣ + (sinφ·cosδ − cosφ·sinδ·cos H)·δ̇
 * 而 event_sample 返回的解析斜率只含第一项。太阳 δ̇ ≈ 0.4°/天可略，
 * 月球最多 6.6°/天：在极区 ∂alt/∂H → 0 时第二项反过来占主导，**连符号都能翻**
 * （实测 88°N：局部真斜率 −0.1984 rad/天，解析式给 −0.1172）。因此判据的斜率
 * 只作牛顿起步方向，升降判定一律走 crossing_is_rise 的数值差分；驻点一贴近
 * 地平线即整日交给兜底（原因见 sf_rise_set_partition 中 extrema_suspect 一节）。 */

/* 在 [lo,hi] 上求 residual 的根，两端判据值 rlo/rhi 已经算好并且异号
 * （或恰有一端为零）。牛顿配二分兜底：牛顿步出界就取中点。
 * 括根区间 [a,b] 始终保持 f(a) 与 f(b) 异号，所以一定收敛。
 * 每次迭代一次求值 —— event_sample 同时给出判据值和解析斜率，不白跑。 */
static int refine_root(const sf_rise_ctx_t *c, double lo, double rlo,
                       double hi, double rhi, double *out, double *slope_out,
                       double *bracket_out)
{
    double a = lo, fa = rlo, b = hi;
    double t = (fabs(rlo) <= fabs(rhi)) ? lo : hi;
    sf_sample_t s;
    double slope = 0.0;

    /* 40 次是给「牛顿不收敛、退化成二分」的情形留的余量：从一天的宽度二分
     * 到 SF_ROOT_TOL_DAYS 要 26 次，24 次够不着。正常情形牛顿 4~5 次就退出，
     * 这个上限只在退化时才会碰到。 */
    for (int i = 0; i < 40; i++) {
        if (event_sample(t, c, &s)) return -1;
        slope = s.slope;
        if (s.residual == 0.0) break;

        /* 注意：斜率近乎零时**不得判失败**。起点常常正是一个驻点（区间端点
         * 就是上/下中天），那里的斜率按定义为 0，此时应退化为二分。
         * 若写成 `if (!(fabs(slope) > 1e-12)) return -1;`，就会在最该二分的
         * 点上直接放弃：一个天顶高度 −66°→+33° 的普通日子都求不出根，
         * 两个区间双双 FAILED。 */
        double next = (fabs(slope) > 1e-12) ? t - s.residual / slope
                                            : 0.5 * (a + b);
        if (!(next > a) || !(next < b)) next = 0.5 * (a + b);

        /* 用**当前这一点**的符号收紧括根区间（next 还没求值，不能拿它更新） */
        if ((fa > 0.0) != (s.residual > 0.0)) b = t;
        else { a = t; fa = s.residual; }

        /* 注意：退出判据**必须同时看步长和括根宽度**，只看步长不成立。
         * 步长小有两种原因：真收敛，或**斜率不可靠**（f/slope ≈ 0，next 几乎
         * 没动）。后者会把括根区间仍宽的那个端点当答案交出。实测 89.9°S 月球
         * 某次月出：终端区间已收到 1 秒以内，因只看步长而交出区间端点，比网格
         * 偏 **0.0988 s**（网格用 1440 点加密后逐位不变）。
         *
         * 斜率不可靠的原因见下文 crossing_is_rise 一节：解析式漏了赤纬漂移项，
         * 高纬连符号都能翻。加上括根宽度这一条后，即使牛顿全程退化为二分也
         * 收敛到 SF_ROOT_TOL_DAYS。 */
        if (fabs(next - t) < SF_NEWTON_TOL_DAYS && (b - a) < SF_ROOT_TOL_DAYS) {
            t = next;
            break;
        }
        t = next;
    }
    /* 循环里最后一步算出的 next 是在**上一轮的**括根区间里夹出来的，区间收紧
     * 之后它可能落在外面。根一定在 [a,b] 里（异号不变量），故夹回区间内。 */
    if (!(t > a) || !(t < b)) t = 0.5 * (a + b);

    /* 补一次求值：拿最终 t 处的判据值和斜率。 */
    if (event_sample(t, c, &s)) return -1;
    if (bracket_out) *bracket_out = b - a;
    *out = t;
    *slope_out = s.slope;
    return 0;
}

/* 求出来的根离零不够近就算「没磨到位」。JS 用它剔除判据的**断点**
 * （蒙气差在 −1° 处硬截断，残差会跳），这里还兼作「交给兜底」的开关。 */
#define SF_REJECT_RAD (1e-4 * SF_DEG)

/* 交叉处是升是降：取 ±1e-5 天的差分定号，**不用解析斜率**，
 * 与 144 点网格那一路（up > dn）口径一致。
 *
 * 注意：解析斜率在高纬不可用 —— 它漏了赤纬漂移项，而极点附近 ∂alt/∂H → 0，
 * 该项反过来占主导，**连符号都能翻**。实测 88°N 月球因此把日落判成日出
 * （快速 2/0 vs 网格 1/1）。根的位置正确，仅分类错误。
 *
 * 返回 1 升 / 0 降 / -1 求值失败。 */
static int crossing_is_rise(const sf_rise_ctx_t *c, double t)
{
    const double up = altitude_residual(t + 1e-5, c);
    const double dn = altitude_residual(t - 1e-5, c);
    if (!isfinite(up) || !isfinite(dn)) return -1;
    return up > dn;
}

/* 判据的斜率在根上带符号：升起 > 0、落下 < 0（与 JS 用 ±1e-5 差分判定等价）。
 *
 * 返回 -1 表示「这一天分割法求不干净」，调用方应**整体交给兜底**。
 * 触发条件是「区间异号却求不出根」或「求出来了但残差超过断点阈值」。
 * **不得默默 continue 丢弃** —— 那正是漏根的来源：
 *
 *   实测 88~90°N 月球：牛顿法用的解析斜率漏了赤纬漂移项，极点附近偏 40%
 *   （局部真斜率 −0.1984 rad/天，解析式给 −0.1172，δ̇ 项贡献 0.08 rad/天），
 *   牛顿退化成线性收敛（步长比 ~0.66/次）、24 次迭代撞上限，残差停在
 *   2.87e-6 rad，刚好越过 1e-4° 阈值，一根**真实存在**的日落被当成断点
 *   剔掉，整天报 0 次穿越。
 *
 * 该情形本该由兜底接管：兜底终端区间已窄到 1 秒，斜率再不准也收敛。 */
static int roots_in_window(const sf_rise_ctx_t *c, double start, double end,
                           const sf_key_t *keys, int nkeys,
                           double *rise, int *n_rise, double *set, int *n_set)
{
    for (int i = 0; i + 1 < nkeys; i++) {
        const double lo = keys[i].t, hi = keys[i + 1].t;
        const double rlo = keys[i].r, rhi = keys[i + 1].r;
        if (!isfinite(rlo) || !isfinite(rhi)) continue;
        if (hi - lo <= SF_ROOT_TOL_DAYS) continue;

        /* 单调区间里至多一根：两端同号就没有 */
        if (rlo != 0.0 && rhi != 0.0 && ((rlo > 0.0) == (rhi > 0.0))) continue;

        double t = 0.0, slope = 0.0, bracket = 0.0;
        if (refine_root(c, lo, rlo, hi, rhi, &t, &slope, &bracket)) return -1;
        /* 注意：磨不到位也交给兜底。分割法要求「每根都定位到 SF_ROOT_TOL_DAYS
         * 以内」，才谈得上「相邻驻点之间单调」；做不到就不硬报。
         *
         * 做不到的情形：地平线附近。蒙气差对高度的链式因子在最底下可达
         * 1 + R'(h) ≈ −7.3，`event_sample` 的解析斜率据此算出的是**局部**
         * 导数，牛顿步长被该放大因子压低，区间每轮几乎不收缩 —— 40 轮
         * 撞上限时还差得远。实测（jd=2440400.5 太阳）：
         *   lat=60.0 lon=−180   分割法偏 **71 ms**，兜底与网格一致到 1e-9 天
         *   lat=64.9 lon=116.4  偏 32 ms
         *   lat=33.87 lon=116.4 偏 13 ms */
        if (bracket > SF_ROOT_TOL_DAYS) return -1;
        if (t < start || t >= end) continue;          /* 归属由区间定义 */
        if (fabs(altitude_residual(t, c)) > SF_REJECT_RAD) return -1;

        const int is_rise = crossing_is_rise(c, t);
        if (is_rise < 0) return -1;
        if (is_rise) {
            if (*n_rise < SF_RISE_MAX) rise[(*n_rise)++] = t;
        } else {
            if (*n_set < SF_RISE_MAX) set[(*n_set)++] = t;
        }
    }
    return 0;
}

/* ==================================================================
 * 廉价预分类：今天到底够不够得到地平
 *
 * 周日圆上的高度是 sin h = sinφ·sinδ + cosφ·cosδ·cos H，对 cos H 单调，
 * 所以两端可以**精确**夹住：
 *
 *     窗口内 min h ≥ φ − 90° + min δ        （cos H = −1，下中天取等）
 *     窗口内 max h ≤ 90° − min|φ − δ|        （cos H = +1，上中天取等）
 *
 * 这两个是恒等式，把 min δ、min|φ−δ| 换成「窗口内的最小值」就是严格界。
 * 拿它们跟判据的等效中心高度地平比一下，就知道是极昼、极夜、还是必然有
 * 穿越 —— 不扫描，也不牛顿。
 *
 * 注意：这不是「纬度 > 多少度」那种魔数。极昼极夜由**纬度 + 当天赤纬**共同
 * 决定：同一个 70°N，夏天极昼、冬天极夜、春秋照常升落。不得以纬度作开关。
 *
 * 注意：**δ 必须在窗口内取样，且不能只取一个点**。判据对 δ 是线性的，
 * 误差即 δ 的误差：太阳一天走 0.4° 可略，**月球一天走 6°**。若仅在上中天
 * 估计与「上中天估计 + 半日」两点取 δ，后者**落在窗口之外**（上中天估计在
 * [mid−0.5, mid+0.5] 内，加 0.5 即出界），会拿窗口外的赤纬判窗口内的极昼
 * —— 66.5°N 春分那组 fixture 即如此失败：算出 h_min = +1.8°，实际升落各一次。
 *
 * 本实现在窗口内均匀取 SF_PRECLASS_N 个点，δ 的漏量由曲率控制：
 * 月球 |d²δ/dt²| ≲ 1.5°/天²，间隔 0.25 天时 (1/8)·1.5·0.25² = 0.012°。
 * |φ−δ| 那个 V 形则**精确**处理：相邻两点的 δ 跨过 φ 就说明最小值是 0。
 * 剩下的 0.3° 盖住中天时刻估计误差与判据在低空的模型误差。
 * ================================================================== */
/* 驻点判据值离零多近就判定为「掠射」，需要精化驻点。见下面的说明。 */
#define SF_GRAZING_MARGIN  (1.0 * SF_DEG)
/* 驻点相对中天的偏移（弧度）超过此值即近似不可靠，交给兜底。 */
#define SF_EXTREMUM_SLIP   (0.05)
#define SF_PRECLASS_N     5
#define SF_PRECLASS_SLACK (0.3 * SF_DEG)

/* 判据的**等效中心高度**地平：边缘 + 蒙气差 = 地平 折到中心高度上。 */
static double horizon_eff_of(const sf_rise_ctx_t *c, double radius_rad)
{
    const double limb0 = c->horizon_rad - c->limb_sign * radius_rad;
    return limb0 - (c->refraction
        ? sf_refraction(limb0, c->pressure_mbar, c->temperature_c) : 0.0);
}

/* 成功分类返回 0 并填好 out；落在余量带里返回 -1（交给完整算法）。
 * 中点那一次的采样结果由调用方传进来复用（省一次求值）。 */
static int preclassify(const sf_rise_ctx_t *c, double start, double end,
                       const sf_sample_t *smid, sf_rise_set_t *out)
{
    const double phi = c->lat_deg * SF_DEG;
    double dec[SF_PRECLASS_N];
    double radius = 0.0;
    int crossed = 0;

    for (int i = 0; i < SF_PRECLASS_N; i++) {
        if (i == SF_PRECLASS_N / 2) {
            dec[i] = smid->dec;
            radius = smid->radius;
            continue;
        }
        double t = start + (end - start) * (double)i / (SF_PRECLASS_N - 1);
        if (t >= end) t = end - 1e-9;          /* 区间是 [start, end) */
        sf_sample_t s;
        if (event_sample(t, c, &s)) return -1;
        dec[i] = s.dec;
    }

    double min_dec = dec[0], min_abs = fabs(phi - dec[0]);
    for (int i = 1; i < SF_PRECLASS_N; i++) {
        if (dec[i] < min_dec) min_dec = dec[i];
        const double a = fabs(phi - dec[i]);
        if (a < min_abs) min_abs = a;
        /* δ 在这两点之间跨过 φ → |φ−δ| 的最小值精确是 0 */
        if ((phi - dec[i - 1]) * (phi - dec[i]) <= 0.0) crossed = 1;
    }
    if (crossed) min_abs = 0.0;

    const double h_min_lo = phi + min_dec - M_PI / 2;
    const double h_max_hi = M_PI / 2 - min_abs;
    const double h_eff = horizon_eff_of(c, radius);

    if (h_max_hi < h_eff - SF_PRECLASS_SLACK) {
        out->n_rise = 0;
        out->n_set = 0;
        out->state = SF_ALT_ALWAYS_BELOW;
        return 0;
    }
    if (h_min_lo > h_eff + SF_PRECLASS_SLACK) {
        out->n_rise = 0;
        out->n_set = 0;
        out->state = SF_ALT_ALWAYS_ABOVE;
        return 0;
    }
    return -1;
}

/* 主算法。成功返回 0；0 个穿越也是成功（state 会说明是极昼/极夜/擦边）。 */
static int sf_rise_set_partition(const sf_rise_ctx_t *c, double start, double end,
                                 sf_rise_set_t *out)
{
    const double mid = start + (end - start) * 0.5;

    sf_sample_t sm;
    if (event_sample(mid, c, &sm)) return -1;

    /* 离窗口中心最近的那次上中天。H 把天体自身的赤经速率也算进去了，
     * 所以对月球（赤经 13°/天）这个估计仍然在 ~20 分钟以内。 */
    const double t_tr0 = mid - wrap_pi(sm.lst - sm.ra)
                       / (SF_TWO_PI * SF_SIDEREAL_RATE);

    /* 明显的极昼/极夜在这里就返回，不用去定位驻点（复用 mid 那一次采样） */
    if (preclassify(c, start, end, &sm, out) == 0) return 0;

    int extrema_suspect = 0;      /* 驻点近似可能不够准，交给兜底 */

    /* 注意：前提校验 —— 这套分割法假设「驻点在中天上」，而那**要求赤纬漂移项可忽略**。
     * 把两项都写出来解一下，驻点相对中天的偏移量是
     *     ε ≈ (B ± C) / A,   A = cosφ·cosδ·Ḣ,  B = δ̇·sinφ·cosδ,  C = δ̇·cosφ·sinδ
     * A 随 cosφ 走，越靠极点越小；δ̇ 对月球最大 6.6°/天。**偏移量大到一定程度，
     * 「相邻驻点之间单调」就不成立了**，这时必须退回网格。这不是纬度魔数，
     * 量的是「这一天这套假设还成不成立」。 */
    {
        sf_sample_t s2;
        const double dt2 = 0.25;
        if (event_sample(mid + dt2, c, &s2)) return -1;
        const double phi2 = c->lat_deg * SF_DEG;
        const double ddec = (s2.dec - sm.dec) / dt2;          /* δ̇ */
        const double dra  = wrap_pi(s2.ra - sm.ra) / dt2;     /* ṙa */
        const double hdot = SF_TWO_PI * SF_SIDEREAL_RATE - dra;
        const double A = cos(phi2) * cos(sm.dec) * hdot;
        const double B = ddec * sin(phi2) * cos(sm.dec);
        const double C = ddec * cos(phi2) * sin(sm.dec);
        if (!(fabs(A) > 1e-9)) {
            extrema_suspect = 1;               /* 极点附近：中天不再是好种子 */
        } else if (fabs((B + C) / A) > SF_EXTREMUM_SLIP
                   || fabs((B - C) / A) > SF_EXTREMUM_SLIP) {
            extrema_suspect = 1;
        }
    }

    /* 候选驻点：上中天 t_tr0 + k，下中天再往前半个恒星日。
     * 一天里上/下中天各 0~1 次（最小间隔也 > 0.5 天），所以 k 取 −1..1
     * 一定覆盖得住窗口。
     *
     * 注意：候选点的**预筛范围**要留够：t_guess 与真驻点最多差 0.053 天
     * （月球 k=±1 时，赤经漂移累积），筛太紧会把窗口内刚过边界的驻点漏掉，
     * 单调性就断了。这里用 0.2 天，多出来的候选会被 `key.t` 的窗口检查丢掉。 */
    double kt[8], kr[8];
    int nk = 0;
    for (int k = -1; k <= 1; k++) {
        for (int half = 0; half < 2; half++) {
            const double t_guess = t_tr0 + (double)k + (half ? 0.5 : 0.0);
            if (t_guess < start - 0.2 || t_guess > end + 0.2) continue;
            sf_key_t key;
            const int rc = stationary_at(c, t_guess, half ? M_PI : 0.0, &key);
            if (rc == SF_STATIONARY_SKIP) continue;
            /* 硬失败：驻点定位不可靠，而单调性靠「驻点都找到了」。
             * 不能只跳过这一个，必须整个放弃分割法退回网格。 */
            if (rc != 0) return -1;
            if (key.t < start || key.t >= end) continue;      /* 只留窗口内的 */
            int dup = 0;
            for (int j = 0; j < nk; j++) if (fabs(kt[j] - key.t) < 1e-7) dup = 1;
            if (dup) continue;
            /* 插排 —— 候选点本来就近有序，nk 很小 */
            int j = nk - 1;
            while (j >= 0 && kt[j] > key.t) {
                kt[j + 1] = kt[j]; kr[j + 1] = kr[j];
                j--;
            }
            kt[j + 1] = key.t; kr[j + 1] = key.r;
            if (fabs(key.r) < SF_GRAZING_MARGIN) extrema_suspect = 1;
            nk++;
        }
    }

    /* 注意：只要有一个驻点贴近地平线，**整日都不要用分割法**，直接交给兜底。
     *
     * 分割法的正确性建立在「驻点都找到了」之上，而候选驻点是从**中天**
     * 枚举出来的（t_tr0 + k + half/2）。极区 ∂alt/∂H → 0，真驻点由 δ̇ = 0
     * 决定，能跑到离中天**半个恒星日**以外，那里根本不在候选集里；
     * 就地精化只能把已有候选推准，**造不出候选**，故不采用。
     *
     *   实测 88°N 月球 2449273.1：窗内只枚举出 1 个候选（下中天 0.585），
     *   而真正的局部极大在 0.94 附近贴着地平线上下穿。分割法在其中一段
     *   看到两端同为负（−2.069° → −0.040°）就判「无穿越」，把日出日落两根
     *   一起漏掉 —— 而该例的驻点精化是**成功返回**的，照样漏。
     *
     * 兜底（有界递归二分，见 sf_rise_set_bisect）**完全不依赖驻点**，只按
     * 斜率上界细分，此类情形仍能找到。既然它已经很便宜，「贴近地平线就
     * 整日交给它」即是划算的：
     *
     *   实测代价（主机，88~90°N，5000 天/纬度）：交给兜底后月球
     *   0.052~0.100 ms/天，而 144 点网格是 2.25~2.88 ms/天 —— 快 20~50 倍。
     *
     * 低纬只有极值真贴着地平线的日子才会走到这里（几个百分点），而那些
     * 日子本来也正是分割法最不该用的情形。 */
    if (extrema_suspect) return -1;


    /* 两端 + 驻点，排序后的相邻两点之间单调 */
    sf_key_t keys[10];
    int n = 0;
    keys[n].t = start;
    keys[n].r = altitude_residual(start, c);
    n++;
    for (int j = 0; j < nk; j++) {
        if (kt[j] <= start || kt[j] >= end) continue;
        /* 插进已排好的序列 */
        int m = n - 1;
        while (m >= 0 && keys[m].t > kt[j]) { keys[m + 1] = keys[m]; m--; }
        keys[m + 1].t = kt[j]; keys[m + 1].r = kr[j];
        n++;
    }
    keys[n].t = end;
    keys[n].r = altitude_residual(end - 1e-9, c);
    n++;

    out->n_rise = 0;
    out->n_set = 0;
    if (roots_in_window(c, start, end, keys, n, out->rise, &out->n_rise,
                        out->set, &out->n_set)) return -1;


    double minv = 1e30, maxv = -1e30;
    for (int j = 0; j < n; j++) {
        if (!isfinite(keys[j].r)) continue;
        if (keys[j].r < minv) minv = keys[j].r;
        if (keys[j].r > maxv) maxv = keys[j].r;
    }

    /* 状态判定与 JS bodyRiseSetForDay 同口径。
     * 注意：这里用**精确驻点**上的极值，而 JS 用 144 个采样点外加三分法
     * 细化的近似极值；后者只是前者的近似，故这里的「擦边」判定比 JS 更准，
     * 两者仅在 |残差| 恰好落在 1e-5 度量级的退化情形上可能选到不同标签。 */
    if (out->n_rise || out->n_set)            out->state = SF_ALT_CROSSES;
    else if (nk && fabs(minv) < 1e-5 * SF_DEG) out->state = SF_ALT_TANGENT;
    else if (minv > 0.0)                       out->state = SF_ALT_ALWAYS_ABOVE;
    else if (maxv < 0.0)                       out->state = SF_ALT_ALWAYS_BELOW;
    else                                       out->state = SF_ALT_NOT_FOUND;
    return 0;
}

/* ==================================================================
 * 兜底：**有界递归二分**，不是整天盲扫
 *
 * 结构取自 cosinekitty / Astronomy Engine 的 FindAscent。该实现整个出没求解
 * 只有这一层，因为它本身就是兜底质量：靠一个**与星历无关**的斜率上界，
 * 把「是否继续细分」变成有证明的判据，而非「采样够密就行」的经验假设。
 *
 * 盲扫 144 点在任何日子都付 144 次求值；这里只对「两端都离零够近、藏得下
 * 一对穿越」的区间花钱，其余一次求值即否掉。因此它便宜到可作**常规**
 * 兜底用的程度，兜底不再是悬崖。
 *
 * 注意：递归**不得在「两端异号」处提前收手**。掠射日的残差是「下潜一点就
 * 回来」的形状，rise/set 两根挤在同一区间；提前收手只会拿到第一根。
 * 这里一律细分到底，终止靠下面第 2 条剪枝 —— 上界乘 dt/2 随 dt 收缩，
 * 递归自行停止，不需要额外的「单调性」假设。
 * ================================================================== */

/* 种子窗口宽度。0.42 天 = 10.08 小时，上游注释写的是
 * "Nyquist-safe for 22-hour period" —— 掠射对的最短重现周期约 22 小时，
 * 采样快它一倍以上才不会整对漏掉。 */
#define SF_RISE_SET_DT      0.42

/* 1 秒。比这更短的事件由蒙气差支配，本质不可分辨（上游原话：
 * "such cases are highly uncertain due to atmospheric refraction"）。 */
#define SF_MIN_EVENT_DAYS   (1.0 / 86400.0)

/* 细分深度上限。0.42 天减半 16 次 ≈ 0.55 秒，已经越过 SF_MIN_EVENT_DAYS，
 * 所以正常情况下第 1 条剪枝先生效；这个上限只是防失控。 */
#define SF_ASCENT_MAX_DEPTH 17

/* 判据斜率的上界，弧度/天。**零次星历求值** —— 两个三角函数，只用纬度。
 *
 * 小时角一天转 360.9856°（= 360 × 恒星日速率），天体自身赤经再叠最多
 * deriv_ra，赤纬漂移最多 deriv_dec。上游那几个界是实验定的极值，照抄。 */
#define SF_SLOPE_SAFETY 1.5

static double slope_bound_rad_per_day(const sf_rise_ctx_t *c)
{
    const double latrad = c->lat_deg * SF_DEG;
    const int is_moon = (c->body == SF_BODY_MOON);
    const double dra  = is_moon ? 4.5 : 0.8;    /* 度/天 */
    const double ddec = is_moon ? 8.2 : 0.5;
    const double deg = fabs((360.0 * SF_SIDEREAL_RATE - dra) * cos(latrad))
                     + fabs(ddec * sin(latrad));
    /* 注意：本判据是「边缘高度 + 蒙气差」，比上游的「高度」多一项蒙气差，
     * 低空 dR/dh ≈ −0.2，整条链最多放大 ~1.3 倍，故乘 1.5 保险。
     * 上界应**宁可给大**：给大只是少剪几刀、多几次求值；给小了会把真有穿越
     * 的区间剪掉，即漏根。 */
    return deg * SF_DEG * SF_SLOPE_SAFETY;
}

typedef struct {
    double start, end;                       /* 归属窗口 [start, end) */
    double max_slope;                        /* 弧度/天 */
    double rise[SF_RISE_MAX], set[SF_RISE_MAX];
    int    n_rise, n_set;
    double minv, maxv;                       /* 见过的判据极值，用于无穿越时的状态 */
} sf_scan_t;

static void scan_note(sf_scan_t *sc, double r)
{
    if (!isfinite(r)) return;
    if (r < sc->minv) sc->minv = r;
    if (r > sc->maxv) sc->maxv = r;
}

/* 收一根穿越。t 已定，is_rise 由 crossing_is_rise 定（与网格同口径）。 */
static void scan_take(sf_scan_t *sc, double t, int is_rise)
{
    if (!(t >= sc->start) || !(t < sc->end)) return;     /* 归属由区间定义 */
    if (is_rise) {
        if (sc->n_rise < SF_RISE_MAX) sc->rise[sc->n_rise++] = t;
    } else {
        if (sc->n_set < SF_RISE_MAX) sc->set[sc->n_set++] = t;
    }
}

/* 终端区间（已收到 1 秒出头）上的求根：**纯二分**，不碰牛顿。
 *
 * 这里特意不用 refine_root。牛顿的步长靠 event_sample 的解析斜率，而该
 * 斜率在地平线附近偏得厉害（漏赤纬漂移项 + 蒙气差链式因子，见 crossing_is_rise
 * 一节），步长被压小、括根区间几乎不收缩。实测：终端区间明明只有 1 秒出头，
 * 走 40 轮牛顿之后仍差 **0.175 s**（70°N 月球月出，网格用 1440 点加密后
 * 逐位不变）。
 *
 * 二分与斜率无关，而且区间本来就窄 —— 从 1 秒收到 SF_ROOT_TOL_DAYS 只要
 * 14 轮，不存在收不动的问题。 */
static int bisect_root(const sf_rise_ctx_t *c, double lo, double rlo,
                       double hi, double rhi, double *out)
{
    double a = lo, b = hi, fa = rlo;
    /* 括根前提：两端异号。调用方只在 diff 为真时进来，这里兜一道底。 */
    if (!isfinite(rlo) || !isfinite(rhi) || (rlo > 0.0) == (rhi > 0.0)) return -1;
    for (int i = 0; i < 64 && (b - a) > SF_ROOT_TOL_DAYS; i++) {
        const double mid = 0.5 * (a + b);
        const double fm = altitude_residual(mid, c);
        if (!isfinite(fm)) return -1;
        if (fm == 0.0) { *out = mid; return 0; }
        if ((fa > 0.0) != (fm > 0.0)) b = mid;
        else { a = mid; fa = fm; }
    }
    *out = 0.5 * (a + b);
    return 0;
}

/* 在 [lo,hi] 里找出**所有**穿越。两端判据值 rlo/rhi 由调用方给。
 * 返回 0 成功，-1 星历不可用（退回网格）。 */
static int find_ascent(const sf_rise_ctx_t *c, sf_scan_t *sc,
                       double lo, double rlo, double hi, double rhi, int depth)
{
    if (!isfinite(rlo) || !isfinite(rhi)) return -1;
    scan_note(sc, rlo);
    scan_note(sc, rhi);

    const double dt = hi - lo;
    const int diff = (rlo < 0.0) != (rhi < 0.0);

    /* 1. 分辨率到底：认下这一段里要得着的那一根，不再细分。 */
    if (dt <= SF_MIN_EVENT_DAYS || depth >= SF_ASCENT_MAX_DEPTH) {
        if (diff) {
            double t = 0.0;
            if (bisect_root(c, lo, rlo, hi, rhi, &t)) return -1;
            /* 与分割法同一道断点判据 —— 两边口径不一致就会在断点上打架 */
            if (fabs(altitude_residual(t, c)) <= SF_REJECT_RAD) {
                const int ir = crossing_is_rise(c, t);
                if (ir < 0) return -1;
                scan_take(sc, t, ir);
            }
        } else if (depth >= SF_ASCENT_MAX_DEPTH) {
            return -1;               /* 细分到顶还说不清 —— 交给网格 */
        }
        return 0;
    }

    /* 2. Nyquist 剪枝：同号、且两端都离零足够远 ⇒ 判据**来不及**在这段时间
     * 里跑到零再回来。这是整套东西能便宜的原因，也是它唯一的正确性依据。 */
    if (!diff && fmin(fabs(rlo), fabs(rhi)) > sc->max_slope * (dt * 0.5)) return 0;

    const double mid = 0.5 * (lo + hi);
    const double rm = altitude_residual(mid, c);
    if (!isfinite(rm)) return -1;
    scan_note(sc, rm);

    if (find_ascent(c, sc, lo, rlo, mid, rm, depth + 1)) return -1;
    return find_ascent(c, sc, mid, rm, hi, rhi, depth + 1);
}

/* 兜底：把一天按种子窗口切开，每个窗口跑一次有界递归二分。 */
static int sf_rise_set_bisect(const sf_rise_ctx_t *c, double start, double end,
                              sf_rise_set_t *out)
{
    sf_scan_t sc;
    sc.start = start;
    sc.end = end;
    sc.max_slope = slope_bound_rad_per_day(c);
    sc.n_rise = 0;
    sc.n_set = 0;
    sc.minv = 1e30;
    sc.maxv = -1e30;

    const double span = end - start;
    int nwin = (int)ceil(span / SF_RISE_SET_DT);
    if (nwin < 1) nwin = 1;

    double t0 = start;
    double r0 = altitude_residual(t0, c);
    if (!isfinite(r0)) return -1;
    scan_note(&sc, r0);

    for (int i = 0; i < nwin; i++) {
        const int last = (i + 1 == nwin);
        const double t1 = last ? end : start + span * (double)(i + 1) / (double)nwin;
        const double te = last ? end - 1e-9 : t1;      /* 区间是 [start, end) */
        const double r1 = altitude_residual(te, c);
        if (!isfinite(r1)) return -1;
        if (find_ascent(c, &sc, t0, r0, te, r1, 0)) return -1;
        t0 = te;
        r0 = r1;
    }

    out->n_rise = sc.n_rise;
    out->n_set = sc.n_set;
    for (int i = 0; i < sc.n_rise; i++) out->rise[i] = sc.rise[i];
    for (int i = 0; i < sc.n_set;  i++) out->set[i]  = sc.set[i];

    /* 状态判定与 JS bodyRiseSetForDay 同口径。没穿越时的极值取自扫描过程中
     * 见过的点 —— 剪枝保证了「被剪掉的区间里没有穿越」，所以符号判得住。 */
    if (sc.n_rise || sc.n_set)              out->state = SF_ALT_CROSSES;
    else if (fabs(sc.minv) < 1e-5 * SF_DEG) out->state = SF_ALT_TANGENT;
    else if (sc.minv > 0.0)                 out->state = SF_ALT_ALWAYS_ABOVE;
    else if (sc.maxv < 0.0)                 out->state = SF_ALT_ALWAYS_BELOW;
    else                                    out->state = SF_ALT_NOT_FOUND;
    return 0;
}

/* ==================================================================
 * FAST：9 点缓变日周三角拟合
 *
 * 一天内 H(t) 近似匀速，而 α/δ/距离只缓慢漂移，因此 sin(站心高度) 是
 * 「缓变系数 × sin/cos(日周角)」。用二次缓变系数展开，共 9 项：
 *   1, x, x², cos wx, sin wx, x cos wx, x sin wx, x² cos wx, x² sin wx
 * x 以窗口中点为 0，范围 [-0.5,0.5] 天。
 * ================================================================== */
#define SF_FAST_FIT_N       9
#define SF_FAST_SCAN_N    144
#define SF_FAST_GUARD_RAD  (0.2 * SF_DEG)
#define SF_FAST_MIN_SLOPE  0.5             /* rad/day */
#define SF_FAST_EDGE_DAYS  (120.0 / 86400.0)

/* 月球 FAST 的位置预算。只影响这 15 次拟合/校正采样；ACCURATE、兜底和
 * sf_moon_ra_dec() 仍是全量。宏可由精度阶梯测试在编译时覆盖。 */
#ifndef SF_FAST_MOON_L_BUD
#define SF_FAST_MOON_L_BUD SF_MOON_L_BUD_129
#endif
#ifndef SF_FAST_MOON_B_BUD
#define SF_FAST_MOON_B_BUD SF_MOON_B_BUD_64
#endif
#ifndef SF_FAST_MOON_R_BUD
#define SF_FAST_MOON_R_BUD SF_MOON_R_BUD_129
#endif
#ifndef SF_FAST_MOON_FULL_CORRECTION
#define SF_FAST_MOON_FULL_CORRECTION 0
#endif

static void fast_basis(double x, double v[SF_FAST_FIT_N])
{
    const double w = SF_TWO_PI * SF_SIDEREAL_RATE;
    const double co = cos(w * x), si = sin(w * x);
    const double x2 = x * x;
    v[0] = 1.0; v[1] = x;  v[2] = x2;
    v[3] = co;  v[4] = si;
    v[5] = x * co;  v[6] = x * si;
    v[7] = x2 * co; v[8] = x2 * si;
}

static double fast_eval(double x, const double coef[SF_FAST_FIT_N])
{
    double v[SF_FAST_FIT_N], y = 0.0;
    fast_basis(x, v);
    for (int i = 0; i < SF_FAST_FIT_N; i++) y += coef[i] * v[i];
    return y;
}

static double fast_deriv(double x, const double coef[SF_FAST_FIT_N])
{
    const double h = 1e-5;
    return (fast_eval(x + h, coef) - fast_eval(x - h, coef)) / (2.0 * h);
}

/* 9×9 带主元高斯消元。矩阵很小，代价相对一次星历求值可以忽略。 */
static int fast_solve(double a[SF_FAST_FIT_N][SF_FAST_FIT_N + 1],
                      double coef[SF_FAST_FIT_N])
{
    for (int i = 0; i < SF_FAST_FIT_N; i++) {
        int q = i;
        for (int r = i + 1; r < SF_FAST_FIT_N; r++)
            if (fabs(a[r][i]) > fabs(a[q][i])) q = r;
        if (!(fabs(a[q][i]) > 1e-13)) return -1;
        if (q != i) {
            for (int j = i; j <= SF_FAST_FIT_N; j++) {
                const double z = a[i][j]; a[i][j] = a[q][j]; a[q][j] = z;
            }
        }
        const double z = a[i][i];
        for (int j = i; j <= SF_FAST_FIT_N; j++) a[i][j] /= z;
        for (int r = 0; r < SF_FAST_FIT_N; r++) {
            if (r == i) continue;
            const double m = a[r][i];
            for (int j = i; j <= SF_FAST_FIT_N; j++) a[r][j] -= m * a[i][j];
        }
    }
    for (int i = 0; i < SF_FAST_FIT_N; i++) coef[i] = a[i][SF_FAST_FIT_N];
    return 0;
}

/* 把「边缘 + 蒙气差 = 地平」反解成固定的几何边缘高度。这个方程只含
 * 廉价标量函数；反解一次后，昂贵的天体曲线保持平滑，不再带蒙气差的陡导数。 */
static int fast_limb_target(const sf_rise_ctx_t *c, double *target)
{
    if (!c->refraction) { *target = c->horizon_rad; return 0; }
    double a = -89.9 * SF_DEG, b = 89.9 * SF_DEG;
    double fa = a + sf_refraction(a, c->pressure_mbar, c->temperature_c)
              - c->horizon_rad;
    double fb = b + sf_refraction(b, c->pressure_mbar, c->temperature_c)
              - c->horizon_rad;
    if (!isfinite(fa) || !isfinite(fb) || (fa > 0.0) == (fb > 0.0)) return -1;
    for (int i = 0; i < 56; i++) {
        const double m = 0.5 * (a + b);
        const double fm = m + sf_refraction(m, c->pressure_mbar, c->temperature_c)
                        - c->horizon_rad;
        if (!isfinite(fm)) return -1;
        if ((fa > 0.0) != (fm > 0.0)) { b = m; fb = fm; }
        else { a = m; fa = fm; }
    }
    (void)fb;
    *target = 0.5 * (a + b);
    return 0;
}

static int fast_geom_sample(double t, const sf_rise_ctx_t *c, double limb_target,
                            double *y)
{
    sf_sample_t s;
    if (event_sample(t, c, &s)) return -1;
    *y = sin(s.alt) - sin(limb_target - c->limb_sign * s.radius);
    return isfinite(*y) ? 0 : -1;
}

typedef struct { double t; int rise; } sf_fast_root_t;

static int fast_fit_roots(const sf_rise_ctx_t *c, double start, double end,
                          sf_rise_set_t *out)
{
    sf_rise_ctx_t fc = *c;
    if (fc.body == SF_BODY_MOON) {
        fc.moon_l_bud = SF_FAST_MOON_L_BUD;
        fc.moon_b_bud = SF_FAST_MOON_B_BUD;
        fc.moon_r_bud = SF_FAST_MOON_R_BUD;
    }
    const sf_rise_ctx_t *fitc = &fc;
    double limb_target = 0.0;
    if (fast_limb_target(c, &limb_target)) return -1;

    double a[SF_FAST_FIT_N][SF_FAST_FIT_N + 1];
    for (int i = 0; i < SF_FAST_FIT_N; i++) {
        /* 格心采样，不把窗口右端点混进 [start,end) 的归属。 */
        const double u = ((double)i + 0.5) / SF_FAST_FIT_N;
        const double x = u - 0.5;
        double v[SF_FAST_FIT_N], y = 0.0;
        fast_basis(x, v);
        if (fast_geom_sample(start + (end - start) * u, fitc, limb_target, &y)) return -1;
        for (int j = 0; j < SF_FAST_FIT_N; j++) a[i][j] = v[j];
        a[i][SF_FAST_FIT_N] = y;
    }
    double coef[SF_FAST_FIT_N];
    if (fast_solve(a, coef)) return -1;

    /* 格心拟合在窗口端点是外推，不能拿“拟合的端点”守日期归属。额外求两次
     * 真值：真实曲线在端点 0.2° 内时，一点点拟合误差就可能把根推到隔壁日，
     * 这种窗口直接交给 ACCURATE。正常一升一落因此共 9+2+4=15 次星历采样。 */
    {
        double yl = 0.0, yr = 0.0;
        if (fast_geom_sample(start, fitc, limb_target, &yl)
            || fast_geom_sample(end - 1e-9, fitc, limb_target, &yr)) return -1;
        if (fabs(yl) < SF_FAST_GUARD_RAD || fabs(yr) < SF_FAST_GUARD_RAD) return -1;
    }

    sf_fast_root_t roots[2 * SF_RISE_MAX];
    int nr = 0, suspect = 0;
    double minv = 1e30, maxv = -1e30;
    double xm = -0.5, ym = fast_eval(xm, coef);
    double x0 = -0.5 + 1.0 / SF_FAST_SCAN_N, y0 = fast_eval(x0, coef);
    if (fabs(ym) < SF_FAST_GUARD_RAD
        || fabs(fast_eval(0.5, coef)) < SF_FAST_GUARD_RAD) suspect = 1;
    if (ym < minv) minv = ym;
    if (ym > maxv) maxv = ym;
    if (y0 < minv) minv = y0;
    if (y0 > maxv) maxv = y0;

    /* 第一段 [start, start+1/N] 没有左邻点可供极值判断，但符号变化仍要收。
     * 若从 i=2 起才进入统一循环，会漏掉贴近窗口左边的根。 */
    if ((ym < 0.0) != (y0 < 0.0)) {
        double lo = xm, hi = x0, flo = ym;
        for (int j = 0; j < 36; j++) {
            const double m = 0.5 * (lo + hi), fm = fast_eval(m, coef);
            if ((flo < 0.0) != (fm < 0.0)) hi = m;
            else { lo = m; flo = fm; }
        }
        const double x = 0.5 * (lo + hi);
        const double slope = fast_deriv(x, coef);
        if (fabs(slope) < SF_FAST_MIN_SLOPE) suspect = 1;
        roots[nr].t = start + x + 0.5;
        roots[nr].rise = slope > 0.0;
        if (roots[nr].t - start < SF_FAST_EDGE_DAYS) suspect = 1;
        nr++;
    }

    for (int i = 2; i <= SF_FAST_SCAN_N; i++) {
        const double x1 = -0.5 + (double)i / SF_FAST_SCAN_N;
        const double y1 = fast_eval(x1, coef);
        if (y1 < minv) minv = y1;
        if (y1 > maxv) maxv = y1;

        /* 拟合极值贴地平：根数对微小拟合误差敏感，交给 ACCURATE。 */
        if ((y0 - ym) * (y1 - y0) <= 0.0 && fabs(y0) < SF_FAST_GUARD_RAD)
            suspect = 1;

        if ((y0 < 0.0) != (y1 < 0.0)) {
            if (nr >= 2 * SF_RISE_MAX) return -1;
            double lo = x0, hi = x1, flo = y0;
            for (int j = 0; j < 36; j++) {
                const double m = 0.5 * (lo + hi), fm = fast_eval(m, coef);
                if ((flo < 0.0) != (fm < 0.0)) hi = m;
                else { lo = m; flo = fm; }
            }
            const double x = 0.5 * (lo + hi);
            const double slope = fast_deriv(x, coef);
            if (fabs(slope) < SF_FAST_MIN_SLOPE) suspect = 1;
            roots[nr].t = start + x + 0.5;
            roots[nr].rise = slope > 0.0;
            if (roots[nr].t - start < SF_FAST_EDGE_DAYS
                || end - roots[nr].t < SF_FAST_EDGE_DAYS) suspect = 1;
            nr++;
        }
        xm = x0; ym = y0; x0 = x1; y0 = y1;
    }
    if (suspect) return -1;

    out->n_rise = out->n_set = 0;
    for (int i = 0; i < nr; i++) {
        double t = roots[i].t;
        /* 两次位置校正；导数取拟合曲线，避开蒙气差导数。月球 FAST 的
         * 校正与锚点使用同一截断档，ACCURATE 仍走全量。 */
        for (int pass = 0; pass < 2; pass++) {
            double y = 0.0;
            const sf_rise_ctx_t *correctc =
                (SF_FAST_MOON_FULL_CORRECTION && c->body == SF_BODY_MOON) ? c : fitc;
            if (fast_geom_sample(t, correctc, limb_target, &y)) return -1;
            const double slope = fast_deriv(t - start - 0.5, coef);
            if (!(fabs(slope) >= SF_FAST_MIN_SLOPE)) return -1;
            const double dt = y / slope;
            if (!isfinite(dt) || fabs(dt) > 0.02) return -1;
            t -= dt;
            if (!(t >= start) || !(t < end)) return -1;
        }
        if (roots[i].rise) {
            if (out->n_rise >= SF_RISE_MAX) return -1;
            out->rise[out->n_rise++] = t;
        } else {
            if (out->n_set >= SF_RISE_MAX) return -1;
            out->set[out->n_set++] = t;
        }
    }

    if (nr) out->state = SF_ALT_CROSSES;
    else if (minv > 0.0) out->state = SF_ALT_ALWAYS_ABOVE;
    else if (maxv < 0.0) out->state = SF_ALT_ALWAYS_BELOW;
    else return -1;                    /* 切触/数值不明确：退回 ACCURATE */
    return 0;
}

/* ==================================================================
 * 一天的穿越点
 * ================================================================== */
int sf_rise_set_for_day(int body, double day_start_ut, double lat_deg,
                        double lon_east_deg, const sf_rise_opts_t *opts,
                        sf_rise_set_t *out)
{
    if (!out || !isfinite(day_start_ut) || !isfinite(lat_deg) || !isfinite(lon_east_deg))
        return -1;
    if (lat_deg < -90.0 || lat_deg > 90.0) return -1;
    if (lon_east_deg < -180.0 || lon_east_deg > 180.0) return -1;
    if (body != SF_BODY_SUN && body != SF_BODY_MOON) return -1;

    sf_rise_opts_t d;
    if (!opts) { sf_rise_opts_default(&d); opts = &d; }
    if (!isfinite(opts->horizon_deg) || fabs(opts->horizon_deg) > 90.0) return -1;

    sf_rise_ctx_t c;
    c.body = body;
    c.lat_deg = lat_deg;
    c.lon_east_deg = lon_east_deg;
    c.height_m = opts->height_m;
    c.limb_sign = (opts->limb == SF_LIMB_UPPER) ? 1
                : (opts->limb == SF_LIMB_LOWER) ? -1 : 0;
    c.refraction = opts->refraction;
    c.pressure_mbar = opts->pressure_mbar;
    c.temperature_c = opts->temperature_c;
    c.horizon_rad = opts->horizon_deg * SF_DEG;
    c.moon_l_bud = SF_MOON_L_BUD_FULL;
    c.moon_b_bud = SF_MOON_B_BUD_FULL;
    c.moon_r_bud = SF_MOON_R_BUD_FULL;

    out->n_rise = 0;
    out->n_set = 0;
    out->state = SF_ALT_NOT_FOUND;

    const double start = day_start_ut;
    const double step = 1.0 / SF_DAY_SAMPLES;

    /* --- 默认 FAST：9 点日周曲线拟合 + 每根两次完整校正 --- */
    if (opts->fast == SF_RISE_FAST
        && fast_fit_roots(&c, start, start + 1.0, out) == 0) return 0;

    out->n_rise = out->n_set = 0;
    out->state = SF_ALT_NOT_FOUND;

    /* --- ACCURATE 主算法：驻点分割法 ---
     *
     * 残差的驻点恰好是中天，所以解出窗口内的中天就能把一天切成若干**单调**
     * 区间，数符号变化即数穿越次数 —— 见上面 sf_rise_set_partition 的说明。
     * 一天二十来次求值，**与纬度无关**。
     *
     * 这套近似在「掠射」和「极点附近」会失效（导数里被略掉的赤纬漂移项
     * 在那里占主导），这时候驻点定位会失败 —— 失败就走下面的兜底。 */
    if (opts->fast != SF_RISE_REFERENCE_GRID
        && sf_rise_set_partition(&c, start, start + 1.0, out) == 0)
        return 0;

    /* --- 兜底：有界递归二分 ---
     *
     * 注意：**不是**整天盲扫。只对「两端都离零够近、藏得下一对穿越」的区间
     * 花钱，别的区间靠一个与星历无关的斜率上界一次否掉 —— 见上面
     * sf_rise_set_bisect 的说明。高纬退到这里也就几十次求值，不是悬崖。 */
    if (opts->fast != SF_RISE_REFERENCE_GRID) {
        if (sf_rise_set_bisect(&c, start, start + 1.0, out) == 0) return 0;
        out->n_rise = 0;
        out->n_set = 0;
        out->state = SF_ALT_NOT_FOUND;
    }

    /* --- 全网格扫描：144 采样 + 三分法细化极值 + 二分 ---
     *
     * 只留给 SF_RISE_REFERENCE_GRID，用来**逐位复现 JS bodyRiseSetForDay** 对拍；
     * 顺带当二分都失败（星历不可用）时的最后手段。一天要评估几百次天体
     * 位置（ESP32-S3/QEMU 上月球 ~400 ms），不适合作为产品路径。 */
    out->n_rise = 0;
    out->n_set = 0;

    /* 采样 + 细化极值，拼成扫描网格 */
    double times[SF_DAY_SAMPLES + 3 + 4];
    double vals[SF_DAY_SAMPLES + 3 + 4];
    int nt = 0;
    for (int i = 0; i <= SF_DAY_SAMPLES; i++) {
        const double t = start + (double)i * step;
        times[nt] = t;
        vals[nt] = altitude_residual(t, &c);
        nt++;
    }
    double minv = 1e30, maxv = -1e30;
    for (int i = 0; i < nt; i++) {
        if (isfinite(vals[i])) {
            if (vals[i] < minv) minv = vals[i];
            if (vals[i] > maxv) maxv = vals[i];
        }
    }
    int n_extrema = 0;
    double etimes[4], evals[4];
    for (int i = 1; i < SF_DAY_SAMPLES && n_extrema < 4; i++) {
        const double d1 = vals[i] - vals[i - 1];
        const double d2 = vals[i + 1] - vals[i];
        if (d1 * d2 >= 0.0) continue;
        /* 与 JS 同一口径：b 比 a 高就是极大（sign = +1） */
        const int sign = (vals[i] > vals[i - 1]) ? 1 : -1;
        const double t = refine_extremum(&c, times[i - 1], times[i + 1], sign);
        const double v = altitude_residual(t, &c);
        if (!isfinite(v)) continue;
        etimes[n_extrema] = t;
        evals[n_extrema] = v;
        n_extrema++;
        if (v < minv) minv = v;
        if (v > maxv) maxv = v;
    }
    /* 把极值并进时间序列（采样本身已排序，极值数量很少，插排即可） */
    double gt[SF_DAY_SAMPLES + 3 + 4], gv[SF_DAY_SAMPLES + 3 + 4];
    int ng = 0;
    for (int i = 0; i < nt; i++) {
        gt[ng] = times[i]; gv[ng] = vals[i]; ng++;
    }
    for (int k = 0; k < n_extrema; k++) {
        int j = ng - 1;
        while (j >= 0 && gt[j] > etimes[k]) { gt[j + 1] = gt[j]; gv[j + 1] = gv[j]; j--; }
        if (j >= 0 && fabs(gt[j] - etimes[k]) <= 1e-8) continue;   /* 与采样重合 */
        gt[j + 1] = etimes[k];
        gv[j + 1] = evals[k];
        ng++;
    }

    /* 逐区间求根，按斜率分类 */
    const double reject = 1e-4 * SF_DEG;      /* JS 的 1e-4 度 */
    for (int i = 0; i + 1 < ng; i++) {
        if (gt[i + 1] - gt[i] <= 1e-8) continue;
        double roots[8];
        const int nr = find_roots(&c, gt[i], gt[i + 1], step, roots, 8);
        for (int k = 0; k < nr; k++) {
            const double rr = roots[k];
            if (fabs(altitude_residual(rr, &c)) > reject) continue;   /* 断点 */
            const double up = altitude_residual(rr + 1e-5, &c);
            const double dn = altitude_residual(rr - 1e-5, &c);
            if (!isfinite(up) || !isfinite(dn)) continue;
            if (up > dn) {
                if (out->n_rise < SF_RISE_MAX) out->rise[out->n_rise++] = rr;
            } else {
                if (out->n_set < SF_RISE_MAX) out->set[out->n_set++] = rr;
            }
        }
    }

    if (out->n_rise || out->n_set)            out->state = SF_ALT_CROSSES;
    else if (n_extrema && fabs(minv) < 1e-5 * SF_DEG) out->state = SF_ALT_TANGENT;
    else if (minv > 0.0)                      out->state = SF_ALT_ALWAYS_ABOVE;
    else if (maxv < 0.0)                      out->state = SF_ALT_ALWAYS_BELOW;
    else                                      out->state = SF_ALT_NOT_FOUND;
    return 0;
}

int sf_sun_rise_set_for_day(double day_start_ut, double lat_deg, double lon_east_deg,
                            const sf_rise_opts_t *opts, sf_rise_set_t *out)
{
    return sf_rise_set_for_day(SF_BODY_SUN, day_start_ut, lat_deg, lon_east_deg, opts, out);
}

int sf_moon_rise_set_for_day(double day_start_ut, double lat_deg, double lon_east_deg,
                             const sf_rise_opts_t *opts, sf_rise_set_t *out)
{
    return sf_rise_set_for_day(SF_BODY_MOON, day_start_ut, lat_deg, lon_east_deg, opts, out);
}

void sf_rise_opts_default(sf_rise_opts_t *o)
{
    if (!o) return;
    o->limb = SF_LIMB_UPPER;
    o->refraction = 1;
    o->horizon_deg = 0.0;
    o->pressure_mbar = SF_ATMO_MBAR;      /* 1013.25，出没路径的默认 */
    o->temperature_c = SF_ATMO_C;         /* 15 —— 不是蒙气差函数的 10 */
    o->height_m = 0.0;
    o->fast = SF_RISE_FAST;
}

/* ==================================================================
 * 旧接口：签名不变，内部调用新实现，并把结果归一到该 UT 日
 *
 * 日期归属由显式 UT 区间定义（见前面「日期归属」一节）。
 * 蒙气差与站心视差采用精确算法后，与 JS 的差从 ~2 s rms 降到噪声级。
 * ================================================================== */
/* 旧接口的窗口：**当地（按经度）那一天的 0 点**，不是 UT 日。
 *
 * 复刻 JS computeSolarRiseSetFast 的取法：
 *     localNoon = round(center + lon/360) − lon/360
 *     window    = [localNoon − 0.5, localNoon + 0.5)
 * 以北京 2024-06-21 为例：localNoon = 04:14 UT（经度 116.4 的地方时正午），
 * 窗口 [06-20 16:14 UT, 06-21 16:14 UT) —— 当地那天的日出（06-20 20:46 UT）
 * 和日落（06-21 11:46 UT）都在里面，rise < set。
 *
 * 注意：不得用 UT 日（floor(jd+0.5)−0.5）。北京那天的日出落在 UT 日**之前**，
 * 窗口里只剩日落和第二天日出，昼长会算成负的（−11.85 h）。 */
static double local_day_start(double jd_ut, double lon_east_deg)
{
    const double lo = lon_east_deg / 360.0;
    const double local_noon = floor(jd_ut + lo + 0.5) - lo;
    return local_noon - 0.5;
}

static int legacy_pair(int body, double jd_ut, double lat_deg, double lon_east_deg,
                       const sf_rise_opts_t *opts,
                       double *out_rise_ut, double *out_set_ut)
{
    if (!out_rise_ut || !out_set_ut) return SF_RISE_ERROR;
    if (!isfinite(jd_ut) || !isfinite(lat_deg) || !isfinite(lon_east_deg))
        return SF_RISE_ERROR;

    const double day0 = local_day_start(jd_ut, lon_east_deg);
    sf_rise_set_t r;
    if (sf_rise_set_for_day(body, day0, lat_deg, lon_east_deg, opts, &r)) return SF_RISE_ERROR;

    if (r.n_rise == 0 && r.n_set == 0) {
        /* FAST / ACCURATE 已给出可靠状态，不必在极昼极夜后再扫 144 点。 */
        if (r.state == SF_ALT_ALWAYS_ABOVE) return SF_RISE_POLAR_DAY;
        if (r.state == SF_ALT_ALWAYS_BELOW) return SF_RISE_POLAR_NIGHT;

        /* 只剩切触/未找到这种返回码表达不了的退化态，再用整日采样判。 */
        sf_rise_ctx_t c;
        sf_rise_opts_t d;
        if (!opts) { sf_rise_opts_default(&d); opts = &d; }
        c.body = body; c.lat_deg = lat_deg; c.lon_east_deg = lon_east_deg;
        c.height_m = opts->height_m;
        c.limb_sign = (opts->limb == SF_LIMB_UPPER) ? 1
                    : (opts->limb == SF_LIMB_LOWER) ? -1 : 0;
        c.refraction = opts->refraction;
        c.pressure_mbar = opts->pressure_mbar;
        c.temperature_c = opts->temperature_c;
        c.horizon_rad = opts->horizon_deg * SF_DEG;
        c.moon_l_bud = SF_MOON_L_BUD_FULL;
        c.moon_b_bud = SF_MOON_B_BUD_FULL;
        c.moon_r_bud = SF_MOON_R_BUD_FULL;
        double best = -1e30;
        for (int i = 0; i <= SF_DAY_SAMPLES; i++) {
            const double v = altitude_residual(day0 + (double)i * (1.0 / SF_DAY_SAMPLES), &c);
            if (isfinite(v) && v > best) best = v;
        }
        return (best > 0.0) ? SF_RISE_POLAR_DAY : SF_RISE_POLAR_NIGHT;
    }
    if (r.n_rise == 0 || r.n_set == 0) {
        /* 只有一半（高纬掠射）：现有的返回码表达不了，按极昼/极夜近似 */
        return (r.n_rise > 0) ? SF_RISE_POLAR_DAY : SF_RISE_POLAR_NIGHT;
    }
    *out_rise_ut = r.rise[0];
    *out_set_ut  = r.set[0];
    return SF_RISE_OK;
}

int sf_sun_rise_set(double jd_ut, double lat_deg, double lon_east_deg,
                    double *out_rise_ut, double *out_set_ut)
{
    return legacy_pair(SF_BODY_SUN, jd_ut, lat_deg, lon_east_deg, NULL,
                       out_rise_ut, out_set_ut);
}

int sf_moon_rise_set(double jd_ut, double lat_deg, double lon_east_deg,
                     double *out_rise_ut, double *out_set_ut)
{
    return legacy_pair(SF_BODY_MOON, jd_ut, lat_deg, lon_east_deg, NULL,
                       out_rise_ut, out_set_ut);
}

int sf_sun_rise_set_day_length(double jd_ut, double lat_deg, double lon_east_deg,
                               double *out_rise_ut, double *out_set_ut,
                               double *out_day_length_hours)
{
    if (!out_rise_ut || !out_set_ut || !out_day_length_hours) return SF_RISE_ERROR;
    if (!isfinite(jd_ut) || !isfinite(lat_deg) || !isfinite(lon_east_deg))
        return SF_RISE_ERROR;

    const double day0 = local_day_start(jd_ut, lon_east_deg);
    sf_rise_set_t r;
    if (sf_sun_rise_set_for_day(day0, lat_deg, lon_east_deg, NULL, &r))
        return SF_RISE_ERROR;

    if (r.n_rise && r.n_set) {
        double d = fmod(r.set[0] - r.rise[0], 1.0);
        if (d < 0.0) d += 1.0;
        *out_rise_ut = r.rise[0];
        *out_set_ut = r.set[0];
        *out_day_length_hours = d * 24.0;
        return SF_RISE_OK;
    }

    /* 昼长沿用 sf_day_length_hours 的定义；返回码沿用 legacy_pair。 */
    *out_day_length_hours = (r.state == SF_ALT_ALWAYS_ABOVE) ? 24.0 : 0.0;
    if (r.n_rise || r.n_set)
        return r.n_rise ? SF_RISE_POLAR_DAY : SF_RISE_POLAR_NIGHT;
    if (r.state == SF_ALT_ALWAYS_ABOVE) return SF_RISE_POLAR_DAY;
    if (r.state == SF_ALT_ALWAYS_BELOW) return SF_RISE_POLAR_NIGHT;

    /* 极罕见的切触/未找到状态：与 legacy_pair 一样，用整日采样判极昼/极夜。 */
    sf_rise_ctx_t c;
    sf_rise_opts_t o;
    sf_rise_opts_default(&o);
    c.body = SF_BODY_SUN; c.lat_deg = lat_deg; c.lon_east_deg = lon_east_deg;
    c.height_m = o.height_m;
    c.limb_sign = 1;
    c.refraction = o.refraction;
    c.pressure_mbar = o.pressure_mbar;
    c.temperature_c = o.temperature_c;
    c.horizon_rad = o.horizon_deg * SF_DEG;
    c.moon_l_bud = SF_MOON_L_BUD_FULL;
    c.moon_b_bud = SF_MOON_B_BUD_FULL;
    c.moon_r_bud = SF_MOON_R_BUD_FULL;
    double best = -1e30;
    for (int i = 0; i <= SF_DAY_SAMPLES; i++) {
        const double v = altitude_residual(day0 + (double)i / SF_DAY_SAMPLES, &c);
        if (isfinite(v) && v > best) best = v;
    }
    if (best > 0.0) {
        *out_day_length_hours = 24.0;
        return SF_RISE_POLAR_DAY;
    }
    return SF_RISE_POLAR_NIGHT;
}

/* 自己指定几何高度角：站心中心高度（不含蒙气差、不含视半径）穿过 h0_deg。
 * 想要「民用晨昏蒙影」传 h0_deg = −6。
 *
 * 注：解的是**站心**中心高度，非地心；太阳视差 8.8″，折合时刻约 0.6 s。 */
int sf_sun_rise_set_at(double jd_ut, double lat_deg, double lon_east_deg,
                       double h0_deg, double *out_rise_ut, double *out_set_ut)
{
    sf_rise_opts_t o;
    sf_rise_opts_default(&o);
    o.limb = SF_LIMB_CENTER;
    o.refraction = 0;
    o.horizon_deg = h0_deg;
    return legacy_pair(SF_BODY_SUN, jd_ut, lat_deg, lon_east_deg, &o,
                       out_rise_ut, out_set_ut);
}

double sf_day_length_hours(double jd_ut, double lat_deg, double lon_east_deg)
{
    const double day0 = local_day_start(jd_ut, lon_east_deg);
    sf_rise_set_t r;
    if (sf_sun_rise_set_for_day(day0, lat_deg, lon_east_deg, NULL, &r)) return NAN;
    if (r.n_rise == 0 || r.n_set == 0)
        return (r.state == SF_ALT_ALWAYS_ABOVE) ? 24.0 : 0.0;
    /* 取模 1 天：高纬掠射时一天里可能有两次穿越，这一对不一定 rise < set */
    double d = fmod(r.set[0] - r.rise[0], 1.0);
    if (d < 0.0) d += 1.0;
    return d * 24.0;
}

/* ==================================================================
 * 高度 / 方位
 * ================================================================== */
double sf_sun_altitude(double jd_ut, double lat_deg, double lon_east_deg,
                       double *out_azimuth)
{
    if (!isfinite(jd_ut) || !isfinite(lat_deg) || !isfinite(lon_east_deg)) return NAN;

    /* 站心几何高度，**不含**蒙气差 —— 与 JS bodyHorizontalPosition 的
     * geometricAltitudeDeg 同口径（那个函数还有 apparentAltitudeDeg，
     * 需要就自己调 sf_refraction 加上去）。 */
    const double jd_tt = sf_ut_to_tt(jd_ut);
    const double lst = sf_last(jd_ut, jd_tt, lon_east_deg);
    sf_topo_t t;
    if (topocentric(SF_BODY_SUN, jd_tt, lst, lat_deg, 0.0,
                    SF_MOON_L_BUD_FULL, SF_MOON_B_BUD_FULL,
                    SF_MOON_R_BUD_FULL, &t)) return NAN;

    const double phi = lat_deg * SF_DEG;
    const double sp = sin(phi), cp = cos(phi);
    double H = lst - t.ra;
    const double sd = sin(t.dec), cd = cos(t.dec);
    double s = sp * sd + cp * cd * cos(H);
    if (s > 1.0) s = 1.0;
    if (s < -1.0) s = -1.0;
    const double alt = asin(s);

    if (out_azimuth) {
        double az = atan2(sin(H), cos(H) * sp - (sd / cd) * cp);
        az += M_PI;                       /* atan2 给的是自南起，转成自北起 */
        if (az < 0.0) az += SF_TWO_PI;
        if (az >= SF_TWO_PI) az -= SF_TWO_PI;
        *out_azimuth = az;
    }
    return alt;
}

/* ------------------------------------------------------------------
 * 均时差
 * ------------------------------------------------------------------ */
double sf_equation_of_time_minutes(double jd_tt)
{
    const double t = (jd_tt - SF_J2000) / SF_CENTURY;

    /* 太阳平黄经（Meeus 25.2），度。
     *
     * 注意：世纪系数是 **36000.76983**，不是 360007.6982779。太阳平黄经每年
     * 走 360°，即每世纪 36000.77°；写成 360007.7 即放大 10 倍，均时差会变成
     * 几百分钟而非几十分钟。该错误不崩溃、不报警，只给出数值上看似正常的
     * 错误结果。 */
    const double l0 = 280.46646 + t * (36000.76983 + t * 0.0003032);

    double ra = 0.0, dec = 0.0;
    sf_sun_ra_dec(jd_tt, &ra, &dec);

    double dpsi = 0.0, deps = 0.0;
    sf_nutation_iau2000b(jd_tt, SF_DEF_NUT, &dpsi, &deps);
    const double eps = sf_mean_obliquity(jd_tt) + deps;

    /* Meeus 28.3：E = L0 − 0.0057183° − α + Δψ·cos ε ，单位度 */
    double e = l0 - 0.0057183 - ra / SF_DEG + dpsi * cos(eps) / SF_DEG;

    /* 折到 ±20° 再转分钟 —— L0 涨得比分点快，不折会积累整圈 */
    e = fmod(e, 360.0);
    if (e > 180.0)  e -= 360.0;
    if (e < -180.0) e += 360.0;
    return e * 4.0;
}

double sf_mean_solar_time_hours(double jd_ut, double lon_east_deg)
{
    double h = (jd_ut + 0.5 - floor(jd_ut + 0.5)) * 24.0 + lon_east_deg / 15.0;
    h = fmod(h, 24.0);
    return h < 0.0 ? h + 24.0 : h;
}

double sf_true_solar_time_hours(double jd_ut, double lon_east_deg)
{
    double h = sf_mean_solar_time_hours(jd_ut, lon_east_deg)
             + sf_equation_of_time_minutes(sf_ut_to_tt(jd_ut)) / 60.0;
    h = fmod(h, 24.0);
    return h < 0.0 ? h + 24.0 : h;
}

/* ==================================================================
 * 太阳地面观测者全要素合算实现
 * ================================================================== */

int sf_sun_observed(double jd_ut, double lat_deg, double lon_east_deg,
                    double elevation_m, double pressure_hpa, double temp_c,
                    sf_sun_observed_t *out)
{
    if (!out) return -1;
    if (!isfinite(jd_ut) || !isfinite(lat_deg) || !isfinite(lon_east_deg)) return -1;

    const double jd_tt = sf_ut_to_tt(jd_ut);

    /* 1. 地心视位置（单次级数展开） */
    double geo_ra = 0.0, geo_dec = 0.0, geo_dist = 0.0;
    sf_sun_ra_dec_dist(jd_tt, &geo_ra, &geo_dec, &geo_dist);
    out->ra_rad      = geo_ra;
    out->dec_rad     = geo_dec;
    out->distance_au = geo_dist;

    /* 2. 恒星时与地方真恒星时 */
    const double gast = sf_gast(jd_ut, jd_tt);
    double last = gast + lon_east_deg * SF_DEG;
    last = fmod(last, SF_TWO_PI);
    if (last < 0.0) last += SF_TWO_PI;
    out->gast_rad = gast;
    out->last_rad = last;

    /* 3. 站心视位置（修正周日视差与测站高程，直接利用已算出的地心矢量） */
    const double cd = cos(geo_dec);
    const double p[3] = { geo_dist * cd * cos(geo_ra),
                          geo_dist * cd * sin(geo_ra),
                          geo_dist * sin(geo_dec) };
    double site[3];
    site_vector_au(lat_deg, last, elevation_m, site);
    const double v[3] = { p[0] - site[0], p[1] - site[1], p[2] - site[2] };
    const double rxy = hypot(v[0], v[1]);
    double topo_ra = atan2(v[1], v[0]);
    if (topo_ra < 0.0) topo_ra += SF_TWO_PI;
    const double topo_dec = atan2(v[2], rxy);
    const double topo_dist = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

    out->topo_ra_rad   = topo_ra;
    out->topo_dec_rad  = topo_dec;
    out->topo_dist_au  = topo_dist;

    /* 地方时角 H = LST - RA */
    double H = wrap_pi(last - topo_ra);
    out->hour_angle_rad = H;

    /* 4. 站心地平坐标（几何高度与方位） */
    const double phi = lat_deg * SF_DEG;
    const double sp = sin(phi), cp = cos(phi);
    const double sd = sin(topo_dec), c_dec = cos(topo_dec);
    double s = sp * sd + cp * c_dec * cos(H);
    if (s > 1.0) s = 1.0;
    if (s < -1.0) s = -1.0;
    const double alt_geo_rad = asin(s);
    out->altitude_geometric_deg = alt_geo_rad / SF_DEG;

    /* 方位角自北起顺时针 [0, 360) */
    double az = atan2(sin(H), cos(H) * sp - (sd / c_dec) * cp) + M_PI;
    if (az < 0.0) az += SF_TWO_PI;
    if (az >= SF_TWO_PI) az -= SF_TWO_PI;
    out->azimuth_deg = az / SF_DEG;

    /* 5. 蒙气差修正与视高度/视天顶角 */
    double alt_app_rad = alt_geo_rad;
    if (pressure_hpa > 0.0) {
        alt_app_rad += sf_refraction(alt_geo_rad, pressure_hpa, temp_c);
    }
    out->altitude_apparent_deg = alt_app_rad / SF_DEG;
    out->zenith_deg = 90.0 - out->altitude_apparent_deg;

    /* 6. 均时差（直接复用已求得的视赤经 geo_ra，省去整套太阳级数重算） */
    const double t_cent = (jd_tt - SF_J2000) / SF_CENTURY;
    const double l0 = 280.46646 + t_cent * (36000.76983 + t_cent * 0.0003032);
    double dpsi = 0.0, deps = 0.0;
    sf_nutation_iau2000b(jd_tt, SF_DEF_NUT, &dpsi, &deps);
    const double eps = sf_mean_obliquity(jd_tt) + deps;
    double e = l0 - 0.0057183 - geo_ra / SF_DEG + dpsi * cos(eps) / SF_DEG;
    e = fmod(e, 360.0);
    if (e > 180.0)  e -= 360.0;
    if (e < -180.0) e += 360.0;
    out->equation_of_time_min = e * 4.0;
    return 0;
}

double sf_solar_incidence_angle(double zenith_deg, double sun_azimuth_deg,
                                double slope_deg, double surface_azimuth_deg)
{
    const double z = zenith_deg * SF_DEG;
    const double s = slope_deg * SF_DEG;
    const double d_az = (sun_azimuth_deg - surface_azimuth_deg) * SF_DEG;

    double cos_i = cos(z) * cos(s) + sin(z) * sin(s) * cos(d_az);
    if (cos_i > 1.0) cos_i = 1.0;
    if (cos_i < -1.0) cos_i = -1.0;
    return acos(cos_i) / SF_DEG;
}

double sf_sun_transit(double jd_ut, double lon_east_deg)
{
    const double day0 = floor(jd_ut - 0.5) + 0.5;
    double t_ut = day0 + (12.0 - lon_east_deg / 15.0) / 24.0;
    const double e = sf_equation_of_time_minutes(sf_ut_to_tt(t_ut));
    return day0 + (12.0 - lon_east_deg / 15.0 - e / 60.0) / 24.0;
}
