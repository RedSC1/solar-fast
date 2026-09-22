/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_check.c —— solar-fast 的验证驱动
 *
 * 模式
 *   raw   <jd>                单点，15 位有效数字（跨平台逐位核对用）
 *   pos   <jd_lo> <jd_hi> <n>  位置级：每点采样的误差统计
 *   ev    <y0> <y1> [ystep]    事件级：逐年 24 气 + 13 朔，误差以秒计
 *   truth [stride]             对 DE441 真值 CSV（逐日 730501 点）
 *
 * 用 -DSF_HAVE_BASE 编译时会把 fl_qishuo.c 一起链进来当基线，
 * pos / ev 就变成「增量模式」——量的是定点化引入的那部分误差，
 * 比与 DE441 比更能告到小量级（DE441 那边含着模型自身的 1.8 s）。
 * ================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sf_qishuo.h"
#include <math.h>

#include "solar_fast.h"
#include "sf_data.h"      /* 诊断模式要直接扫 *PC 预算表 */

#ifdef SF_HAVE_BASE
#include "fl_qishuo.h"
#endif

#define ARCSEC (180.0 * 3600.0 / M_PI)
#define RAD2DEG (180.0 / M_PI)

/* 真值 CSV（DE441 经 taiyin-ephemeris）。仓库里**不带**这个文件，
 * 需要时自己生成或用 -DSF_TRUTH_CSV=/path/to/ty_sun_full.csv 指定。
 * 格式：jd_tt,T,ty_geom_deg,ty_app_deg,ty_eq_ra_deg,ty_eq_dec_deg,ty_R_au,ty_geom1976_deg */
#ifndef SF_TRUTH_CSV
#define SF_TRUTH_CSV "test/truth.csv"
#endif

static double wrap180(double d)
{
    double v = fmod(d + 180.0, 360.0);
    if (v < 0.0) v += 360.0;
    return v - 180.0;
}

#ifdef SF_HAVE_BASE
static double wrap_pi(double a)
{
    return atan2(sin(a), cos(a));
}
#endif

/* ---- 统计 ---- */
typedef struct { double mean, rms, p95, p99, maxv; long n; } stat_t;

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static stat_t stats(double *v, long n)
{
    stat_t s;
    memset(&s, 0, sizeof s);
    s.n = n;
    if (n <= 0) return s;
    double sum = 0.0, sq = 0.0, mx = 0.0;
    for (long i = 0; i < n; i++) {
        sum += v[i];
        sq += v[i] * v[i];
        if (fabs(v[i]) > mx) mx = fabs(v[i]);
    }
    s.mean = sum / n;
    s.rms = sqrt(sq / n);
    s.maxv = mx;
    qsort(v, n, sizeof(double), cmp_d);
    s.p95 = v[(long)(0.95 * (n - 1))];
    s.p99 = v[(long)(0.99 * (n - 1))];
    return s;
}

static void pr(const char *tag, stat_t s, const char *unit)
{
    printf("  %-10s mean=%+.4e rms=%.4e p95=%.4e p99=%.4e max=%.4e %s\n",
           tag, s.mean, s.rms, s.p95, s.p99, s.maxv, unit);
}

/* ================================================================== */
static void mode_raw(double jd)
{
    double ra, dec;
    printf("RAW jd=%.15f\n", jd);
    printf("  apparent_longitude %.15f rad = %.12f deg\n",
           sf_sun_apparent_longitude(jd), sf_sun_apparent_longitude(jd) * RAD2DEG);
    printf("  apparent_latitude  %.15f rad = %.9f arcsec\n",
           sf_sun_apparent_latitude(jd), sf_sun_apparent_latitude(jd) * ARCSEC);
    printf("  distance           %.15f AU\n", sf_sun_distance(jd));
    sf_sun_ra_dec(jd, &ra, &dec);
    printf("  ra                 %.15f rad = %.12f deg\n", ra, ra * RAD2DEG);
    printf("  dec                %.15f rad = %.12f deg\n", dec, dec * RAD2DEG);
    printf("  elongation         %.15f rad = %.12f deg\n",
           sf_elongation(jd), sf_elongation(jd) * RAD2DEG);
#ifdef SF_HAVE_BASE
    printf("  BASE Lsolar        %.15f   elong %.15f\n",
           fl_solar_longitude(jd), fl_elongation(jd));
#endif
}

/* 低项数气朔的边界契约：有效窗外必须逐位退回完整算法。 */
static void mode_low_contract(void)
{
    static const double Y[] = { -7000.0, 11000.0 };
    int fail = 0;
    for (unsigned i = 0; i < sizeof Y / sizeof Y[0]; i++) {
        const double t = (Y[i] - 2000.0) * 365.25 / 36525.0;
        const double qa = 1.75347 + M_PI + 628.3319653318 * t;
        const double sa = 7771.37714500204 * t - 1.08472;
        const double q0 = sf_solar_term_time(qa);
        const double q1 = sf_solar_term_time_low(qa);
        const double s0 = sf_lunar_phase_time(sa);
        const double s1 = sf_lunar_phase_time_low(sa);
        if (!(q0 == q1) || !(s0 == s1)) {
            fprintf(stderr, "LOW fallback failed at year %.0f: qi %.17g/%.17g shuo %.17g/%.17g\n",
                    Y[i], q0, q1, s0, s1);
            fail++;
        }
    }
    /* 窗内冒烟：两条低项路径都要给有限值。 */
    if (!isfinite(sf_solve_solar_term_low(0.0, 2451545.0))
        || !isfinite(sf_solve_lunar_phase_low(0.0, 2451545.0))) fail++;
    printf("LOW contract: outside-range exact fallback, failures=%d\n", fail);
    if (fail) exit(1);
}

/* ================================================================== */
static void mode_pos(double lo, double hi, long n)
{
    double *dm = malloc(sizeof(double) * n);   /* 同预算增量 */
    double *dd = malloc(sizeof(double) * n);   /* 定点增量（锁 nut=10、R 全量）*/
    double *de = malloc(sizeof(double) * n);   /* 距角增量 */
    double *dn = malloc(sizeof(double) * n);   /* 刻意的章动差 */
    if (!dm || !dd || !de || !dn) { fprintf(stderr, "oom\n"); exit(1); }

    for (long i = 0; i < n; i++) {
        const double jd = lo + (hi - lo) * (double)i / (double)(n - 1);
#ifdef SF_HAVE_BASE
        /* 同预算比：两边都用 EarthL "129"(208项) / EarthB 全量 / EarthR "30"(107项)。
         * 这样剩下的差就纯粹是定点内层引入的。 */
        dm[i] = wrap_pi(sf_sun_apparent_longitude_budget(
                            jd, SF_EARTH_L_BUD_129, 10,
                            SF_EARTH_B_BUD_FULL, SF_EARTH_R_BUD_59)
                        - fl_solar_longitude_budget(
                            jd,
                            5,   /* FL_EARTH_L_BUD_129 —— 该枚举是 fl_qishuo.c 私有的，
                                  * 头文件没暴露，只能按行索引给：PC 表第 5 行 = 208 项 */
                            10,
                            1,   /* FL_EARTH_B_BUD_FULL = 50 项 */
                            1)); /* FL_EARTH_R_BUD_30   = 107 项 */
        /* 默认档比：两边都锁 nut=10 且 R 都用全量，这样剩下的差才是定点内层的。
         * 注意：不得直接调 sf_sun_apparent_longitude(jd) —— 它现在用 SF_DEF_NUT=20，
         * 那 0.076″ 的章动差会盖掉要量的 0.006″ 定点增量。 */
        dd[i] = wrap_pi(sf_sun_apparent_longitude_budget(
                            jd, SF_EARTH_L_BUD_FULL, 10,
                            SF_EARTH_B_BUD_FULL, SF_EARTH_R_BUD_FULL)
                        - fl_solar_longitude_budget(jd, 6, 10, 1, 2));
        /* 刻意的章动差（10 项 vs SF_DEF_NUT 项），单独报，不要混进上面 */
        dn[i] = wrap_pi(sf_sun_apparent_longitude_budget(
                            jd, SF_DEF_L_BUD, SF_DEF_NUT, SF_DEF_B_BUD, SF_DEF_R_BUD)
                        - sf_sun_apparent_longitude_budget(
                            jd, SF_DEF_L_BUD, 10, SF_DEF_B_BUD, SF_DEF_R_BUD));
        /* 距角同预算对照：两边都用 EarthL 全量，隔离出定点增量。
         * 索引 2=MOON_L full, 6=EARTH_L full, 1=MOON_B, 1=EARTH_B full, 1=EARTH_R 107
         *
         * 注意：月亮 B 档取 SF_MOON_B_BUD_7 —— SF_MOON_B_BUD_10 在 4021d7f
         * 重建 MOON_B 预算表后已不存在。档位对结果无影响：月球那组帧投影行里
         * tanβ 的系数是 0（纯黄道面内旋转），β 不进距角，实测 7 项换 277 项
         * 距角差恰好 0.00″（见 sf_series.c 的 sf_elongation_budget）。 */
        de[i] = wrap_pi(sf_elongation_budget(jd, SF_MOON_L_BUD_FULL, SF_EARTH_L_BUD_FULL,
                                             SF_MOON_B_BUD_7, SF_EARTH_B_BUD_FULL,
                                             SF_EARTH_R_BUD_59)
                        - fl_elongation_budget(jd, 2, 6, 1, 1, 1));
#else
        (void)jd;
        dm[i] = dd[i] = de[i] = dn[i] = 0.0;
#endif
    }
    printf("POS n=%ld jd=%.0f..%.0f\n", n, lo, hi);
#ifdef SF_HAVE_BASE
    {   /* 最差 3 个日期（定位孤立爆点用）*/
        for (int k = 0; k < 3; k++) {
            long bi = -1; double bv = -1;
            for (long i = 0; i < n; i++) if (fabs(dd[i]) > bv) { bv = fabs(dd[i]); bi = i; }
            if (bi < 0) break;
            printf("  worst#%d  jd=%.1f  |Δ|=%.4e rad (%.2f″)  D=%.0f  frac=%.6f\n",
                   k + 1, lo + (hi - lo) * (double)bi / (double)(n - 1), bv, bv * ARCSEC,
                   lo + (hi - lo) * (double)bi / (double)(n - 1) - 2451545.0,
                   fmod(lo + (hi - lo) * (double)bi / (double)(n - 1), 1.0));
            dd[bi] = 0.0;
        }
    }
    for (long i = 0; i < n; i++) { dm[i] *= ARCSEC; dd[i] *= ARCSEC; de[i] *= ARCSEC; dn[i] *= ARCSEC; }
    /* solar_fix：两边都是 L386/B50/R 全量、nut=10（表内项集完全相同，只是组内
     * 顺序不同，而 int64 组内累加与顺序无关）→ 这**就是**定点内层的净增量。 */
    pr("solar_fix", stats(dd, n), "arcsec  (L386/B50/R 全量 + nut10 两边同：定点内层净增量)");
    pr("solar_bud", stats(dm, n), "arcsec  (L129 vs 基线 L208：档位差，不是定点差)");
    pr("elong_fix", stats(de, n), "arcsec  (距角 L386 两边全量：定点内层的净增量)");
    pr("nut_diff",  stats(dn, n), "arcsec  (刻意的章动差：SF_DEF_NUT 项 vs 10 项，非误差)");
    free(dn);
#else
    printf("  (未定义 SF_HAVE_BASE，pos 不报误差)\n");
    pr("solar", stats(dm, n), "rad");
#endif
    free(dm); free(dd); free(de);
}

/* ================================================================== */
static void mode_ev(int y0, int y1, int ystep, int lbud)
{
    const int ny = (y1 - y0) / ystep + 1;
    double *dv = malloc(sizeof(double) * ny * 24);
    double *dp = malloc(sizeof(double) * ny * 13);
    long nt = 0, np = 0;
    int failt = 0, failp = 0;

    for (int y = y0; y <= y1; y += ystep) {
        double t24[24], p13[13];
#ifdef SF_HAVE_BASE
        (void)t24; (void)p13;
        const double jd0 = 2451545.0 + ((double)y - 2000.0) * 365.2425;
        for (int i = 0; i < 24; i++) {
            const double target = i * (2.0 * M_PI / 24.0);
            const double seed = jd0 + i * 15.2;
            const double rb = fl_solve_solar_longitude_fast(target, seed);
            const double rx = sf_solve_solar_term_budget(target, seed, lbud,
                                                         SF_DEF_B_BUD, SF_DEF_R_BUD);
            if (!isfinite(rb) || !isfinite(rx)) { failt++; continue; }
            dv[nt++] = fabs(rx - rb) * 86400.0;
        }
        for (int i = 0; i < 13; i++) {
            const double target = i * (2.0 * M_PI / 12.365);
            const double seed = jd0 + i * 29.53;
            const double rb = fl_solve_lunar_phase_fast(target, seed);
            const double rx = sf_solve_lunar_phase(target, seed);
            if (!isfinite(rb) || !isfinite(rx)) { failp++; continue; }
            dp[np++] = fabs(rx - rb) * 86400.0;
        }
#else
        /* 没有基线可比：只把这一年的气/朔时刻收进来报条数，兼当
         * sf_qishuo 的最小冒烟。注意：默认相位角是 {0}，所以只有朔、没有望。 */
        (void)t24; (void)p13;
        (void)failt; (void)failp; (void)lbud;
        {
            sf_qishuo_options o;
            sf_qishuo_options_init(&o);
            static sf_qishuo_event qev[SF_QISHUO_MAX_EVENTS];
            sf_qishuo_year qy;
            if (sf_qishuo_year_run(&o, y, qev, SF_QISHUO_MAX_EVENTS, &qy)
                == SF_QISHUO_OK) {
                for (int i = 0; i < qy.count; i++) {
                    if (qev[i].kind == SF_QISHUO_KIND_SOLAR_TERM)
                        dv[nt++] = qev[i].jd_tt;
                    else if (qev[i].kind == SF_QISHUO_KIND_LUNAR_PHASE)
                        dp[np++] = qev[i].jd_tt;
                }
            }
        }
#endif
    }
#ifdef SF_HAVE_BASE
    printf("EV years=%d..%d step=%d fail(term=%d phase=%d)\n", y0, y1, ystep, failt, failp);
    if (nt > 0 && np > 0) {
        const stat_t st = stats(dv, nt), sp = stats(dp, np);
        printf("  %-10s mean=%9.4f rms=%9.4f p95=%9.4f p99=%9.4f max=%9.4f s  n=%ld\n",
               "term24", st.mean, st.rms, st.p95, st.p99, st.maxv, nt);
        printf("  %-10s mean=%9.4f rms=%9.4f p95=%9.4f p99=%9.4f max=%9.4f s  n=%ld\n",
               "phase", sp.mean, sp.rms, sp.p95, sp.p99, sp.maxv, np);
    }
#else
    printf("EV years=%d..%d  次数=%ld 朔数=%ld\n", y0, y1, nt, np);
    printf("  (未定义 SF_HAVE_BASE：只做有限性检查，不报误差)\n");
#endif
    free(dv); free(dp);
}

/* ==================================================================
 * 档位曲线：扫某张预算表的**每一行**，对真值报误差。
 * 用来在同等项数下比较「上游手调配额档」与「贪心重排的嵌套档」。
 * ================================================================== */
static void mode_lcurve(long stride)
{
    FILE *f = fopen(SF_TRUTH_CSV, "r");
    if (!f) { fprintf(stderr, "  跳过：读不到 %s\n", SF_TRUTH_CSV); return; }
    char line[512];
    if (!fgets(line, sizeof line, f)) { fprintf(stderr, "空文件\n"); exit(1); }

    long cap = 800000;
    double *jdv = malloc(sizeof(double) * cap);
    double *alon = malloc(sizeof(double) * cap);
    double *glonv = malloc(sizeof(double) * cap);
    long n = 0, seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (seen++ % stride) continue;
        double jd, T, glon, al, ra_d, dec_d, R, g1976;
        if (sscanf(line, "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf",
                   &jd, &T, &glon, &al, &ra_d, &dec_d, &R, &g1976) != 8) continue;
        if (n >= cap) break;
        jdv[n] = jd; alon[n] = al; glonv[n] = glon; n++;
    }
    fclose(f);

    double *v = malloc(sizeof(double) * n);
    printf("LCURVE n=%ld (stride=%ld)  —— 变 EARTH_L 档，B/R 固定默认\n", n, stride);
    printf("  %-10s %6s %18s %18s\n", "档位", "项数", "几何黄经 rms", "几何黄经 max");

    for (int li = 0; li < SF_EARTH_L_NBUD; li++) {
        int cnt = 0;
        for (int g = 0; g < SF_EARTH_L_NG; g++) cnt += SF_EARTH_L_PC[li][g];
        for (long i = 0; i < n; i++) {
            const double lam = sf_sun_geometric_longitude_budget(
                                   jdv[i], li, SF_DEF_B_BUD, SF_DEF_R_BUD) * RAD2DEG;
            v[i] = wrap180(lam - glonv[i]) * 3600.0;
        }
        const stat_t s = stats(v, n);
        printf("  %-10d %6d %18.4f %18.4f\n", li, cnt, s.rms, s.maxv);
    }
    free(jdv); free(alon); free(glonv); free(v);
}

/* ==================================================================
 * oracle <csv> [l_bud b_bud r_bud]
 *
 * 读 taiyin-ephemeris 直接读 DE441 生成的几何位置表
 * （tools/oracle_sun.cpp，格式 jd_tt,geom_lon_deg,geom_lat_deg,R_au），
 * 按「距 J2000 多远」分桶报误差，用来看远期退化。
 * ================================================================== */
static void mode_oracle(const char *path, int lbud, int bbud, int rbud)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "打不开 %s\n", path); exit(1); }

    enum { NB = 9 };
    static const double edge[NB + 1] = {
        -2922000, -2191500, -1461000, -730500, 0, 730500, 1461000, 2191500, 2922000, 2922001
    };
    double *bl[NB], *bb[NB], *br[NB];
    long bn[NB];
    for (int b = 0; b < NB; b++) {
        bl[b] = malloc(sizeof(double) * 4096);
        bb[b] = malloc(sizeof(double) * 4096);
        br[b] = malloc(sizeof(double) * 4096);
        bn[b] = 0;
    }

    char line[512];
    long n = 0;
    double worst = 0; double worst_jd = 0;
    double alll_rms = 0, allb_rms = 0, allr_rms = 0;

    while (fgets(line, sizeof line, f)) {
        double jd, lon, lat, R;
        if (sscanf(line, "%lf,%lf,%lf,%lf", &jd, &lon, &lat, &R) != 4) continue;
        const double dl = wrap180(sf_sun_geometric_longitude_budget(jd, lbud, bbud, rbud)
                                  * RAD2DEG - lon) * 3600.0;
        const double db = (sf_sun_apparent_latitude_budget(jd, lbud, SF_DEF_NUT, bbud, rbud)
                           * RAD2DEG - lat) * 3600.0;
        const double dr = (sf_sun_distance_budget(jd, rbud) - R) * 1.495978707e8;
        n++;
        alll_rms += dl * dl; allb_rms += db * db; allr_rms += dr * dr;
        if (fabs(dl) > worst) { worst = fabs(dl); worst_jd = jd; }
        const double d = jd - 2451545.0;
        for (int b = 0; b < NB; b++) {
            if (d >= edge[b] && d < edge[b + 1] && bn[b] < 4096) {
                bl[b][bn[b]] = dl; bb[b][bn[b]] = db; br[b][bn[b]] = dr; bn[b]++;
                break;
            }
        }
    }
    fclose(f);

    printf("ORACLE %s  n=%ld  L_bud=%d B_bud=%d R_bud=%d\n", path, n, lbud, bbud, rbud);
    printf("  总体  几何黄经 rms=%.4f″  几何黄纬 rms=%.4f″  距离 rms=%.1f km   (黄经 max=%.4f″ @jd %.0f)\n",
           sqrt(alll_rms / n) * 1.0, sqrt(allb_rms / n), sqrt(allr_rms / n), worst, worst_jd);
    printf("  %-20s %-7s %12s %12s %14s\n", "距 J2000 的区间(儒略年)", "点数", "黄经 rms″", "黄纬 rms″", "距离 rms km");
    for (int b = 0; b < NB; b++) {
        if (bn[b] == 0) continue;
        const stat_t sl = stats(bl[b], bn[b]), sb = stats(bb[b], bn[b]), sr = stats(br[b], bn[b]);
        char tag[40];
        snprintf(tag, sizeof tag, "%+6.0f .. %+6.0f",
                 edge[b] / 365.25, edge[b + 1] > 2922000 ? 8000 : edge[b + 1] / 365.25);
        printf("  %-20s %-7ld %12.4f %12.4f %14.1f\n", tag, bn[b], sl.rms, sb.rms, sr.rms);
    }
    for (int b = 0; b < NB; b++) { free(bl[b]); free(bb[b]); free(br[b]); }
}

/* ================================================================== */
static void mode_truth(long stride, int lbud, int bbud, int rbud)
{
    FILE *f = fopen(SF_TRUTH_CSV, "r");
    if (!f) { fprintf(stderr, "  跳过：读不到 %s\n", SF_TRUTH_CSV); return; }

    char line[512];
    if (!fgets(line, sizeof line, f)) { fprintf(stderr, "空文件\n"); exit(1); }

    long cap = 800000;
    double *dl  = malloc(sizeof(double) * cap);   /* 视黄经 */
    double *dg  = malloc(sizeof(double) * cap);   /* 几何黄经 */
    double *dk  = malloc(sizeof(double) * cap);   /* R 误差，km */
    double *dra = malloc(sizeof(double) * cap);
    double *ddc = malloc(sizeof(double) * cap);
    if (!dl || !dg || !dk || !dra || !ddc) { fprintf(stderr, "oom\n"); exit(1); }
    long n = 0, seen = 0;

    while (fgets(line, sizeof line, f)) {
        if (seen++ % stride) continue;
        double jd, T, glon, alon, ra_d, dec_d, R, g1976;
        if (sscanf(line, "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf",
                   &jd, &T, &glon, &alon, &ra_d, &dec_d, &R, &g1976) != 8) continue;
        if (n >= cap) break;

        const double lam = sf_sun_apparent_longitude_budget(jd, lbud, SF_DEF_NUT, bbud, rbud) * RAD2DEG;
        const double gmt = sf_sun_geometric_longitude_budget(jd, lbud, bbud, rbud) * RAD2DEG;
        const double rr  = sf_sun_distance_budget(jd, rbud);
        double ra, dec;
        sf_sun_ra_dec_budget(jd, &ra, &dec, lbud, SF_DEF_NUT, bbud, rbud);

        dl[n]  = wrap180(lam - alon) * 3600.0;
        dg[n]  = wrap180(gmt - glon) * 3600.0;
        dk[n]  = (rr - R) * 1.495978707e8;                    /* AU -> km */
        dra[n] = wrap180(ra * RAD2DEG - ra_d) * 3600.0 * cos(dec_d / RAD2DEG);
        ddc[n] = (dec * RAD2DEG - dec_d) * 3600.0;
        n++;
    }
    fclose(f);

    printf("TRUTH n=%ld (stride=%ld) L_bud=%d B_bud=%d R_bud=%d\n",
           n, stride, lbud, bbud, rbud);
    pr("geom",   stats(dg, n),  "arcsec  <- 对 ty_geom_deg（DE441 几何黄经）");
    pr("lambda", stats(dl, n),  "arcsec  <- 对 ty_app_deg（视黄经）");
    pr("RAcosd", stats(dra, n), "arcsec");
    pr("dec",    stats(ddc, n), "arcsec");
    pr("R",      stats(dk, n),  "km");
    free(dl); free(dg); free(dk); free(dra); free(ddc);
}

/* 预算扫描：给出各档的定位精度，用来定默认档 */
static void mode_sweep(long stride)
{
    static const struct { int l, b, r; const char *tag; } cfg[] = {
        /* ---- 黄经曲线（B、R 固定）---- */
        { SF_EARTH_L_BUD_16,   SF_EARTH_B_BUD_FULL,  SF_EARTH_R_BUD_16,   "L16   B50 R16 " },
        { SF_EARTH_L_BUD_32,   SF_EARTH_B_BUD_FULL,  SF_EARTH_R_BUD_16,   "L32   B50 R16 " },
        { SF_EARTH_L_BUD_64,   SF_EARTH_B_BUD_FULL,  SF_EARTH_R_BUD_16,   "L64   B50 R16 " },
        { SF_EARTH_L_BUD_96,   SF_EARTH_B_BUD_FULL,  SF_EARTH_R_BUD_16,   "L96   B50 R16 " },
        { SF_EARTH_L_BUD_129,  SF_EARTH_B_BUD_FULL,  SF_EARTH_R_BUD_16,   "L129  B50 R16 " },
        { SF_EARTH_L_BUD_192,  SF_EARTH_B_BUD_FULL,  SF_EARTH_R_BUD_16,   "L192  B50 R16 " },
        { SF_EARTH_L_BUD_FULL, SF_EARTH_B_BUD_FULL,  SF_EARTH_R_BUD_16,   "L386  B50 R16 " },
        /* ---- 黄纬曲线 ---- */
        { SF_EARTH_L_BUD_FULL, SF_EARTH_B_BUD_7,     SF_EARTH_R_BUD_16,   "L386  B7  R16 " },
        { SF_EARTH_L_BUD_FULL, SF_EARTH_B_BUD_16,    SF_EARTH_R_BUD_16,   "L386  B16 R16 " },
        { SF_EARTH_L_BUD_FULL, SF_EARTH_B_BUD_32,    SF_EARTH_R_BUD_16,   "L386  B32 R16 " },
        /* ---- 距离：只有近全量才准 ---- */
        { SF_EARTH_L_BUD_FULL, SF_EARTH_B_BUD_FULL,  SF_EARTH_R_BUD_FULL, "L386  B50 R475" },
        /* ---- 对齐 NREL SPA 的项数 L129 / B7 / R59 ---- */
        { SF_EARTH_L_BUD_129,  SF_EARTH_B_BUD_7,     SF_EARTH_R_BUD_59,   "SPA L129B7R59 " },
    };
    const int NC = (int)(sizeof cfg / sizeof cfg[0]);

    FILE *f = fopen(SF_TRUTH_CSV, "r");
    if (!f) { fprintf(stderr, "  跳过：读不到 %s\n", SF_TRUTH_CSV); return; }
    char line[512];
    if (!fgets(line, sizeof line, f)) { fprintf(stderr, "空文件\n"); exit(1); }

    long cap = 800000;
    double *jdv = malloc(sizeof(double) * cap);
    double *alon = malloc(sizeof(double) * cap);
    double *rad = malloc(sizeof(double) * cap);
    double *decd = malloc(sizeof(double) * cap);
    double *Rv = malloc(sizeof(double) * cap);
    long n = 0, seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (seen++ % stride) continue;
        double jd, T, glon, al, ra_d, dec_d, R, g1976;
        if (sscanf(line, "%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf",
                   &jd, &T, &glon, &al, &ra_d, &dec_d, &R, &g1976) != 8) continue;
        if (n >= cap) break;
        jdv[n] = jd; alon[n] = al; rad[n] = ra_d; decd[n] = dec_d; Rv[n] = R;
        n++;
    }
    fclose(f);

    double *v = malloc(sizeof(double) * n);
    printf("SWEEP n=%ld (stride=%ld)\n", n, stride);
    printf("  %-16s %18s %18s %18s %16s\n",
           "预算", "黄经 rms/max", "RAcosD rms/max", "赤纬 rms/max", "R rms/max");
    for (int c = 0; c < NC; c++) {
        stat_t sl, sa, sd, sr;
        for (long i = 0; i < n; i++) {
            const double lam = sf_sun_apparent_longitude_budget(jdv[i], cfg[c].l, SF_DEF_NUT, cfg[c].b, cfg[c].r) * RAD2DEG;
            double ra, dec;
            sf_sun_ra_dec_budget(jdv[i], &ra, &dec, cfg[c].l, SF_DEF_NUT, cfg[c].b, cfg[c].r);
            v[i] = wrap180(lam - alon[i]) * 3600.0;
        }
        sl = stats(v, n);
        for (long i = 0; i < n; i++) {
            double ra, dec;
            sf_sun_ra_dec_budget(jdv[i], &ra, &dec, cfg[c].l, SF_DEF_NUT, cfg[c].b, cfg[c].r);
            v[i] = wrap180(ra * RAD2DEG - rad[i]) * 3600.0 * cos(decd[i] / RAD2DEG);
        }
        sa = stats(v, n);
        for (long i = 0; i < n; i++) {
            double ra, dec;
            sf_sun_ra_dec_budget(jdv[i], &ra, &dec, cfg[c].l, SF_DEF_NUT, cfg[c].b, cfg[c].r);
            v[i] = (dec * RAD2DEG - decd[i]) * 3600.0;
        }
        sd = stats(v, n);
        for (long i = 0; i < n; i++)
            v[i] = (sf_sun_distance_budget(jdv[i], cfg[c].r) - Rv[i]) * 1.495978707e8;
        sr = stats(v, n);
        printf("  %-16s %8.4f/%8.4f %8.4f/%8.4f %8.4f/%8.4f %8.1f/%8.1f\n",
               cfg[c].tag, sl.rms, sl.maxv, sa.rms, sa.maxv,
               sd.rms, sd.maxv, sr.rms, sr.maxv);
    }
    free(jdv); free(alon); free(rad); free(decd); free(Rv); free(v);
}

/* ================================================================== */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s raw <jd> | low | pos <lo> <hi> <n> | ev <y0> <y1> [step] | truth [stride]\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "raw") && argc >= 3)
        mode_raw(atof(argv[2]));
    else if (!strcmp(argv[1], "low"))
        mode_low_contract();
    else if (!strcmp(argv[1], "pos") && argc >= 5)
        mode_pos(atof(argv[2]), atof(argv[3]), atol(argv[4]));
    else if (!strcmp(argv[1], "ev") && argc >= 4)
        mode_ev(atoi(argv[2]), atoi(argv[3]), argc >= 5 ? atoi(argv[4]) : 1,
                argc >= 6 ? atoi(argv[5]) : SF_DEF_L_BUD);
    else if (!strcmp(argv[1], "truth"))
        mode_truth(argc >= 3 ? atol(argv[2]) : 1,
                   argc >= 4 ? atoi(argv[3]) : SF_DEF_L_BUD,
                   argc >= 5 ? atoi(argv[4]) : SF_DEF_B_BUD,
                   argc >= 6 ? atoi(argv[5]) : SF_DEF_R_BUD);
    else if (!strcmp(argv[1], "lcurve"))
        mode_lcurve(argc >= 3 ? atol(argv[2]) : 200);
    else if (!strcmp(argv[1], "oracle") && argc >= 3)
        mode_oracle(argv[2], argc >= 4 ? atoi(argv[3]) : SF_DEF_L_BUD,
                    argc >= 5 ? atoi(argv[4]) : SF_DEF_B_BUD,
                    argc >= 6 ? atoi(argv[5]) : SF_DEF_R_BUD);
    else if (!strcmp(argv[1], "sweep"))
        mode_sweep(argc >= 3 ? atol(argv[2]) : 200);
    else if (!strcmp(argv[1], "observed")) {
        /* 简单自检 sf_sun_observed / incidence / transit */
        sf_sun_observed_t obs;
        double jd = 2460350.5; // 2024-02-10 00:00 UT
        if (sf_sun_observed(jd, 39.9042, 116.4074, 50.0, 1013.25, 15.0, &obs) != 0) {
            fprintf(stderr, "sf_sun_observed 失败\n");
            return 1;
        }
        double inc = sf_solar_incidence_angle(obs.zenith_deg, obs.azimuth_deg, 30.0, 180.0);
        double transit = sf_sun_transit(jd, 116.4074);
        printf("OBS: az=%.4f deg, alt_app=%.4f deg, zen=%.4f deg, inc=%.4f deg, eot=%.2f min, transit=%.5f\n",
               obs.azimuth_deg, obs.altitude_apparent_deg, obs.zenith_deg, inc, obs.equation_of_time_min, transit);
    }
    else {
        fprintf(stderr, "未知模式\n");
        return 2;
    }
    return 0;
}
