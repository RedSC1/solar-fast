/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_calendar.c —— 农历规则层
 *
 * 实现移植自 taiyin-ephemeris/src/chinese_calendar/calendar.cpp，
 * 天文部分换成本库的 sf_solve_*。移植尽量与上游**逐语句**对应，
 * 注释因此大量引用上游行号。
 *
 * ==================================================================
 * 这一层收 JD(UT)，不是 TT
 *
 * solar_fast.h 的立场是「全部 TT，ΔT 由调用方处理」。农历不能这么办：
 * 归日是**民用日**判定，本质上活在 UT 里。所以这一层自带 ΔT，把 UT 转成
 * TT 再交给 sf_solve_*。
 * ==================================================================
 *
 * 古历归日表（sf_calendar_data.h）已接入。**搬表的正确性以两个
 * oracle SHA256 对全表 85485 条归日核验**，不止 8 条 fixture ——
 * 复现方法见 experimental/verify_calendar_data.py。
 *
 * 干支在 sf_ganzhi.c。
 * ================================================================== */
#include <math.h>
#include <string.h>

#include "sf_calendar.h"
#include "sf_calendar_data.h"   /* 古历归日表（生成） */
#include "solar_fast.h"
#include "sf_internal.h"        /* SF_TWO_PI / SF_J2000 在这条链上（sf_data.h） */

#define SF_DEG         (M_PI / 180.0)
#define SF_CHINA_OFF   (480.0 / 1440.0)   /* 东经 120°，结构日界，钉死的 */

/* 上游 chinese-calendar.js:17-19 的同名常数。它们刻意比本库的日月求解器粗 ——
 * 只用来当**种子**，落在正确的 15.2 天 / 29.5 天格子里就够了。 */
#define SF_DAYS_PER_SOLAR_TERM      15.2184
#define SF_DAYS_PER_SYNODIC_MONTH   29.5306
#define SF_DAYS_PER_TROPICAL_YEAR   365.2422

/* 一个岁的窗口上限。25 个节气 / 15 个朔 跨度都远小于这个数。 */
#define SF_ESTIMATE_SPAN            371.0

/* ------------------------------------------------------------------
 * 时间尺度 / ΔT
 *
 * 复刻 taiyin-lite/src/time.js 的 deltaTSeconds。分段（分支顺序也照抄）：
 *
 *   year <  -820          长期抛物线  -20 + 32u^2,  u = (year-1820)/100
 *   -820 <= year < -720   earlyJoin：抛物线 <-> S15 的一次 Hermite 桥接
 *   -720 <= year < 1953   S15 三次样条（Stephenson-Morrison-Hohenkerk 2016）
 *   1953 <= year < 2027   逐年 ΔT 表，Catmull-Rom（等价于三次 Hermite）
 *   2027 <= year < 2028   futureJoin：年表末值 <-> future 公式的桥接
 *   2028 <= year          future 经验公式（SMH 基线 + 1500 年余弦 + 18.6 年交点）
 *
 * 注意：**年表有保质期**：末项 2027 来自 IERS Bulletin C 的预报。到期后由
 * future 公式接管，而 future 公式是长期的、不需要更新。所以「过期」的后果
 * 仅是 1953-2027 这一段不再有实测值，不会崩、不会跳。
 *
 * 注意：**闰秒表（TAI-UTC）不在这条路径上。** JS 的 deltaTSecondsFromUt1 也是
 * 直接查这里；闰秒只被它那个独立的工具 taiMinusUtc() 用到，跟 ΔT 无关。
 *
 * 换掉 Espenak-Meeus (2006) 的理由：不只是"跟上游一致"。ΔT 是**测量量**不是
 * 模型，IERS 表和 SMH2016 比 2006 年那套拟合准得多 —— 现代差约 4 s、
 * 2050 差 18 s、2100 差 109 s、公元 1000 年差 481 s。远期那几段本就
 * 同源（都是 -20+32u^2），分岔的是中段和未来的长期项。
 *
 * 表 2.3 KB rodata，查表 O(1)（年份连续，直接算下标）+ 一次三次多项式。
 * ------------------------------------------------------------------ */
#define SF_S15_START_YEAR        (-720)
#define SF_EARLY_JOIN_START_YEAR (-820)
#define SF_ANNUAL_START_YEAR     1953
#define SF_ANNUAL_END_YEAR       (SF_ANNUAL_START_YEAR + SF_ANNUAL_N - 1)  /* 2027 */
#define SF_FUTURE_START_YEAR     (SF_ANNUAL_END_YEAR + 1)                  /* 2028 */

/* future 公式的常量，JS 里是"预计算好、不指望引擎折叠"的那几个 */
#define SF_DT_W15            0.41887902047863906   /* 2pi / 15 */
#define SF_DT_W18            0.33756972584642919   /* 2pi / 18.613 */
#define SF_DT_PHASE18        2013.91314
#define SF_DT_FUTURE_OFFSET  (-293.95181375822420)
#define SF_DT_COSINE_RATE    1.4610000000591095    /* 348.7880578 * W15 / 100 */
#define SF_DT_NODAL_RATE     0.19793400875005376   /* 0.58635 * W18 */

#define SF_S15_N 36
static const double SF_S15_SPLINE[][6] = {
    { -720.00000000000000, -100.00000000000000, 409.16000000000003, 776.24699999999996, -9999.5859999999993, 20371.848000000002 },
    { -100.00000000000000, 400.00000000000000, -503.43299999999999, 1303.1510000000001, -5822.2700000000004, 11557.668000000000 },
    { 400.00000000000000, 1000.0000000000000, 1085.0870000000000, -298.29100000000000, -5671.5190000000002, 6535.1160000000000 },
    { 1000.0000000000000, 1150.0000000000000, -25.346000000000000, 184.81100000000001, -753.21000000000004, 1650.3930000000000 },
    { 1150.0000000000000, 1300.0000000000000, -24.640999999999998, 108.77100000000000, -459.62799999999999, 1056.6469999999999 },
    { 1300.0000000000000, 1500.0000000000000, -29.414000000000001, 61.953000000000003, -421.34500000000003, 681.14900000000000 },
    { 1500.0000000000000, 1600.0000000000000, 16.196999999999999, -6.5720000000000001, -192.84100000000001, 292.34300000000002 },
    { 1600.0000000000000, 1650.0000000000000, 3.0179999999999998, 10.505000000000001, -78.697000000000003, 109.12700000000000 },
    { 1650.0000000000000, 1720.0000000000000, -2.1269999999999998, 38.332999999999998, -68.088999999999999, 43.951999999999998 },
    { 1720.0000000000000, 1800.0000000000000, -37.939000000000000, 41.731000000000002, 2.5070000000000001, 12.068000000000000 },
    { 1800.0000000000000, 1810.0000000000000, 1.9179999999999999, -1.1259999999999999, -3.4809999999999999, 18.367000000000001 },
    { 1810.0000000000000, 1820.0000000000000, -3.8119999999999998, 4.6289999999999996, 0.021000000000000001, 15.678000000000001 },
    { 1820.0000000000000, 1830.0000000000000, 3.2500000000000000, -6.8060000000000000, -2.1570000000000000, 16.515999999999998 },
    { 1830.0000000000000, 1840.0000000000000, -0.096000000000000002, 2.9440000000000000, -6.0179999999999998, 10.804000000000000 },
    { 1840.0000000000000, 1850.0000000000000, -0.53900000000000003, 2.6579999999999999, -0.41599999999999998, 7.6340000000000003 },
    { 1850.0000000000000, 1855.0000000000000, -0.88300000000000001, 0.26100000000000001, 1.6419999999999999, 9.3379999999999992 },
    { 1855.0000000000000, 1860.0000000000000, 1.5580000000000001, -2.3889999999999998, -0.48599999999999999, 10.356999999999999 },
    { 1860.0000000000000, 1865.0000000000000, -2.4769999999999999, 2.2839999999999998, -0.59099999999999997, 9.0399999999999991 },
    { 1865.0000000000000, 1870.0000000000000, 2.7200000000000002, -5.1479999999999997, -3.4560000000000000, 8.2550000000000008 },
    { 1870.0000000000000, 1875.0000000000000, -0.91400000000000003, 3.0110000000000001, -5.5930000000000000, 2.3710000000000000 },
    { 1875.0000000000000, 1880.0000000000000, -0.039000000000000000, 0.26900000000000002, -2.3140000000000001, -1.1259999999999999 },
    { 1880.0000000000000, 1885.0000000000000, 0.56299999999999994, 0.15200000000000000, -1.8930000000000000, -3.2100000000000000 },
    { 1885.0000000000000, 1890.0000000000000, -1.4379999999999999, 1.8420000000000001, 0.10100000000000001, -4.3879999999999999 },
    { 1890.0000000000000, 1895.0000000000000, 1.8710000000000000, -2.4740000000000002, -0.53100000000000003, -3.8839999999999999 },
    { 1895.0000000000000, 1900.0000000000000, -0.23200000000000001, 3.1379999999999999, 0.13400000000000001, -5.0170000000000003 },
    { 1900.0000000000000, 1905.0000000000000, -1.2569999999999999, 2.4430000000000001, 5.7149999999999999, -1.9770000000000001 },
    { 1905.0000000000000, 1910.0000000000000, 0.71999999999999997, -1.3290000000000000, 6.8280000000000003, 4.9230000000000000 },
    { 1910.0000000000000, 1915.0000000000000, -0.82499999999999996, 0.83099999999999996, 6.3300000000000001, 11.141999999999999 },
    { 1915.0000000000000, 1920.0000000000000, 0.26200000000000001, -1.6430000000000000, 5.5179999999999998, 17.478999999999999 },
    { 1920.0000000000000, 1925.0000000000000, 0.0080000000000000002, -0.85599999999999998, 3.0200000000000000, 21.617000000000001 },
    { 1925.0000000000000, 1930.0000000000000, 0.12700000000000000, -0.83099999999999996, 1.3330000000000000, 23.789000000000001 },
    { 1930.0000000000000, 1935.0000000000000, 0.14199999999999999, -0.44900000000000001, 0.051999999999999998, 24.417999999999999 },
    { 1935.0000000000000, 1940.0000000000000, 0.70199999999999996, -0.021999999999999999, -0.41899999999999998, 24.164000000000001 },
    { 1940.0000000000000, 1945.0000000000000, -1.1060000000000001, 2.0859999999999999, 1.6450000000000000, 24.425999999999998 },
    { 1945.0000000000000, 1950.0000000000000, 0.61399999999999999, -1.2320000000000000, 2.4990000000000001, 27.050000000000001 },
    { 1950.0000000000000, 1953.0000000000000, -0.27700000000000002, 0.22000000000000000, 1.1270000000000000, 28.931999999999999 },
};

#define SF_ANNUAL_N 75
static const double SF_ANNUAL_DT[] = {
    30.000000000000000, 30.199999999999999, 30.410000000000000, 30.760000000000002, 31.340000000000000, 32.030000000000001, 32.649999999999999, 33.070000000000000,
    33.359999999999999, 33.997224199999998, 34.473468799999999, 35.030689000000002, 35.742421399999998, 36.544435600000000, 37.431984800000002, 38.295178499999999,
    39.204936799999999, 40.180628300000002, 41.169497800000002, 42.229485900000000, 43.373405599999998, 44.484700400000001, 45.476106899999998, 46.458177599999999,
    47.519964799999997, 48.534700200000003, 49.585573699999998, 50.538958600000001, 51.380449700000000, 52.166514900000003, 52.956586100000003, 53.786425999999999,
    54.343127699999997, 54.870420600000003, 55.322136000000000, 55.819696800000003, 56.300055600000000, 56.855217500000002, 57.565317200000003, 58.309137000000000,
    59.121799299999999, 59.984484700000003, 60.785300999999997, 61.628601500000002, 62.295079999999999, 62.965876799999997, 63.467334600000001, 63.828527600000001,
    64.090774400000001, 64.299817500000003, 64.473443000000003, 64.573582099999996, 64.687631600000003, 64.845206899999994, 65.146394700000002, 65.457263200000000,
    65.776835100000000, 66.069864100000004, 66.324518800000007, 66.603041099999999, 66.906943499999997, 67.281066199999998, 67.643928200000005, 68.102487800000006,
    68.592713000000003, 68.967643300000006, 69.220156700000004, 69.361166499999996, 69.359365400000002, 69.294507300000006, 69.203847499999995, 69.175242800000007,
    69.137677900000000, 69.109999999999999, 69.099999999999994,
};

static double dt_long_term(double year)
{
    const double u = (year - 1820.0) / 100.0;
    return -20.0 + 32.0 * u * u;
}

static double dt_long_term_rate(double year)
{
    return 64.0 * (year - 1820.0) / 10000.0;
}

static double dt_future(double year)
{
    const double t = (year - 1825.0) / 100.0;
    return SF_DT_FUTURE_OFFSET
         + 32.50725 * t * t
         + 348.7880578 * cos(SF_DT_W15 * t)
         + 0.58635 * sin(SF_DT_W18 * (year - SF_DT_PHASE18));
}

static double dt_future_rate(double year)
{
    const double t = (year - 1825.0) / 100.0;
    return 0.650145 * t
         - SF_DT_COSINE_RATE * sin(SF_DT_W15 * t)
         + SF_DT_NODAL_RATE * cos(SF_DT_W18 * (year - SF_DT_PHASE18));
}

static double dt_s15(double year)
{
    for (int i = 0; i < SF_S15_N; i++) {
        const double x0 = SF_S15_SPLINE[i][0], x1 = SF_S15_SPLINE[i][1];
        if (year >= x0 && year < x1) {
            const double x = (year - x0) / (x1 - x0);
            return ((SF_S15_SPLINE[i][2] * x + SF_S15_SPLINE[i][3]) * x
                    + SF_S15_SPLINE[i][4]) * x + SF_S15_SPLINE[i][5];
        }
    }
    {   /* 末段外推：JS 的 S15_SPLINE.at(-1) 分支 */
        const double *r = SF_S15_SPLINE[SF_S15_N - 1];
        return ((r[2] + r[3]) + r[4]) + r[5];
    }
}

static double dt_s15_rate(double year)
{
    for (int i = 0; i < SF_S15_N; i++) {
        const double x0 = SF_S15_SPLINE[i][0], x1 = SF_S15_SPLINE[i][1];
        if (year >= x0 && year < x1) {
            const double x = (year - x0) / (x1 - x0);
            return (3.0 * SF_S15_SPLINE[i][2] * x * x
                    + 2.0 * SF_S15_SPLINE[i][3] * x
                    + SF_S15_SPLINE[i][4]) / (x1 - x0);
        }
    }
    {
        const double *r = SF_S15_SPLINE[SF_S15_N - 1];
        return (3.0 * r[2] + 2.0 * r[3] + r[4]) / (r[1] - r[0]);
    }
}

/* 一次 Hermite：两端点值 + 两端点斜率。上游两处 join 都用这个形状。 */
static double dt_hermite(double x, double p0, double p1, double m0, double m1)
{
    const double x2 = x * x, x3 = x2 * x;
    return (2.0 * x3 - 3.0 * x2 + 1.0) * p0
         + (x3 - 2.0 * x2 + x) * m0
         + (-2.0 * x3 + 3.0 * x2) * p1
         + (x3 - x2) * m1;
}

/* -820 .. -720：抛物线 <-> S15，两端点值和斜率都接上，两个源模型在区间外不动 */
static double dt_early_join(double year)
{
    const double x0 = SF_EARLY_JOIN_START_YEAR, x1 = SF_S15_START_YEAR;
    const double span = x1 - x0;
    return dt_hermite((year - x0) / span,
                      dt_long_term(x0), dt_s15(x1),
                      dt_long_term_rate(x0) * span, dt_s15_rate(x1) * span);
}

/* 逐年表插值。Catmull-Rom 写成三次 Hermite；表两端用重复控制点，
 * 与 C++ 那边的单侧斜率约定一致。年份连续，下标直接算。 */
static double dt_interpolate_annual(double year)
{
    const int index = (int)floor(year) - SF_ANNUAL_START_YEAR;
    int i0 = index - 1; if (i0 < 0) i0 = 0;
    const int i1 = index;
    int i2 = index + 1; if (i2 > SF_ANNUAL_N - 1) i2 = SF_ANNUAL_N - 1;
    int i3 = index + 2; if (i3 > SF_ANNUAL_N - 1) i3 = SF_ANNUAL_N - 1;

    const double t0 = SF_ANNUAL_START_YEAR + i0;
    const double t1 = SF_ANNUAL_START_YEAR + i1;
    const double t2 = SF_ANNUAL_START_YEAR + i2;
    const double t3 = SF_ANNUAL_START_YEAR + i3;
    const double p0 = SF_ANNUAL_DT[i0], p1 = SF_ANNUAL_DT[i1];
    const double p2 = SF_ANNUAL_DT[i2], p3 = SF_ANNUAL_DT[i3];

    const double dt = t2 - t1;
    const double m1 = (p2 - p0) / (t2 - t0) * dt;
    const double m2 = (p3 - p1) / (t3 - t1) * dt;
    return dt_hermite((year - t1) / dt, p1, p2, m1, m2);
}

/* 2027 -> 2028：保住 IERS 最后那个预报值，再用一年接上 future 公式 */
static double dt_future_join(double year)
{
    const double x0 = SF_ANNUAL_END_YEAR, x1 = SF_FUTURE_START_YEAR;
    const double span = x1 - x0;
    const double last = SF_ANNUAL_DT[SF_ANNUAL_N - 1];
    return dt_hermite((year - x0) / span,
                      last, dt_future(x1),
                      (last - SF_ANNUAL_DT[SF_ANNUAL_N - 2]) * span,
                      dt_future_rate(x1) * span);
}

double sf_delta_t_seconds(double year)
{
    if (!isfinite(year)) return year;
    /* 前两个分支优先，顺序与 JS 一致 */
    if (year >= SF_S15_START_YEAR && year < SF_ANNUAL_START_YEAR) return dt_s15(year);
    if (year >= SF_ANNUAL_START_YEAR && year < SF_ANNUAL_END_YEAR)
        return dt_interpolate_annual(year);
    if (year < SF_EARLY_JOIN_START_YEAR) return dt_long_term(year);
    if (year < SF_S15_START_YEAR) return dt_early_join(year);
    if (year < SF_FUTURE_START_YEAR) return dt_future_join(year);
    return dt_future(year);
}

/* 某年 1 月 1 日的 JD。照抄 time.js 的 julianDayAtYearStart：
 * 用「年-1 / 月 13」，而世纪修正按**原始** year > 1582 判。 */
static double julian_day_at_year_start(int32_t year)
{
    const int32_t ay = year - 1;
    long correction = 0;
    if (year > 1582) {
        const long century = ay / 100;
        correction = 2 - century + century / 4;
    }
    return floor(365.25 * (ay + 4716)) + floor(30.6001 * 14.0)
         + 1.0 + (double)correction - 1524.5;
}

/* 小数年。照抄 time.js 的 decimalYearFromJulianDay：先取该 JD 所在的历年，
 * 再在「本年 1 月 1 日」与「次年 1 月 1 日」之间线性插值。
 *
 * 注意：不得用近似式 `y + (mo-0.5)/12`，其误差可达 ±0.04 年 —— 乘上 ΔT 的
 * 年变化率（现代约 0.7 s/年）即 0.03 s。虽小，既要求对齐就须对齐。 */
static double decimal_year_from_jd(double jd)
{
    int32_t y = 0, mo = 0, d = 0;
    sf_solar_date_from_day_number((int64_t)floor(jd + 0.5), &y, &mo, &d);
    const double start = julian_day_at_year_start(y);
    const double next  = julian_day_at_year_start(y + 1);
    return (double)y + (jd - start) / (next - start);
}

double sf_ut_to_tt(double jd_ut)
{
    return jd_ut + sf_delta_t_seconds(decimal_year_from_jd(jd_ut)) / 86400.0;
}

double sf_tt_to_ut(double jd_tt)
{
    /* 照抄 JS deltaTSecondsFromTt 的**两轮定点细化**：ΔT 要按 UT1 查，
     * 而 UT1 又依赖 ΔT，迭代两次就收敛到 1e-9 天以内。 */
    double jd_ut = jd_tt, dt = 0.0;
    for (int i = 0; i < 2; i++) {
        dt = sf_delta_t_seconds(decimal_year_from_jd(jd_ut));
        jd_ut = jd_tt - dt / 86400.0;
    }
    return jd_ut;
}

/* ==================================================================
 * 闰秒（TAI-UTC）与 UTC
 *
 * 上面那对 sf_ut_to_tt / sf_tt_to_ut 里的「UT」是 **UT1**（ΔT = TT − UT1，
 * 年表本来就是从 C04 的 UT1−UTC 导出来的）。但**调用方手上通常是 UTC** ——
 * 手机时戳、日志、用户填的出生时间，全是 UTC/民用时。两者的差是 DUT1，
 * 上限 0.9 s，不能忽略（恒星时 1 s 差 15″，出没时刻差 1 s）。
 *
 * 三条链：
 *     TT  = UTC + (TAI−UTC) + 32.184          ← 精确，用本节的表
 *     UT1 = UTC + DUT1                        ← DUT1 由下面的恒等式反推
 *     UT1 = TT − ΔT                           ← 原有的路，收 UT1
 *
 * 注意：**DUT1 不是闰秒表能给的** —— UT1−UTC 是 IERS 观测量，闰秒只保证
 * |DUT1| < 0.9 s。但把 ΔT 的定义拆开就能反推：
 *
 *     ΔT = TT − UT1 = 32.184 + (TAI−UTC) − (UT1−UTC)
 *     ⟹  UT1 − UTC = 32.184 + (TAI−UTC) − ΔT
 *
 * 右边三项库里都有（ΔT 年表 1953–2027 本来就是 C04 导出的），所以 DUT1 的
 * 信息**早就在库里**，缺的只是闰秒表这一块。实测反推每个闰秒生效日插入前的
 * DUT1，26 个全部落在 −0.68 … −0.19（插入后 +0.32 … +0.81），与 IERS
 * 「DUT1 逼近 ±0.9 才补闰秒」的策略吻合 —— 这条检验是有效的：ΔT 若有整体偏
 * 差，这一列会整列平移、立刻掉出该区间。
 *
 * 注意：反推出来的 DUT1 精度受**年表采样**限制（ΔT 是逐年值，年内靠三次插值），
 * 量级 ~0.05 s；这比「只有闰秒」时的 ±0.9 s 好一个半数量级，但**没有**跟
 * C04 逐日对过。要求更高时应自行接 IERS 的 finals2000A。
 *
 * 注意：1960-01-01 之前没有 UTC，本层沿用上游的约定：**UTC ≈ UT1**。
 * ================================================================== */
#define SF_UTC_HISTORY_START_JD  2436934.5      /* 1960-01-01 00:00 UTC */
#define SF_TT_MINUS_TAI_SECONDS  32.184

/* TAI-UTC 历史。列 = [生效 MJD, 基准秒数, 参考 MJD, 每天漂移秒]。
 *
 * 注意：1972 之前 UTC 用的是**分段频率偏移**（所谓橡皮秒），段内随时间线性变；
 * 1972 起才是整秒阶跃。所以除「阶跃 + 新秒数」外还须存斜率。
 *
 * 来源：IERS UTC-TAI.history（1961-1971）、IERS Leap_Second.dat / Bulletin C 72
 * （1972 起，本次编制有效期到 2027-06-28）、1960 段沿用 IAU SOFA/ERFA 的历史
 * 表达式。与 taiyin-lite `time.js` 的 TAI_MINUS_UTC_HISTORY **逐值相同**。
 *
 * 注意：有保质期，同 ΔT 年表：新闰秒由 Bulletin C 发布。到期后本表给的 TAI-UTC
 * 会偏小（每次闰秒 1 s），表现为 UTC→TT 和 DUT1 一起偏。 */
#define SF_TAI_UTC_N 42
static const double SF_TAI_UTC[SF_TAI_UTC_N][4] = {
    { 36934, 1.4178180, 37300, 0.0012960 },
    { 37300, 1.4228180, 37300, 0.0012960 },
    { 37512, 1.3728180, 37300, 0.0012960 },
    { 37665, 1.8458580, 37665, 0.0011232 },
    { 38334, 1.9458580, 37665, 0.0011232 },
    { 38395, 3.2401300, 38761, 0.0012960 },
    { 38486, 3.3401300, 38761, 0.0012960 },
    { 38639, 3.4401300, 38761, 0.0012960 },
    { 38761, 3.5401300, 38761, 0.0012960 },
    { 38820, 3.6401300, 38761, 0.0012960 },
    { 38942, 3.7401300, 38761, 0.0012960 },
    { 39004, 3.8401300, 38761, 0.0012960 },
    { 39126, 4.3131700, 39126, 0.0025920 },
    { 39887, 4.2131700, 39126, 0.0025920 },
    { 41317, 10, 41317, 0 },
    { 41499, 11, 41499, 0 },
    { 41683, 12, 41683, 0 },
    { 42048, 13, 42048, 0 },
    { 42413, 14, 42413, 0 },
    { 42778, 15, 42778, 0 },
    { 43144, 16, 43144, 0 },
    { 43509, 17, 43509, 0 },
    { 43874, 18, 43874, 0 },
    { 44239, 19, 44239, 0 },
    { 44786, 20, 44786, 0 },
    { 45151, 21, 45151, 0 },
    { 45516, 22, 45516, 0 },
    { 46247, 23, 46247, 0 },
    { 47161, 24, 47161, 0 },
    { 47892, 25, 47892, 0 },
    { 48257, 26, 48257, 0 },
    { 48804, 27, 48804, 0 },
    { 49169, 28, 49169, 0 },
    { 49534, 29, 49534, 0 },
    { 50083, 30, 50083, 0 },
    { 50630, 31, 50630, 0 },
    { 51179, 32, 51179, 0 },
    { 53736, 33, 53736, 0 },
    { 54832, 34, 54832, 0 },
    { 56109, 35, 56109, 0 },
    { 57204, 36, 57204, 0 },
    { 57754, 37, 57754, 0 },
};

/* TAI − UTC，秒。**1960-01-01 之前返回 NAN**（那时还没有 UTC）。
 * 参数是 UTC 儒略日；返回 NAN 时调用方应退化到 UTC≈UT1。 */
double sf_tai_minus_utc_seconds(double jd_utc)
{
    if (!(jd_utc >= SF_UTC_HISTORY_START_JD)) return NAN;   /* 一并滤除 NaN */
    const double mjd = jd_utc - 2400000.5;
    for (int i = SF_TAI_UTC_N - 1; i >= 0; i--) {          /* 倒序 = 取最后一条生效的 */
        if (mjd >= SF_TAI_UTC[i][0])
            return SF_TAI_UTC[i][1] + (mjd - SF_TAI_UTC[i][2]) * SF_TAI_UTC[i][3];
    }
    return NAN;
}

/* UTC → TT：TT = UTC + (TAI−UTC) + 32.184。1960 前退化到 UTC≈UT1。 */
double sf_utc_to_tt(double jd_utc)
{
    const double tai = sf_tai_minus_utc_seconds(jd_utc);
    if (isnan(tai)) return sf_ut_to_tt(jd_utc);
    return jd_utc + (tai + SF_TT_MINUS_TAI_SECONDS) / 86400.0;
}

/* TT → UTC。先按 UT1 估一次：既用来判 1960 前的退化分支，也当迭代的起点 ——
 * UT1 与 UTC 相差不到 1 s，所以第一轮几乎总落在正确的闰秒段里，段边界附近
 * 再靠后面两轮纠正（段要由结果自己决定，一次不够）。
 *
 * 注意：起点**不采用**上游那个「先减末项 37 s」的写法：它只在现代成立，一到
 * 1960 年附近就会跳到表外、落进 NAN 分支，把 UT1 估计原样返回（实测
 * 2436934.5 处往返差 0.0575 s）。改用 UT1 估计后全区间往返精确到 0。
 *
 * 注意：本函数**表达不了 23:59:60 这个标签**，闰秒那一秒会映射到前后两点之一。 */
double sf_tt_to_utc(double jd_tt)
{
    const double historical = sf_tt_to_ut(jd_tt);
    const double utc_era_start_tt = sf_utc_to_tt(SF_UTC_HISTORY_START_JD);
    if (jd_tt < utc_era_start_tt) return historical;
    double jd_utc = historical;
    for (int k = 0; k < 3; k++) {
        const double tai = sf_tai_minus_utc_seconds(jd_utc);
        if (isnan(tai)) return historical;
        jd_utc = jd_tt - (tai + SF_TT_MINUS_TAI_SECONDS) / 86400.0;
    }
    return jd_utc;
}

/* DUT1 = UT1 − UTC，秒。用 ΔT 的定义反推（见本节头注）。
 * 1960 前返回 NAN；1960–1972 段含橡皮秒，值可能超出 ±0.9（正常）。 */
double sf_dut1_seconds(double jd_utc)
{
    const double tai = sf_tai_minus_utc_seconds(jd_utc);
    if (isnan(tai)) return NAN;
    return SF_TT_MINUS_TAI_SECONDS + tai
         - sf_delta_t_seconds(decimal_year_from_jd(jd_utc));
}

/* UTC → UT1。1960 前退化到 UTC≈UT1（那时民用时就是 UT1）。 */
double sf_utc_to_ut1(double jd_utc)
{
    const double dut1 = sf_dut1_seconds(jd_utc);
    if (isnan(dut1)) return jd_utc;
    return jd_utc + dut1 / 86400.0;
}

/* UT1 → UTC。DUT1 要按 UTC 查，而 UTC 又依赖 DUT1 —— 迭代三次（差值 <1 s，
 * 第一次就已经到 1e-4 s 以内）。1960 前退化到 UTC≈UT1。 */
double sf_ut1_to_utc(double jd_ut1)
{
    double jd_utc = jd_ut1;
    for (int k = 0; k < 3; k++) {
        const double dut1 = sf_dut1_seconds(jd_utc);
        if (isnan(dut1)) return jd_ut1;
        jd_utc = jd_ut1 - dut1 / 86400.0;
    }
    return jd_utc;
}

/* ------------------------------------------------------------------
 * 混合儒略/格里历
 *
 * 注意：必须是**混合历**，不得一路用格里历。古历归日表一路到公元前 722 年，
 * 那全在儒略历区间；用纯格里历会整体偏 10 天以上，而 fixture 里
 * −456 / −104 / 10 / 238 / 690 这些全是儒略历日期。
 *
 * 切换规则照抄 time.js:293 julianDay()：判据用**原始**年月日，
 * 而世纪修正用「月 ≤ 2 已调整」之后的年。
 * ------------------------------------------------------------------ */
double sf_julian_day_ut(int32_t y, int32_t mo, int32_t d,
                        int32_t h, int32_t mi, double s)
{
    int32_t ay = y, am = mo;
    if (am <= 2) { ay -= 1; am += 12; }

    const int gregorian = (y > 1582)
        || (y == 1582 && (mo > 10 || (mo == 10 && d >= 15)));

    long correction = 0;
    if (gregorian) {
        /* C99 整数除法向零截断，和 JS 的 Math.trunc 一致 */
        const long century = ay / 100;
        correction = 2 - century + century / 4;
    }

    const double day_fraction = (h + (mi + s / 60.0) / 60.0) / 24.0;
    return floor(365.25 * (ay + 4716)) + floor(30.6001 * (am + 1))
         + d + day_fraction + (double)correction - 1524.5;
}

int64_t sf_solar_day_number(int32_t y, int32_t mo, int32_t d)
{
    /* = floor(julianDay({y,mo,d,0,0,0}) + 0.5)，但直接把 .5 约掉，
     * 全程整数，避免 2451545.5 这种值在大数上丢精度 */
    int32_t ay = y, am = mo;
    if (am <= 2) { ay -= 1; am += 12; }

    const int gregorian = (y > 1582)
        || (y == 1582 && (mo > 10 || (mo == 10 && d >= 15)));

    long correction = 0;
    if (gregorian) {
        const long century = ay / 100;
        correction = 2 - century + century / 4;
    }
    return (int64_t)floor(365.25 * (ay + 4716))
         + (int64_t)floor(30.6001 * (am + 1))
         + d + correction - 1524;
}

void sf_solar_date_from_day_number(int64_t jdn, int32_t *y, int32_t *mo, int32_t *d)
{
    /* 照抄 time.js:317 calendarDateFromJulianDay(jdn - 0.5)，只是小数部分恒为 0 */
    const double z = (double)jdn;
    double a = z;
    if (z >= 2299161.0) {                       /* 2299161 = 1582-10-15 */
        const double alpha = floor((z - 1867216.25) / 36524.25);
        a = z + 1 + alpha - (double)(long)(alpha / 4.0);   /* Math.trunc */
    }
    const double b = a + 1524;
    const double c = floor((b - 122.1) / 365.25);
    const double dd = floor(365.25 * c);
    const double e = floor((b - dd) / 30.6001);

    const double day_decimal = b - dd - floor(30.6001 * e);
    const int32_t day = (int32_t)floor(day_decimal);
    const int32_t month = (int32_t)(e < 14 ? e - 1 : e - 13);
    const int32_t year = (int32_t)(month > 2 ? c - 4716 : c - 4715);

    if (y)  *y = year;
    if (mo) *mo = month;
    if (d)  *d = day;
}

void sf_cal_datetime_from_jd(double jd, sf_cal_datetime *out)
{
    /* 照抄 time.js:317 calendarDateFromJulianDay —— 这是**带小数**的那一版，
     * 上面 sf_solar_date_from_day_number 是它取整的特例。
     * 注意：与 JS 一样，这个函数**不管时标**：给它什么 JD 就按什么 JD 拆。
     * 要当地民用钟面，喂 jd_utc + offset/1440（ZonedTime.fromJulianTime 就是这么做的）。 */
    if (!out) return;
    const double shifted = jd + 0.5;
    const double z = floor(shifted);
    const double fraction = shifted - z;
    double a = z;
    if (z >= 2299161.0) {                       /* 1582-10-15 */
        const double alpha = floor((z - 1867216.25) / 36524.25);
        a = z + 1 + alpha - (double)(long)(alpha / 4.0);   /* Math.trunc */
    }
    const double b = a + 1524;
    const double c = floor((b - 122.1) / 365.25);
    const double dd = floor(365.25 * c);
    const double e = floor((b - dd) / 30.6001);

    const double day_decimal = b - dd - floor(30.6001 * e) + fraction;
    const int32_t day = (int32_t)floor(day_decimal);
    const int32_t month = (int32_t)(e < 14 ? e - 1 : e - 13);

    double seconds = (day_decimal - (double)day) * 86400.0;
    const int32_t hour = (int32_t)floor(seconds / 3600.0);
    seconds -= (double)hour * 3600.0;
    const int32_t minute = (int32_t)floor(seconds / 60.0);

    out->year = (int32_t)(month > 2 ? c - 4716 : c - 4715);
    out->month = month;
    out->day = day;
    out->hour = hour;
    out->minute = minute;
    out->second = seconds - (double)minute * 60.0;
}

/* sf_solar_date 的 month/day 是 uint8_t，转换函数收 int32_t*，中间倒一手 */
static void solar_date_from_day(int64_t jdn, sf_solar_date *out)
{
    int32_t y = 0, mo = 0, d = 0;
    sf_solar_date_from_day_number(jdn, &y, &mo, &d);
    out->year = y;
    out->month = (uint8_t)mo;
    out->day = (uint8_t)d;
}

/* ------------------------------------------------------------------
 * 配置
 * ------------------------------------------------------------------ */
void sf_cal_config_init(sf_cal_config *c)
{
    memset(c, 0, sizeof *c);
    c->mode = SF_CAL_CHINA_STANDARD_HISTORICAL;
    c->day_boundary_mode = SF_CAL_FIXED_UTC_OFFSET;
    c->utc_offset_minutes = 480;
    c->pillar_historical_mode = SF_PILLAR_HISTORICAL_FOLLOW_CALENDAR;
    c->calendar_meridian_deg = 0.0;
}

void sf_cal_config_init_china_standard_historical(sf_cal_config *c,
                                                  int32_t local_utc_offset_minutes)
{
    sf_cal_config_init(c);
    c->mode = SF_CAL_CHINA_STANDARD_HISTORICAL;
    c->utc_offset_minutes = local_utc_offset_minutes;
}

void sf_cal_config_init_china_standard_astronomical(sf_cal_config *c,
                                                    int32_t local_utc_offset_minutes)
{
    sf_cal_config_init(c);
    c->mode = SF_CAL_CHINA_STANDARD_ASTRONOMICAL;
    c->utc_offset_minutes = local_utc_offset_minutes;
}

void sf_cal_config_init_local_astronomical_utc_offset(sf_cal_config *c,
                                                      int32_t utc_offset_minutes)
{
    sf_cal_config_init(c);
    c->mode = SF_CAL_LOCAL_ASTRONOMICAL;
    c->day_boundary_mode = SF_CAL_FIXED_UTC_OFFSET;
    c->utc_offset_minutes = utc_offset_minutes;
}

void sf_cal_config_init_local_astronomical_meridian(sf_cal_config *c,
                                                    double longitude_deg)
{
    sf_cal_config_init(c);
    c->mode = SF_CAL_LOCAL_ASTRONOMICAL;
    c->day_boundary_mode = SF_CAL_MEAN_SOLAR_MERIDIAN;
    c->calendar_meridian_deg = longitude_deg;
}

static int config_valid(const sf_cal_config *c)
{
    if (!c) return 0;
    if (c->mode != SF_CAL_CHINA_STANDARD_HISTORICAL
        && c->mode != SF_CAL_LOCAL_ASTRONOMICAL
        && c->mode != SF_CAL_CHINA_STANDARD_ASTRONOMICAL) return 0;
    if (c->day_boundary_mode != SF_CAL_FIXED_UTC_OFFSET
        && c->day_boundary_mode != SF_CAL_MEAN_SOLAR_MERIDIAN) return 0;
    if (c->utc_offset_minutes < -14 * 60 || c->utc_offset_minutes > 14 * 60) return 0;
    if (!(c->calendar_meridian_deg >= -180.0 && c->calendar_meridian_deg <= 180.0)) return 0;
    return 1;
}

/* 显示日界。**只有 instant→日期 那一步用它**，结构一概不用 —— 见下面
 * structure_day_offset。 */
static double local_day_offset(const sf_cal_config *c)
{
    if (c->day_boundary_mode == SF_CAL_FIXED_UTC_OFFSET)
        return (double)c->utc_offset_minutes / (24.0 * 60.0);
    return c->calendar_meridian_deg / 360.0;
}

/* 结构日界：决定「哪个朔是哪一天」「冬至落在哪一天」，进而决定闰月。
 *
 * 注意：除 LOCAL_ASTRONOMICAL 外**一律钉死在东经 120°**，跟 utc_offset_minutes
 * 无关。这正是「设备时区设成纽约，初一还是对的」的原因，也是
 * instantToLunar(同一瞬时, 偏移 +480 vs 0) 会给出不同农历日、而
 * calculateChineseCalendarYear 完全无视 utc_offset_minutes 的原因。 */
static double structure_day_offset(const sf_cal_config *c)
{
    if (c->mode != SF_CAL_LOCAL_ASTRONOMICAL) return SF_CHINA_OFF;
    return local_day_offset(c);
}

/* ------------------------------------------------------------------
 * 归日
 * ------------------------------------------------------------------ */
static int64_t civil_day_number(double jd_ut, double day_offset)
{
    return (int64_t)floor(jd_ut + day_offset + 0.5);
}

/* 对外的那一份（`chinese-calendar.js` 的 `civilDayNumber`）。
 * 气朔事件表要用它算「当地民用日」与「历法归日」两个日号。 */
int64_t sf_cal_civil_day_number(double jd_ut, double day_offset)
{
    if (!isfinite(day_offset)) return -1;      /* JS 这里抛 TypeError */
    return civil_day_number(jd_ut, day_offset);
}

/* ---- 古历归日表的解码 --------------------------------------------
 *
 * 表是「分段线性 + 稀疏 ±1 残差」，见 sf_calendar_data.h 的说明。
 * 与上游 calendar.cpp:186-300 逐函数对应。
 * ------------------------------------------------------------------ */
enum { SF_HIST_NEW_MOON = 0, SF_HIST_SOLAR_TERM = 1 };

static uint32_t hist_popcount64(uint64_t v)
{
    uint32_t n = 0;
    while (v) { v &= v - 1; n++; }
    return n;
}

/* 位序是 **uint64 内小端**（低位在前） */
static int hist_bit_at(const uint64_t *words, size_t i)
{
    return (int)((words[i / 64u] >> (i % 64u)) & UINT64_C(1));
}

/* 前 i 位里有多少个 1 —— 这是该事件在 residual_signs 里的下标。
 *
 * 注意：分块的「字跨度」是 `rankBlockEvents / 64`。JS 那边因为把 uint64 拆成了
 * 两个 uint32，同样的表达式写成 `/32`。抄错这个**不会崩**，只会静默给出
 * 差一天的结果 —— 是这套表里最容易错、又最难发现的一处。 */
static uint32_t hist_rank(const uint64_t *words, const uint16_t *prefixes, size_t i)
{
    const size_t block = i / SF_HIST_RESIDUAL_RANK_BLOCK_EVENTS;
    const size_t block_word = block * (SF_HIST_RESIDUAL_RANK_BLOCK_EVENTS / 64u);
    const size_t word = i / 64u;
    uint32_t rank = prefixes[block];
    for (size_t k = block_word; k < word; k++) rank += hist_popcount64(words[k]);
    const unsigned bit = (unsigned)(i % 64u);
    if (bit != 0u)
        rank += hist_popcount64(words[word] & ((UINT64_C(1) << bit) - UINT64_C(1)));
    return rank;
}

/* 注意：真正的地板除。JS 用 Math.floor，C++ 用向零截断的整数除法 —— 两者只在
 * ticks ≥ 0 时等价（本数据集全是正的，所以上游两种写法结果相同）。
 * 这里写地板除是为了对齐**语义规范**（JS 那份），而非碰巧一致。 */
static int64_t hist_floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    if (a % b != 0 && a < 0) q--;
    return q;
}

static int64_t hist_linear_day(const sf_hist_segment *s, size_t idx, int64_t phase)
{
    const int64_t local = (int64_t)idx - (int64_t)s->first_event_index;
    const int64_t ticks = s->base_ticks + s->step_ticks * local + phase;
    return hist_floor_div(ticks + SF_HIST_CIVIL_DAY_SCALE / 2, SF_HIST_CIVIL_DAY_SCALE);
}

#define SF_HIST_N(a) (sizeof(a) / sizeof((a)[0]))

static int64_t hist_profile_civil_day(int kind, size_t idx)
{
    if (kind == SF_HIST_SOLAR_TERM) {
        for (size_t i = 0; i < SF_HIST_N(sf_hist_solar_term_exact_segments); i++) {
            const sf_hist_segment *s = &sf_hist_solar_term_exact_segments[i];
            if (idx >= s->first_event_index
                && idx < (size_t)s->first_event_index + s->event_count)
                return hist_linear_day(s, idx, 0);
        }
        const size_t local = idx - sf_hist_solar_term_tail.first_event_index;
        const int64_t phase =
            sf_hist_solar_term_phase_ticks[local % SF_HIST_N(sf_hist_solar_term_phase_ticks)];
        int64_t day = hist_linear_day(&sf_hist_solar_term_tail, idx, phase);
        if (!hist_bit_at(sf_hist_solar_term_residual_mask, local)) return day;
        const uint32_t rank = hist_rank(sf_hist_solar_term_residual_mask,
                                        sf_hist_solar_term_residual_rank, local);
        return day + (hist_bit_at(sf_hist_solar_term_residual_signs, rank) ? 1 : -1);
    }

    for (size_t i = 0; i < SF_HIST_N(sf_hist_new_moon_exact_segments); i++) {
        const sf_hist_segment *s = &sf_hist_new_moon_exact_segments[i];
        if (idx >= s->first_event_index
            && idx < (size_t)s->first_event_index + s->event_count)
            return hist_linear_day(s, idx, 0);
    }
    const size_t local = idx - sf_hist_new_moon_tail.first_event_index;
    int64_t day = hist_linear_day(&sf_hist_new_moon_tail, idx, 0);
    if (!hist_bit_at(sf_hist_new_moon_residual_mask, local)) return day;
    const uint32_t rank = hist_rank(sf_hist_new_moon_residual_mask,
                                    sf_hist_new_moon_residual_rank, local);
    return day + (hist_bit_at(sf_hist_new_moon_residual_signs, rank) ? 1 : -1);
}

/* 用相位索引定位事件序号。**表不负责判断「调用方看的是哪一次事件」** ——
 * 那由调用方的天文估计决定；表只回答「那个序号被归到哪一天」。
 * 2451259 / 2451551 是 sxwnl 自己的相位原点，+7 / +14 是它的 pc 偏移。 */
static int hist_event_index(int kind, double est_jd_ut, size_t *out)
{
    if (!(est_jd_ut < (double)SF_HIST_PROFILE_END_JD)) return 0;

    const double phase = (kind == SF_HIST_SOLAR_TERM)
        ? floor((est_jd_ut + 7.0 - 2451259.0) / SF_DAYS_PER_TROPICAL_YEAR * 24.0)
        : floor((est_jd_ut + 14.0 - 2451551.0) / SF_DAYS_PER_SYNODIC_MONTH);
    const int64_t first = (kind == SF_HIST_SOLAR_TERM)
        ? SF_HIST_SOLAR_TERM_FIRST_PHASE_INDEX : SF_HIST_NEW_MOON_FIRST_PHASE_INDEX;
    const double count = (kind == SF_HIST_SOLAR_TERM)
        ? (double)SF_HIST_SOLAR_TERM_EVENT_COUNT : (double)SF_HIST_NEW_MOON_EVENT_COUNT;

    const double ordinal = phase - (double)first;
    if (!isfinite(ordinal) || ordinal < 0.0 || ordinal >= count) return 0;
    *out = (size_t)ordinal;
    return 1;
}

int64_t sf_cal_historical_civil_day(int kind, double estimate_jd_ut)
{
    if (kind != SF_CAL_HIST_NEW_MOON && kind != SF_CAL_HIST_SOLAR_TERM) return -1;
    size_t idx = 0;
    if (!hist_event_index(kind, estimate_jd_ut, &idx)) return -1;
    return hist_profile_civil_day(kind, idx);
}

/* 查得到就用表，查不到退回天文。
 *
 * 表**不是**替代品而是兼容层：1960-01-01 之后一律走天文；前 722 年之前
 * （朔）/ 前 221 年之前（气）也走天文 —— 后者那种「朔用古历、气用天文」
 * 的不对称是 sxwnl 的原设计，不是 bug。 */
static int64_t assigned_event_day(const sf_cal_config *c, int kind, double jd_ut)
{
    if (c->mode == SF_CAL_CHINA_STANDARD_HISTORICAL) {
        size_t idx = 0;
        if (hist_event_index(kind, jd_ut, &idx))
            return hist_profile_civil_day(kind, idx);
    }
    return civil_day_number(jd_ut, structure_day_offset(c));
}

/* ------------------------------------------------------------------
 * 求解器胶水
 *
 * 注意：sf_solve_* 必须返回**最近**的那次事件，不能是「种子之后第一次」。
 * 下面的 find_winter_solstice / fill_new_moons 都靠「往回走」收敛，
 * 返回「之后第一次」会让那些循环不终止。sf_solve_solar_term 内部用
 * sf_js_round 选圈，是「最近」，兼容。
 * ------------------------------------------------------------------ */

/* 按**目标黄经**解一个节气。返回民用日号，*out_jd_tt 给出 TT。
 *
 * 单独拆出「按黄经」这一版是因为 sf_cal_find_term 里步长会越出 0..23
 * （它要在近似位置 ±3 步的范围内找），那时只有「目标角」有意义。 */
/* 低项数解只用于归日预判。距“历法结构日界”20 分钟内才重算完整解。
 *
 * 注意：这里不是显示时区的当地午夜：CHINA 两种模式固定东经 120°，LOCAL 才跟
 * 配置里的历法经度/结构偏移走。正好复用 structure_day_offset(c)，不读
 * local_day_offset(c)。全窗口穷举最坏为气 15.473 / 朔 14.766 分钟；20 分钟
 * 仍留 4.5 分钟余量，且把精算触发率从 4.167% 降到约 2.778%。 */
#define SF_CAL_LOW_BOUNDARY_MINUTES 20.0

static int near_structure_day_boundary(const sf_cal_config *c, double jd_ut)
{
    const double x = jd_ut + structure_day_offset(c) + 0.5;
    const double frac = x - floor(x);
    const double days = fmin(frac, 1.0 - frac);
    return days * 1440.0 <= SF_CAL_LOW_BOUNDARY_MINUTES;
}

static int64_t term_event_at_mode(const sf_cal_config *c, double target_rad,
                                  double est_jd_tt, double *out_jd_tt,
                                  int low_for_day)
{
    double jd_tt = low_for_day ? sf_solve_solar_term_low(target_rad, est_jd_tt)
                               : sf_solve_solar_term(target_rad, est_jd_tt);
    double jd_ut = isfinite(jd_tt) ? sf_tt_to_ut(jd_tt) : NAN;
    if (low_for_day && isfinite(jd_ut)
        && near_structure_day_boundary(c, jd_ut)) {
        jd_tt = sf_solve_solar_term(target_rad, jd_tt);
        jd_ut = isfinite(jd_tt) ? sf_tt_to_ut(jd_tt) : NAN;
    }
    if (out_jd_tt) *out_jd_tt = jd_tt;
    if (!isfinite(jd_ut)) return INT64_MIN;
    return assigned_event_day(c, SF_HIST_SOLAR_TERM, jd_ut);
}

static int64_t term_event_at(const sf_cal_config *c, double target_rad,
                             double est_jd_tt, double *out_jd_tt)
{
    return term_event_at_mode(c, target_rad, est_jd_tt, out_jd_tt, 0);
}

/* 第 index 个节气（0 = 冬至 = 270°）。 */
static int64_t term_event_mode(const sf_cal_config *c, int index,
                               double est_jd_tt, double *out_jd_tt,
                               int low_for_day)
{
    /* 上游是用 posmod(term_id - 5, 24) 绕一圈算的，等价但难读。
     * 这里用 chinese-calendar.js:220 的直白写法：270° 起每 15° 一个。 */
    double target = fmod((270.0 + 15.0 * index) * SF_DEG, SF_TWO_PI);
    if (target < 0.0) target += SF_TWO_PI;
    return term_event_at_mode(c, target, est_jd_tt, out_jd_tt, low_for_day);
}

static int64_t term_event(const sf_cal_config *c, int index,
                          double est_jd_tt, double *out_jd_tt)
{
    return term_event_mode(c, index, est_jd_tt, out_jd_tt, 0);
}

/* 距 estimate 最近的一次定朔。返回民用日号，*out_jd_tt 给出 TT。 */
static int64_t new_moon_event_mode(const sf_cal_config *c, double est_jd_tt,
                                   double *out_jd_tt, int low_for_day)
{
    double jd_tt = low_for_day ? sf_solve_lunar_phase_low(0.0, est_jd_tt)
                               : sf_solve_lunar_phase(0.0, est_jd_tt);
    double jd_ut = isfinite(jd_tt) ? sf_tt_to_ut(jd_tt) : NAN;
    if (low_for_day && isfinite(jd_ut)
        && near_structure_day_boundary(c, jd_ut)) {
        jd_tt = sf_solve_lunar_phase(0.0, jd_tt);
        jd_ut = isfinite(jd_tt) ? sf_tt_to_ut(jd_tt) : NAN;
    }
    if (out_jd_tt) *out_jd_tt = jd_tt;
    if (!isfinite(jd_ut)) return INT64_MIN;
    return assigned_event_day(c, SF_HIST_NEW_MOON, jd_ut);
}

/* 冬至锚点 = civilDayNumber ≤ target_day 的**最后一个**冬至。
 * 上游 calendar.cpp:565 用「扫一整年找 270° 交点」，JS 用往回/往前各走一步
 * 的循环。这里照 JS（chinese-calendar.js:212）—— 它只需要最近事件求解器，
 * 不用另开一套交点搜索 API，语义完全一致。 */
static int find_winter_solstice(const sf_cal_config *c, double jd_ut,
                                sf_cal_term_event *out, int low_for_day)
{
    const int64_t target_day = civil_day_number(jd_ut, structure_day_offset(c));

    double jd_tt = 0.0;
    int64_t day = term_event_mode(c, 0, sf_ut_to_tt(jd_ut), &jd_tt, low_for_day);
    if (day == INT64_MIN) return -1;

    /* 往回退到不晚于 target_day */
    for (int guard = 0; day > target_day; guard++) {
        if (guard > 8) return -1;
        day = term_event_mode(c, 0, jd_tt - SF_DAYS_PER_TROPICAL_YEAR,
                              &jd_tt, low_for_day);
        if (day == INT64_MIN) return -1;
    }
    /* 再往前推进到最后一个仍不晚于 target_day 的 */
    for (int guard = 0; guard <= 8; guard++) {
        double next_tt = 0.0;
        const int64_t next = term_event_mode(c, 0,
            jd_tt + SF_DAYS_PER_TROPICAL_YEAR, &next_tt, low_for_day);
        if (next == INT64_MIN) return -1;
        if (next > target_day) break;
        day = next;
        jd_tt = next_tt;
    }

    out->index_from_winter_solstice = 0;
    out->target_longitude_rad = 270.0 * SF_DEG;
    out->jd_ut = sf_tt_to_ut(jd_tt);
    out->civil_day_number = day;
    return 0;
}

/* ------------------------------------------------------------------
 * 一个岁的两条序列
 * ------------------------------------------------------------------ */
static int fill_solar_terms(const sf_cal_config *c, sf_cal_year *out, int low_for_day)
{
    /* find_winter_solstice 已给出第 0 项。日期快路径判闰月只消费中气
     * （偶数项），小节气无读者，直接不算。 */
    double jd_tt = sf_ut_to_tt(out->solar_terms[0].jd_ut);
    unsigned prev = 0;
    for (unsigned i = low_for_day ? 2u : 1u; i < SF_CAL_TERM_COUNT;
         i += low_for_day ? 2u : 1u) {
        const unsigned step = i - prev;
        const int64_t day = term_event_mode(c, (int)i,
            jd_tt + (double)step * SF_DAYS_PER_SOLAR_TERM, &jd_tt, low_for_day);
        if (day == INT64_MIN) return -1;
        out->solar_terms[i].index_from_winter_solstice = (uint8_t)i;
        {
            double target = fmod((270.0 + 15.0 * (double)i) * SF_DEG, SF_TWO_PI);
            if (target < 0.0) target += SF_TWO_PI;
            out->solar_terms[i].target_longitude_rad = target;
        }
        out->solar_terms[i].jd_ut = sf_tt_to_ut(jd_tt);
        out->solar_terms[i].civil_day_number = day;
        prev = i;
    }

    out->solar_term_count = (uint8_t)SF_CAL_TERM_COUNT;
    out->first_winter_solstice_day_number  = out->solar_terms[0].civil_day_number;
    out->second_winter_solstice_day_number = out->solar_terms[24].civil_day_number;
    return 0;
}

static int fill_new_moons(const sf_cal_config *c, sf_cal_year *out, int low_for_day)
{
    const int64_t winter_day = out->solar_terms[0].civil_day_number;
    const double winter_tt = sf_ut_to_tt(out->solar_terms[0].jd_ut);

    double jd_tt = 0.0;
    int64_t day = new_moon_event_mode(c, winter_tt, &jd_tt, low_for_day);
    if (day == INT64_MIN) return -1;

    /* newMoons[0] = civilDayNumber ≤ 冬至日的**最后一个**定朔（可正好是当天） */
    for (int guard = 0; day > winter_day; guard++) {
        if (guard > 8) return -1;
        day = new_moon_event_mode(c, jd_tt - SF_DAYS_PER_SYNODIC_MONTH,
                                  &jd_tt, low_for_day);
        if (day == INT64_MIN) return -1;
    }
    for (int guard = 0; guard <= 8; guard++) {
        double next_tt = 0.0;
        const int64_t next = new_moon_event_mode(c,
            jd_tt + SF_DAYS_PER_SYNODIC_MONTH, &next_tt, low_for_day);
        if (next == INT64_MIN) return -1;
        if (next > winter_day) break;
        day = next;
        jd_tt = next_tt;
    }

    out->new_moons[0].jd_ut = sf_tt_to_ut(jd_tt);
    out->new_moons[0].civil_day_number = day;
    for (unsigned i = 1; i < SF_CAL_NEW_MOON_COUNT; i++) {
        day = new_moon_event_mode(c, jd_tt + SF_DAYS_PER_SYNODIC_MONTH,
                                  &jd_tt, low_for_day);
        if (day == INT64_MIN) return -1;
        out->new_moons[i].jd_ut = sf_tt_to_ut(jd_tt);
        out->new_moons[i].civil_day_number = day;
    }
    out->new_moon_count = (uint8_t)SF_CAL_NEW_MOON_COUNT;
    return 0;
}

/* ------------------------------------------------------------------
 * 月序与闰月
 * ------------------------------------------------------------------ */
static int positive_mod(int v, int m)
{
    int r = v % m;
    return r < 0 ? r + m : r;
}

/* 序列号 → 月名。0 = 十一月（含冬至的那个月），1 = 十二月，2 = 正月，… */
static uint8_t month_number_from_sequence(int seq)
{
    static const uint8_t kMonthNumbers[12] = {
        11, 12, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    };
    return kMonthNumbers[positive_mod(seq, 12)];
}

/* 上游 calendar.cpp:697 的**核心**。返回 leap_index，并把 sequence 就地重编号。
 *
 * 读法：
 *   - 13 个月 iff newMoons[13] 的日号 ≤ 第二个冬至的日号（否则 12 个月）
 *   - 无中气之月为闰：从 k=1 起扫，第一个「不包含自己中气 solarTerms[2k]」
 *     的月就是闰月。k=0 永远包含冬至，不可能是闰月。
 *   - 从 leap_index 起全部 -1，于是闰月沿用**前一个月**的编号。
 *
 * 只可能有一个月缺中气（中气间距 29.4–31.5 天 vs 朔望月 29.53 天，
 * 一年内漂移 ≤ 11 天），所以「第一个失败的」就是「唯一失败的」。 */
static int resolve_physical_month_sequences(const sf_cal_year *out, int *seq)
{
    for (unsigned i = 0; i < SF_CAL_MONTH_COUNT; i++) seq[i] = (int)i;

    if (out->new_moons[13].civil_day_number > out->solar_terms[24].civil_day_number)
        return -1;                        /* 12 个月的岁，无闰 */

    int leap_index = 1;
    while (leap_index < 13
        && out->new_moons[leap_index + 1].civil_day_number
           > out->solar_terms[2 * leap_index].civil_day_number) {
        ++leap_index;
    }
    for (unsigned i = (unsigned)leap_index; i < SF_CAL_MONTH_COUNT; i++) seq[i] -= 1;
    return leap_index;
}

/* 上游 calendar.cpp:731。农历年的编号 = 正月到下一个正月这一段的**中点**
 * 所在的格里历年。窗口开头（第一个正月之前）的十一月/十二月归上一年。 */
static void assign_lunar_years(sf_cal_year *out)
{
    int starts[SF_CAL_MONTH_COUNT];
    int nstarts = 0;
    for (unsigned i = 0; i < SF_CAL_MONTH_COUNT; i++) {
        const sf_cal_month *m = &out->months[i];
        if (m->month == 1 && !m->is_leap && m->month_name != SF_MONTH_NAME_ALT_ONE)
            starts[nstarts++] = (int)i;
    }

    if (!nstarts) {
        /* 防御分支：窗口里没有正月。上游也有这条。 */
        for (unsigned i = 0; i < SF_CAL_MONTH_COUNT; i++) {
            int32_t y, mo, d;
            sf_solar_date_from_day_number(out->months[i].first_civil_day_number, &y, &mo, &d);
            const int32_t ly = (out->months[i].month >= 11) ? y - 1 : y;
            out->months[i].lunar_year = ly;
            out->months[i].historical_year = ly;
        }
        return;
    }

    int32_t first_year = 0;
    for (int b = 0; b < nstarts; b++) {
        const int first = starts[b];
        const int next = (b + 1 < nstarts) ? starts[b + 1] : (int)SF_CAL_MONTH_COUNT;
        const int64_t start_day = out->months[first].first_civil_day_number;
        const int64_t end_day = (b + 1 < nstarts)
            ? out->months[next].first_civil_day_number
            : start_day + 180;
        int32_t y, mo, d;
        sf_solar_date_from_day_number((start_day + end_day) / 2, &y, &mo, &d);
        if (b == 0) first_year = y;
        for (int i = first; i < next; i++) {
            out->months[i].lunar_year = y;
            out->months[i].historical_year = y;
        }
    }
    for (int i = 0; i < starts[0]; i++) {
        out->months[i].lunar_year = first_year - 1;
        out->months[i].historical_year = first_year - 1;
    }
}

/* ------------------------------------------------------------------
 * 月名：分两条路 —— 古历窗口走位置编号，其余走无中气规则
 * ------------------------------------------------------------------ */

/* 上游 calendar.cpp:777 assign_early_historical_months。
 *
 * 前 721 … 前 104 年**完全不用无中气规则**：月号是按「距该纪元年首多少个
 * 朔望月」数出来的，第十三个月由 offset >= 12 决定。
 *
 * 三个纪元分支（年首估计是 sxwnl 硬编码的经验式，无法化简）：
 *   ≥ -721  春秋       年首 = 正月，岁末闰月叫「十三月」
 *   ≥ -479  战国       同上，换纪元常数
 *   ≥ -220  秦汉颛顼历  年首 = **十月**（建亥），岁末闰月叫「后九月」
 */
static int assign_early_historical_months(const sf_cal_config *c, int year_hint,
                                          sf_cal_year *out)
{
    int seq[SF_CAL_MONTH_COUNT];
    (void)resolve_physical_month_sequences(out, seq);

    int64_t year_starts[3];
    int base_months[3], special_names[3];

    for (int i = 0; i < 3; i++) {
        const int hy = year_hint + i - 1;
        double estimate = NAN;
        if (hy >= -721) {
            estimate = 1457698.0
                + floor(0.342 + (hy + 721) * 12.368422) * SF_DAYS_PER_SYNODIC_MONTH;
            base_months[i] = 2; special_names[i] = SF_MONTH_NAME_THIRTEEN;
        }
        if (hy >= -479) {
            estimate = 1546083.0
                + floor(0.500 + (hy + 479) * 12.368422) * SF_DAYS_PER_SYNODIC_MONTH;
            base_months[i] = 2; special_names[i] = SF_MONTH_NAME_THIRTEEN;
        }
        if (hy >= -220) {
            estimate = 1640641.0
                + floor(0.866 + (hy + 220) * 12.369000) * SF_DAYS_PER_SYNODIC_MONTH;
            base_months[i] = 11; special_names[i] = SF_MONTH_NAME_LATER_NINE;
        }
        if (!isfinite(estimate)) return -1;
        year_starts[i] = assigned_event_day(c, SF_HIST_NEW_MOON, estimate);
    }

    for (unsigned i = 0; i < SF_CAL_MONTH_COUNT; i++) {
        int era = 2;
        while (era > 0 && out->new_moons[i].civil_day_number < year_starts[era]) era--;

        const int offset = (int)floor(
            (double)(out->new_moons[i].civil_day_number - year_starts[era] + 15)
            / SF_DAYS_PER_SYNODIC_MONTH);
        sf_cal_month *m = &out->months[i];

        /* 颛顼历建亥：写出来的农历年号比实际纪年**晚一年**。上游特意把这一步
         * 写成显式的 shift，而不是交给某个「冬至窗口中点」的启发式 —— 那个
         * 在改历边界上会在相邻两次 calcY() 之间摇摆，同一年分出两个同身份的月。 */
        const int winter_year_shift = (base_months[era] == 11) ? 1 : 0;
        m->historical_year = year_hint + era - 1;
        m->lunar_year = year_hint + era - 1 - winter_year_shift;
        m->month_building_branch = (uint8_t)positive_mod(seq[i], 12);

        if (offset < 12) {
            m->month = month_number_from_sequence(offset + base_months[era]);
        } else {
            m->month_name = (uint8_t)special_names[era];
            m->month = (special_names[era] == SF_MONTH_NAME_THIRTEEN) ? 13 : 9;
            m->is_leap = 1u;
        }
    }
    out->leap_month_index = -1;
    return 0;
}

/* 上游 calendar.cpp:867 assign_months。 */
static int assign_months(const sf_cal_config *c, sf_cal_year *out)
{
    const int historical = (c->mode == SF_CAL_CHINA_STANDARD_HISTORICAL);

    /* 第一个冬至所在的格里历年。+190/365.2422 ≈ +0.52 年，把 12 月下旬的
     * 冬至归进它自己那一年（否则岁首会被算到上一年去）。 */
    const int year_hint = (int)floor(
        ((double)out->solar_terms[0].civil_day_number - 2451545.0 + 190.0)
        / SF_DAYS_PER_TROPICAL_YEAR) + 2000;

    if (historical && year_hint >= -721 && year_hint <= -104)
        return assign_early_historical_months(c, year_hint, out);

    int seq[SF_CAL_MONTH_COUNT];
    const int leap_index = resolve_physical_month_sequences(out, seq);
    out->leap_month_index = (int8_t)leap_index;

    for (unsigned i = 0; i < SF_CAL_MONTH_COUNT; i++) {
        sf_cal_month *m = &out->months[i];
        const int s = seq[i];
        m->month_building_branch = (uint8_t)positive_mod(s, 12);
        m->month = month_number_from_sequence(s);
        m->is_leap = (leap_index >= 0 && (int)i == leap_index) ? 1u : 0u;

        if (!historical) continue;

        /* 以下历史改历规则按月首日号分别触发；各区间作为独立特例处理。 */
        const int64_t day = m->first_civil_day_number;

        if ((day >= 1724360 && day <= 1729794)
            || (day >= 1807724 && day <= 1808699)) {
            m->month = month_number_from_sequence(s + 1);   /* 新莽 / 景初 建丑 */
        } else if (day >= 1999349 && day <= 1999467) {
            m->month = month_number_from_sequence(s + 2);   /* 唐 建子 */
        } else if (day >= 1973067 && day <= 1977052) {
            if (s % 12 == 0) m->month = 1;                  /* 武周 建子 */
            if (s == 2) { m->month = 1; m->month_name = SF_MONTH_NAME_ALT_ONE; }
        }
        if (day == 1729794 || day == 1808699) {
            m->month = 12;
            m->month_name = SF_MONTH_NAME_ALT_TWELVE;       /* 建丑回改 */
        }
        /* 同名后月：同一个农历年里出现两个写法相同的月号。只打标记 ——
         * 既不当闰月，也不改显示名。 */
        if (day == 1977112 || day == 1999526)
            m->month_name = SF_MONTH_NAME_LATER_SAME_NAME;
    }

    assign_lunar_years(out);

    if (historical) {
        /* 颛顼历建亥那一段：年号比实际纪年晚一年 */
        for (unsigned i = 0; i < SF_CAL_MONTH_COUNT; i++) {
            const int64_t d = out->months[i].first_civil_day_number;
            if (d >= 1640641 && d < 1683490)
                out->months[i].historical_year = out->months[i].lunar_year + 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------
 * 组装一个岁
 * ------------------------------------------------------------------ */
static int cal_year_ut_impl(const sf_cal_config *c, double jd_ut,
                            sf_cal_year *out, int low_for_day)
{
    if (!config_valid(c) || !out || !isfinite(jd_ut)) return -1;
    memset(out, 0, sizeof *out);
    out->leap_month_index = -1;

    if (find_winter_solstice(c, jd_ut, &out->solar_terms[0], low_for_day) != 0) return -1;
    if (fill_solar_terms(c, out, low_for_day) != 0) return -1;
    if (fill_new_moons(c, out, low_for_day) != 0) return -1;

    /* 月长只可能是 29 或 30 —— 除了一处：景初历那个 28 天的月
     * (日号 1807696，公元 237-03-15)，且**只在 HISTORICAL 模式下**合法。
     * 上游把它写成硬编码的例外，这里照抄。 */
    for (unsigned i = 0; i < SF_CAL_MONTH_COUNT; i++) {
        const int64_t first = out->new_moons[i].civil_day_number;
        const int64_t count = out->new_moons[i + 1].civil_day_number - first;
        const int jingchu = (c->mode == SF_CAL_CHINA_STANDARD_HISTORICAL)
                         && count == 28 && first == 1807696;
        if (!jingchu && (count < 29 || count > 30)) return -1;
        out->months[i].first_civil_day_number = first;
        out->months[i].astronomical_new_moon_jd_ut = out->new_moons[i].jd_ut;
        out->months[i].day_count = (uint8_t)count;
        out->months[i].month_name = SF_MONTH_NAME_NORMAL;
    }

    if (assign_months(c, out) != 0) return -1;
    out->month_count = (uint8_t)SF_CAL_MONTH_COUNT;
    return 0;
}

int sf_cal_year_ut(const sf_cal_config *c, double jd_ut, sf_cal_year *out)
{
    /* 这个接口会把天文事件时刻暴露给调用方，所以保持完整求解。 */
    return cal_year_ut_impl(c, jd_ut, out, 0);
}

/* 日期换算只消费“事件归到哪一天”，不向调用方暴露内部的事件时刻。这里才允许
 * 用低项数预判 + 结构日界附近精算；不能拿它实现 sf_cal_year_ut。 */
static int cal_year_days_fast(const sf_cal_config *c, double jd_ut, sf_cal_year *out)
{
    return cal_year_ut_impl(c, jd_ut, out, 1);
}

/* ------------------------------------------------------------------
 * 查询
 * ------------------------------------------------------------------ */

/* 只有 firstCivilDayNumber < secondWinterSolsticeDayNumber 的月是可寻址的；
 * 最后一个月纯粹用来算前一个月的 day_count。上游三处查询都带这个 break。 */
static int month_addressable(const sf_cal_year *y, unsigned i)
{
    return y->months[i].first_civil_day_number < y->second_winter_solstice_day_number;
}

int sf_cal_from_solar(const sf_cal_config *c, const sf_solar_date *s,
                      sf_lunar_date *out)
{
    if (!config_valid(c) || !s || !out) return -1;
    if (s->month < 1 || s->month > 12 || s->day < 1 || s->day > 31) return -1;

    const int64_t target_day = sf_solar_day_number(s->year, s->month, s->day);
    sf_cal_year year;
    /* 反解出一个 UT 瞬时，使 civilDayNumber(它, structureOffset) == target_day */
    if (cal_year_days_fast(c, (double)target_day - structure_day_offset(c), &year) != 0)
        return -1;

    for (unsigned i = 0; i < SF_CAL_MONTH_COUNT; i++) {
        if (!month_addressable(&year, i)) break;
        const int64_t first = year.months[i].first_civil_day_number;
        if (target_day < first || target_day >= first + year.months[i].day_count)
            continue;
        memset(out, 0, sizeof *out);
        out->year = year.months[i].lunar_year;
        out->historical_year = year.months[i].historical_year;
        out->month = year.months[i].month;
        out->day = (uint8_t)(target_day - first + 1);
        out->is_leap = year.months[i].is_leap;
        out->month_days = year.months[i].day_count;
        out->month_name = year.months[i].month_name;
        return 0;
    }
    return -1;
}

int sf_cal_from_instant_ut(const sf_cal_config *c, double jd_ut, sf_lunar_date *out)
{
    if (!config_valid(c) || !out || !isfinite(jd_ut)) return -1;
    /* 这是**唯一**用 local_offset 的地方：先把瞬时落到观者的民用日，
     * 再拿这个日期去查（结构仍按 structure_offset 算） */
    const int64_t local_day = civil_day_number(jd_ut, local_day_offset(c));
    sf_solar_date sd;
    solar_date_from_day(local_day, &sd);
    return sf_cal_from_solar(c, &sd, out);
}

int sf_cal_from_lunar(const sf_cal_config *c, const sf_lunar_date *l,
                      sf_solar_date *out)
{
    if (!config_valid(c) || !l || !out) return -1;
    if (l->month < 1 || l->month > 13 || l->day < 1 || l->day > 30) return -1;

    /* 锚点取**该年 6 月 1 日 12:00**，再试 year+1：农历年跨公历年，
     * 一个锚点盖不住。照抄上游 chinese-calendar.js:522。 */
    for (int offset = 0; offset <= 1; offset++) {
        const double anchor = sf_julian_day_ut(l->year + offset, 6, 1, 12, 0, 0.0)
                            - structure_day_offset(c);
        sf_cal_year year;
        if (cal_year_days_fast(c, anchor, &year) != 0) return -1;

        for (unsigned i = 0; i < SF_CAL_MONTH_COUNT; i++) {
            if (!month_addressable(&year, i)) break;
            const sf_cal_month *m = &year.months[i];
            if (m->lunar_year != l->year || m->month != l->month
                || m->is_leap != (l->is_leap ? 1u : 0u)
                || m->month_name != l->month_name) continue;
            if (l->day > m->day_count) return -1;
            solar_date_from_day(m->first_civil_day_number + l->day - 1, out);
            return 0;
        }
    }
    return -1;
}

int sf_cal_month_days(const sf_cal_config *c, int32_t lunar_year,
                      uint8_t month, int is_leap, uint8_t *out_day_count)
{
    if (!config_valid(c) || !out_day_count) return -1;
    if (month < 1 || month > 13) return -1;

    /* 优先返回 NORMAL 名字的那个月的天数；只有找不到 NORMAL 才退回特殊的。
     * 原因是改历窗口里同一个月可能以不同 month_name 出现两次。 */
    int exceptional = -1;
    for (int offset = 0; offset <= 1; offset++) {
        const double anchor = sf_julian_day_ut(lunar_year + offset, 6, 1, 12, 0, 0.0)
                            - structure_day_offset(c);
        sf_cal_year year;
        if (cal_year_days_fast(c, anchor, &year) != 0) return -1;

        for (unsigned i = 0; i < SF_CAL_MONTH_COUNT; i++) {
            if (!month_addressable(&year, i)) break;
            const sf_cal_month *m = &year.months[i];
            if (m->lunar_year != lunar_year || m->month != month) continue;
            if (m->is_leap != (is_leap ? 1u : 0u)) continue;
            if (m->month_name == SF_MONTH_NAME_NORMAL) {
                *out_day_count = m->day_count;
                return 0;
            }
            if (exceptional < 0) exceptional = m->day_count;
        }
    }
    if (exceptional >= 0) { *out_day_count = (uint8_t)exceptional; return 0; }
    return -1;
}

/* ------------------------------------------------------------------
 * 节气查询
 * ------------------------------------------------------------------ */

/* 黄经步长 ↔ 冬至起算的节气序号。上游两处换算的逆：
 *   evaluate_standalone_solar_term 里 longitude_step = posmod(term_id - 5, 24) */
static int term_id_from_longitude_step(int step)
{
    return positive_mod(step + 5, 24);
}

int sf_cal_get_specific_term(const sf_cal_config *c, int32_t civil_year,
                             uint8_t term_index_from_vernal_equinox,
                             sf_cal_term_event *out)
{
    if (!config_valid(c) || !out || term_index_from_vernal_equinox >= 24) return -1;

    /* 照抄上游：先拿该年 6 月 1 日定位那个冬至，再往后推 */
    const double anchor = sf_julian_day_ut(civil_year, 6, 1, 0, 0, 0.0);
    sf_cal_term_event winter;
    if (find_winter_solstice(c, anchor, &winter, 0) != 0) return -1;

    /* 0=春分 是冬至之后第 6 个；18=冬至 是第 0 个。
     * 19..23 是**同一公历年** 1–3 月的节气，所以是 19-18=1 … 23-18=5。 */
    const int idx = (term_index_from_vernal_equinox >= 19)
        ? (int)term_index_from_vernal_equinox - 18
        : (int)term_index_from_vernal_equinox + 6;

    double jd_tt = 0;
    const int64_t day = term_event(c, idx,
        sf_ut_to_tt(winter.jd_ut) + (double)idx * SF_DAYS_PER_SOLAR_TERM, &jd_tt);
    if (day == INT64_MIN) return -1;

    double target = fmod((270.0 + 15.0 * idx) * SF_DEG, SF_TWO_PI);
    if (target < 0.0) target += SF_TWO_PI;

    memset(out, 0, sizeof *out);
    out->index_from_winter_solstice = (uint8_t)(idx % 24);
    out->target_longitude_rad = target;
    out->jd_ut = sf_tt_to_ut(jd_tt);
    out->civil_day_number = day;
    return 0;
}

int sf_cal_find_term(const sf_cal_config *c, double jd_ut, int next,
                     int filter, sf_cal_term_event *out)
{
    if (!config_valid(c) || !out || !isfinite(jd_ut)) return -1;
    if (filter != SF_TERM_ANY && filter != SF_TERM_JIE && filter != SF_TERM_QI)
        return -1;

    const double jd_tt = sf_ut_to_tt(jd_ut);
    const double lon = sf_sun_apparent_longitude(jd_tt);
    if (!isfinite(lon)) return -1;

    double norm = fmod(lon, SF_TWO_PI);
    if (norm < 0.0) norm += SF_TWO_PI;
    const int approx = (int)floor(norm / (M_PI / 12.0));

    /* 近似步长两侧各找 3 个：筛子可能跳过一整类，得留余量 */
    int found = 0;
    sf_cal_term_event best;
    for (int off = -3; off <= 3; off++) {
        const int step = approx + off;
        const int term_id = term_id_from_longitude_step(step);
        if (filter == SF_TERM_JIE && (term_id & 1) != 0) continue;
        if (filter == SF_TERM_QI  && (term_id & 1) == 0) continue;

        const double target = (double)positive_mod(step, 24) * (M_PI / 12.0);
        double cand_tt = 0;
        const int64_t cand_day = term_event_at(
            c, target, jd_tt + (double)off * SF_DAYS_PER_SOLAR_TERM, &cand_tt);
        if (cand_day == INT64_MIN) return -1;
        const double cand_ut = sf_tt_to_ut(cand_tt);

        /* 注意：边界语义：往前**含**正好落在 jd_ut 上的那个，往后不含。
         * 月柱在节气当天换柱就靠这一条。容差 1e-10 天，是求解器的
         * 「同一个根」地板，不是民用时间的窗口。 */
        const double diff = cand_ut - jd_ut;
        const int valid = next ? (diff > 1e-10) : (diff <= 1e-10);
        if (!valid) continue;
        if (found && (next ? cand_ut >= best.jd_ut : cand_ut <= best.jd_ut)) continue;

        found = 1;
        best.index_from_winter_solstice = (uint8_t)positive_mod(term_id + 1, 24);
        best.target_longitude_rad = target;
        best.jd_ut = cand_ut;
        best.civil_day_number = cand_day;
    }
    if (!found) return -1;
    *out = best;
    return 0;
}
