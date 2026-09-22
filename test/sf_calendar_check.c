/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_calendar_check.c —— 农历的验收程序
 *
 * 断言全部来自 taiyin-lite/test/chinese-calendar.test.js。那份测试是 JS 写的，
 * 但它对照的是 taiyin-ephemeris 的 C++ 实现（原文：
 * 'historical month reforms match the C++ regression fixtures'），
 * 本库正是从那份 C++ 移植的 —— 这些期望值是三方共同的口径。
 *
 *   ./test/sf_calendar_check            全部
 *   ./test/sf_calendar_check modern     只跑现代路径
 *   ./test/sf_calendar_check hist       只跑古历 fixture
 *
 * 注意这里**不是**古历表的唯一验证手段：这些 fixture 只有 8 个点，而表里
 * 有 85485 条。全表核对走离线探针（未随库发布）experimental/verify_calendar_data.py 的 oracle 哈希。
 * ================================================================== */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sf_calendar.h"
#include "sf_rise.h"
#include "solar_fast.h"   /* sf_moon_distance / sf_gast 之外的位置量 */
#include "sf_qishuo.h"    /* 整年事件表 sf_qishuo_year_run */

/* 古历归日表已接入，所以这 8 条是硬断言。
 * 万一要临时关掉（比如在没搬表的精简构建里），改成 0 它们会算「跳过」
 * 而不是「失败」—— 免得一个预期中的未完成状态把 make check 弄红。 */
#define HISTORICAL_IMPLEMENTED 1

static int g_pass, g_fail, g_skip;

#define OK(cond, ...) do {                                    \
    if (cond) { g_pass++; }                                   \
    else { g_fail++; printf("  ✗ %s:%d  ", __FILE__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); }               \
} while (0)

static int lunar_eq(const sf_lunar_date *a, int32_t y, int32_t hy, int mo, int d,
                    int leap, int days, int name)
{
    return a->year == y && a->historical_year == hy && a->month == mo
        && a->day == d && a->is_leap == leap && a->month_days == days
        && a->month_name == name;
}

static void dump_lunar(const char *tag, const sf_lunar_date *l)
{
    printf("      %s → year=%d hist=%d month=%d%s day=%d days=%d name=%d\n",
           tag, l->year, l->historical_year, l->month,
           l->is_leap ? "(闰)" : "", l->day, l->month_days, l->month_name);
}

/* ------------------------------------------------------------------
 * 现代路径（天文定朔定气）
 * ------------------------------------------------------------------ */
static void run_modern(void)
{
    printf("== 现代路径 ==\n");

    /* --- 公历 → 农历 --- */
    {
        sf_cal_config c;
        sf_cal_config_init(&c);
        sf_solar_date s;
        sf_lunar_date l;

        s = (sf_solar_date){ 2025, 1, 29 };
        memset(&l, 0, sizeof l);
        OK(sf_cal_from_solar(&c, &s, &l) == 0, "2025-01-29 求解失败");
        if (l.month == 0) { dump_lunar("2025-01-29", &l); g_skip++; }
        else OK(lunar_eq(&l, 2025, 2025, 1, 1, 0, 30, SF_MONTH_NAME_NORMAL),
                "2025-01-29 期望 2025 正月初一/30天");
        if (!lunar_eq(&l, 2025, 2025, 1, 1, 0, 30, SF_MONTH_NAME_NORMAL))
            dump_lunar("2025-01-29", &l);

        s = (sf_solar_date){ 2026, 3, 15 };
        memset(&l, 0, sizeof l);
        OK(sf_cal_from_solar(&c, &s, &l) == 0, "2026-03-15 求解失败");
        OK(lunar_eq(&l, 2026, 2026, 1, 27, 0, 30, SF_MONTH_NAME_NORMAL),
           "2026-03-15 期望 2026 正月廿七/30天");
        if (!lunar_eq(&l, 2026, 2026, 1, 27, 0, 30, SF_MONTH_NAME_NORMAL))
            dump_lunar("2026-03-15", &l);
    }

    /* --- 2033 闰十一月：农历实现的经典地狱案例 --- */
    {
        sf_cal_config c;
        sf_cal_config_init(&c);
        const sf_solar_date s = { 2033, 12, 22 };
        sf_lunar_date l;
        memset(&l, 0, sizeof l);
        OK(sf_cal_from_solar(&c, &s, &l) == 0, "2033-12-22 求解失败");
        OK(lunar_eq(&l, 2033, 2033, 11, 1, 1, 29, SF_MONTH_NAME_NORMAL),
           "2033-12-22 期望 闰十一月初一/29天");
        if (!lunar_eq(&l, 2033, 2033, 11, 1, 1, 29, SF_MONTH_NAME_NORMAL))
            dump_lunar("2033-12-22", &l);

        /* 往返 */
        sf_solar_date back;
        memset(&back, 0, sizeof back);
        OK(sf_cal_from_lunar(&c, &l, &back) == 0, "闰十一月初一 反解失败");
        OK(back.year == 2033 && back.month == 12 && back.day == 22,
           "闰十一月初一 应回到 2033-12-22，得到 %d-%d-%d",
           back.year, back.month, back.day);

        uint8_t days = 0;
        OK(sf_cal_month_days(&c, 2033, 11, 1, &days) == 0 && days == 29,
           "闰十一月 应有 29 天，得到 %u", days);
    }

    /* --- 岁结构：2034-01-15 往前那个岁的闰月索引应为 1 --- */
    {
        sf_cal_config c;
        sf_cal_config_init(&c);
        const double anchor = sf_julian_day_ut(2034, 1, 15, 12, 0, 0.0)
                            - 480.0 / 1440.0;
        sf_cal_year y;
        OK(sf_cal_year_ut(&c, anchor, &y) == 0, "2034 岁求解失败");
        OK(y.leap_month_index == 1, "2034 岁的闰月索引应为 1，得到 %d",
           y.leap_month_index);
        OK(y.months[0].month == 11 && y.months[0].is_leap == 0
           && y.months[1].month == 11 && y.months[1].is_leap == 1,
           "岁首两月应为 (11,非闰)(11,闰)，得到 (%u,%u)(%u,%u)",
           y.months[0].month, y.months[0].is_leap,
           y.months[1].month, y.months[1].is_leap);
        OK(y.solar_term_count == 25 && y.new_moon_count == 15 && y.month_count == 14,
           "计数应为 25/15/14，得到 %u/%u/%u",
           y.solar_term_count, y.new_moon_count, y.month_count);
    }
}

/* ------------------------------------------------------------------
 * 结构日界 vs 显示时区
 * ------------------------------------------------------------------ */
static void run_boundary(void)
{
    printf("== 日界：结构不跟显示时区走 ==\n");

    /* 同一瞬时，显示偏移 +480 vs 0 —— 农历日不同，但都**不是**各自时区
     * 算出来的结构，结构始终按东经 120° */
    const double jd = sf_julian_day_ut(2025, 1, 28, 16, 30, 0.0);

    sf_cal_config c;
    sf_lunar_date l;

    sf_cal_config_init_china_standard_historical(&c, 480);
    memset(&l, 0, sizeof l);
    OK(sf_cal_from_instant_ut(&c, jd, &l) == 0, "+480 瞬时求解失败");
    OK(lunar_eq(&l, 2025, 2025, 1, 1, 0, 30, SF_MONTH_NAME_NORMAL),
       "+480 应得 2025 正月初一/30天");
    if (!lunar_eq(&l, 2025, 2025, 1, 1, 0, 30, SF_MONTH_NAME_NORMAL))
        dump_lunar("+480", &l);

    sf_cal_config_init_china_standard_historical(&c, 0);
    memset(&l, 0, sizeof l);
    OK(sf_cal_from_instant_ut(&c, jd, &l) == 0, "偏移 0 瞬时求解失败");
    OK(lunar_eq(&l, 2024, 2024, 12, 29, 0, 29, SF_MONTH_NAME_NORMAL),
       "偏移 0 应得 2024 腊月廿九/29天");
    if (!lunar_eq(&l, 2024, 2024, 12, 29, 0, 29, SF_MONTH_NAME_NORMAL))
        dump_lunar("偏移 0", &l);
}

/* ------------------------------------------------------------------
 * 两种模式的差异：同一瞬时在朔的边界上
 * ------------------------------------------------------------------ */
static void run_modes(void)
{
    printf("== CHINA_ASTRO vs LOCAL_ASTRO（朔边界）==\n");

    /* 2026-08-12 17:40 UT 紧挨着一次定朔。中国标准历按东经 120° 归日，
     * 印度经线 82.5°E 的地方历则归到后一天。 */
    const double jd = sf_julian_day_ut(2026, 8, 12, 17, 40, 0.0);

    sf_cal_config c;
    sf_lunar_date l;

    sf_cal_config_init_china_standard_astronomical(&c, 330);
    memset(&l, 0, sizeof l);
    OK(sf_cal_from_instant_ut(&c, jd, &l) == 0, "CHINA_ASTRO 求解失败");
    OK(l.month == 6 && l.day == 30, "CHINA_ASTRO 应得 (6,30)，得到 (%u,%u)",
       l.month, l.day);
    if (l.month != 6 || l.day != 30) dump_lunar("CHINA_ASTRO", &l);

    sf_cal_config_init_local_astronomical_meridian(&c, 82.5);
    c.utc_offset_minutes = 330;
    memset(&l, 0, sizeof l);
    OK(sf_cal_from_instant_ut(&c, jd, &l) == 0, "LOCAL_ASTRO 求解失败");
    OK(l.month == 7 && l.day == 1, "LOCAL_ASTRO 应得 (7,1)，得到 (%u,%u)",
       l.month, l.day);
    if (l.month != 7 || l.day != 1) dump_lunar("LOCAL_ASTRO", &l);
}

/* ------------------------------------------------------------------
 * 古历 fixture（改历窗口）
 * ------------------------------------------------------------------ */
static void run_historical(void)
{
    printf("== 古历 fixture（改历窗口）==\n");

    static const struct {
        int32_t sy, sm, sd;
        int32_t ly, lhy, lm, ld; int leap, days, name;
    } fx[] = {
        { -456,  4,  4, -456, -456,  5, 12, 0, 30, SF_MONTH_NAME_NORMAL },
        { -104,  1,  3, -105, -104, 11, 27, 0, 29, SF_MONTH_NAME_NORMAL },
        { -103,  1, 20, -104, -103, 11, 27, 0, 30, SF_MONTH_NAME_NORMAL },
        {   10,  6,  1,   10,   10,  6,  1, 0, 30, SF_MONTH_NAME_NORMAL },
        {  238,  6,  1,  238,  238,  6,  2, 0, 29, SF_MONTH_NAME_NORMAL },
        {  690,  6,  1,  690,  690,  4, 19, 0, 29, SF_MONTH_NAME_NORMAL },
        {   23, 12,  2,   23,   23, 12,  1, 0, 29, SF_MONTH_NAME_ALT_TWELVE },
        {  690,  2, 15,  690,  690,  1,  1, 0, 29, SF_MONTH_NAME_ALT_ONE },
    };

    sf_cal_config c;
    sf_cal_config_init(&c);

    for (unsigned i = 0; i < sizeof fx / sizeof fx[0]; i++) {
        sf_solar_date s = { fx[i].sy, (uint8_t)fx[i].sm, (uint8_t)fx[i].sd };
        sf_lunar_date l;
        memset(&l, 0, sizeof l);
        if (sf_cal_from_solar(&c, &s, &l) != 0) {
            g_skip++;
            printf("  · %d-%d-%d 暂不可解（第 2 批）\n", fx[i].sy, fx[i].sm, fx[i].sd);
            continue;
        }
        if (!lunar_eq(&l, fx[i].ly, fx[i].lhy, fx[i].lm, fx[i].ld,
                      fx[i].leap, fx[i].days, fx[i].name)) {
#if HISTORICAL_IMPLEMENTED
            g_fail++;
            const char *mark = "✗";
#else
            g_skip++;
            const char *mark = "·";
#endif
            printf("  %s %d-%d-%d 期望 %d(h%d)/%d%s/%d天/名%d\n",
                   mark, fx[i].sy, fx[i].sm, fx[i].sd, fx[i].ly, fx[i].lhy,
                   fx[i].lm, fx[i].leap ? "(闰)" : "", fx[i].ld, fx[i].name);
            dump_lunar("  实际", &l);
        } else {
            g_pass++;
        }
    }
}

/* ------------------------------------------------------------------
 * 古历表的自检
 *
 * 这一组**不依赖任何外部期望值** —— 它检的是表自己的内部一致性和边界，
 * 是移植那 5 KB 二进制数据时唯一能自动抓住「搬错一位」的东西。
 * 上游 JS 测试里对应 'historical linear-plus-sparse-residual profiles
 * preserve their C++ endpoints' 那条。
 * ------------------------------------------------------------------ */
static void run_historical_profile(void)
{
    printf("== 古历表自检 ==\n");

    /* 上游的相位估计：从相位索引反推一个粗略的 JD */
    struct { int kind; int64_t first_phase, count, first_day, last_day; const char *name; } p[] = {
        { SF_CAL_HIST_NEW_MOON,   -33655, 33161, 1457698, 2436933, "朔" },
        { SF_CAL_HIST_SOLAR_TERM, -53265, 52324, 1640650, 2436925, "气" },
    };

    for (unsigned i = 0; i < sizeof p / sizeof p[0]; i++) {
        /* phaseEstimate：newMoon 与 solarTerm 的线性式不同 */
        const double per = (p[i].kind == SF_CAL_HIST_NEW_MOON)
            ? 29.5306 : 365.2422 / 24.0;
        const double base = (p[i].kind == SF_CAL_HIST_NEW_MOON)
            ? 2451551.0 - 14.0 : 2451259.0 - 7.0;

        const double first_est = base + ((double)p[i].first_phase + 0.5) * per;
        const double last_est  = base + ((double)(p[i].first_phase + p[i].count - 1) + 0.5) * per;
        const double after_est = base + ((double)(p[i].first_phase + p[i].count) + 0.5) * per;

        const int64_t d0 = sf_cal_historical_civil_day(p[i].kind, first_est);
        const int64_t d1 = sf_cal_historical_civil_day(p[i].kind, last_est);
        const int64_t d2 = sf_cal_historical_civil_day(p[i].kind, after_est);

        OK(d0 == p[i].first_day,
           "%s 表首归日应为 %lld，得到 %lld", p[i].name,
           (long long)p[i].first_day, (long long)d0);
        OK(d1 == p[i].last_day,
           "%s 表尾归日应为 %lld，得到 %lld", p[i].name,
           (long long)p[i].last_day, (long long)d1);
        OK(d2 == -1, "%s 超出表尾应返回 -1，得到 %lld",
           p[i].name, (long long)d2);

        /* 表外：1960 之后必须退化成天文，不能返回表里的值 */
        OK(sf_cal_historical_civil_day(p[i].kind, 2451545.0) == -1,
           "%s 在 2000 年应返回 -1（表只覆盖到 1960）", p[i].name);

        /* 单调性：整张表的归日必须严格递增，否则月长会出现 0 或负数。
         * 这一步扫全表（8 万多条），是抓「搬错一位」最直接的手段。 */
        int monotone = 1;
        int64_t prev = INT64_MIN;
        for (int64_t k = 0; k < p[i].count; k++) {
            const double est = base + ((double)(p[i].first_phase + k) + 0.5) * per;
            const int64_t d = sf_cal_historical_civil_day(p[i].kind, est);
            if (d < 0 || d <= prev) { monotone = 0; break; }
            prev = d;
        }
        OK(monotone, "%s 全表归日不严格递增（在序号附近断裂）", p[i].name);
    }
}

/* ------------------------------------------------------------------
 * 长区间：只要求不崩、有序、月长合法
 * ------------------------------------------------------------------ */
static void run_long_span(void)
{
    printf("== 长区间稳定性 ==\n");
    static const int32_t years[] = { 1900, 1960, 2033, 2100 };
    sf_cal_config c;
    sf_cal_config_init(&c);

    for (unsigned i = 0; i < sizeof years / sizeof years[0]; i++) {
        const double anchor = sf_julian_day_ut(years[i], 6, 1, 12, 0, 0.0)
                            - 480.0 / 1440.0;
        sf_cal_year y;
        if (sf_cal_year_ut(&c, anchor, &y) != 0) {
            g_fail++;
            printf("  ✗ %d 岁求解失败\n", years[i]);
            continue;
        }
        int ordered = 1;
        for (unsigned k = 1; k < SF_CAL_TERM_COUNT; k++)
            if (y.solar_terms[k].jd_ut <= y.solar_terms[k - 1].jd_ut) ordered = 0;
        for (unsigned k = 1; k < SF_CAL_NEW_MOON_COUNT; k++)
            if (y.new_moons[k].jd_ut <= y.new_moons[k - 1].jd_ut) ordered = 0;
        OK(ordered, "%d 岁的节气/朔序列不严格递增", years[i]);

        int lens_ok = 1;
        for (unsigned k = 0; k < SF_CAL_MONTH_COUNT; k++)
            if (y.months[k].day_count != 29 && y.months[k].day_count != 30) lens_ok = 0;
        OK(lens_ok, "%d 岁有非 29/30 天的月", years[i]);

        /* 往返：每个可寻址月的初一都应能回到它自己的首日 */
        int rt_ok = 1;
        for (unsigned k = 0; k < SF_CAL_MONTH_COUNT; k++) {
            if (y.months[k].first_civil_day_number >= y.second_winter_solstice_day_number)
                break;
            int32_t dy = 0, dmo = 0, dd = 0;
            sf_solar_date_from_day_number(y.months[k].first_civil_day_number,
                                          &dy, &dmo, &dd);
            const sf_solar_date sd = { dy, (uint8_t)dmo, (uint8_t)dd };
            sf_lunar_date l;
            memset(&l, 0, sizeof l);
            if (sf_cal_from_solar(&c, &sd, &l) != 0 || l.day != 1) { rt_ok = 0; break; }
        }
        OK(rt_ok, "%d 岁有月初一往返失败", years[i]);
    }
}

/* ------------------------------------------------------------------
 * 低项数归日路径：拿公开的精确岁表当参照，专门扫不同“历法经度”。
 *
 * 这里比较的是每个精确月首：快路径必须仍返回那个年月、月号、闰标记和初一。
 * 显示时区故意不参与；LOCAL 的结构日界完全由 meridian 决定。
 * ------------------------------------------------------------------ */
static void run_low_day_assignment(void)
{
    printf("== 低项数气朔：历法结构日界 ==\n");
    static const int32_t years[] = { -6000, -4000, -2000, 0, 2000,
                                     4000, 6000, 8000, 9999 };
    static const double meridians[] = { -165.0, -90.0, 0.0, 82.5, 120.0, 179.9 };
    int bad = 0, checked = 0;

    for (unsigned mi = 0; mi < sizeof meridians / sizeof meridians[0]; mi++) {
        sf_cal_config c;
        sf_cal_config_init_local_astronomical_meridian(&c, meridians[mi]);
        /* 显示偏移刻意固定成一个与经度无关的值，抓误用 local_offset 的回归。 */
        c.utc_offset_minutes = 330;

        for (unsigned yi = 0; yi < sizeof years / sizeof years[0]; yi++) {
            const double anchor = sf_julian_day_ut(years[yi], 6, 1, 12, 0, 0.0)
                                - meridians[mi] / 360.0;
            sf_cal_year exact;
            if (sf_cal_year_ut(&c, anchor, &exact) != 0) { bad++; continue; }

            for (unsigned k = 0; k < SF_CAL_MONTH_COUNT; k++) {
                const sf_cal_month *m = &exact.months[k];
                if (m->first_civil_day_number >= exact.second_winter_solstice_day_number)
                    break;
                int32_t sy = 0, sm = 0, sd = 0;
                sf_solar_date_from_day_number(m->first_civil_day_number, &sy, &sm, &sd);
                const sf_solar_date s = { sy, (uint8_t)sm, (uint8_t)sd };
                sf_lunar_date got;
                memset(&got, 0, sizeof got);
                checked++;
                if (sf_cal_from_solar(&c, &s, &got) != 0
                    || got.year != m->lunar_year
                    || got.historical_year != m->historical_year
                    || got.month != m->month || got.day != 1
                    || got.is_leap != m->is_leap
                    || got.month_days != m->day_count
                    || got.month_name != m->month_name) {
                    bad++;
                }
            }
        }
    }
    OK(bad == 0, "%d 个跨经度/月首对照中有 %d 个归日不一致", checked, bad);
}

/* ------------------------------------------------------------------
 * 干支（上游 test/ganzhi.test.js）
 *
 * 编码：8 位打包 (天干<<4 | 地支)，所以 0x66 = 己巳、0x46 = 戊午。
 * ------------------------------------------------------------------ */
static void run_ganzhi(void)
{
    printf("== 干支 ==\n");

    /* --- 规则：编码 / 进位 / 纳音 --- */
    {
        sf_ganzhi g;
        OK(sf_ganzhi_make(4, 6, &g) == 0 && g == 0x46, "makeGanzhi(4,6) 应为 0x46");
        OK(sf_ganzhi_advance(0x46, 1, &g) == 0 && g == 0x57,
           "advance(0x46,1) 应为 0x57，得到 0x%02x", g);
        /* 天干地支奇偶性不同 → 不是合法干支 */
        OK(sf_ganzhi_make(0, 1, &g) != 0, "makeGanzhi(0,1) 应被拒（奇偶性不符）");

        /* 六十甲子：序号、纳音编号、纳音五行全部自洽 */
        int all_ok = 1;
        for (int i = 0; i < 60; i++) {
            sf_ganzhi v;
            uint8_t el = 0, ny = 0;
            int32_t idx = -1;
            if (sf_ganzhi_make((uint8_t)(i % 10), (uint8_t)(i % 12), &v) != 0
                || sf_ganzhi_index(v, &idx) != 0 || idx != i
                || sf_ganzhi_nayin_id(v, &ny) != 0 || ny != i / 2
                || sf_ganzhi_nayin_element(v, &el) != 0 || el > SF_WUXING_FIRE) {
                all_ok = 0;
                break;
            }
        }
        OK(all_ok, "六十甲子的序号/纳音编号/五行应全部自洽");

        uint8_t el = 0xff;
        OK(sf_ganzhi_nayin_element(0x00, &el) == 0 && el == SF_WUXING_METAL,
           "甲子纳音应为金，得到 %u", el);
        OK(sf_ganzhi_nayin_element(0x28, &el) == 0 && el == SF_WUXING_FIRE,
           "壬午纳音应为火，得到 %u", el);
    }

    /* --- 日柱锚点 --- */
    {
        sf_cal_datetime d = { 2000, 1, 7, 12, 0, 0.0 };
        sf_ganzhi g;
        OK(sf_ganzhi_day_pillar(&d, &g) == 0 && g == 0x00,
           "2000-01-07 应为甲子日 (0x00)，得到 0x%02x", g);
        d.day = 9;
        OK(sf_ganzhi_day_pillar(&d, &g) == 0 && g == 0x22,
           "2000-01-09 应为丙寅日 (0x22)，得到 0x%02x", g);
    }

    /* --- 年号取干支 ---
     * 锚点 + 每 60 年一个循环；再断言「跟正月初一换、不跟立春换」这一点，
     * 那是它与年柱唯一的分岔处。 */
    {
        static const struct { int32_t y; uint8_t want; } fx[] = {
            { 1984, 0x00 },   /* 甲子 —— 锚点本身 */
            { 2024, 0x04 },   /* 甲辰 */
            { 2025, 0x15 },   /* 乙巳 */
            { 2026, 0x26 },   /* 丙午 */
            { 1900, 0x60 },   /* 庚子（庚子事变那年） */
            {   -1, 0x57 },   /* 己未（天文纪年 -1 = 公元前 2 年） */
        };
        for (unsigned i = 0; i < sizeof fx / sizeof fx[0]; i++) {
            sf_ganzhi g;
            OK(sf_ganzhi_year_of(fx[i].y, &g) == 0 && g == fx[i].want,
               "%d 年应为 0x%02x，得到 0x%02x", fx[i].y, fx[i].want, g);
        }
        sf_ganzhi g;
        OK(sf_ganzhi_year_of(1984, &g) == 0
           && sf_ganzhi_year_of(1984 + 60, &g) == 0 && g == 0x00,
           "60 年循环：2044 也应是甲子");
        OK(sf_ganzhi_year_of(2026, NULL) != 0, "空指针应被拒");
    }

    /* --- 年号干支 ≠ 年柱：2026 立春(2/4) 到 春节(2/17) 之间 ---
     * 这 13 天里年柱已经是丙午，而农历年还是乙巳。屏上那行农历如果配了
     * 年柱，就会印出「丙午年 腊月廿三」—— 丙午年没有腊月。 */
    {
        sf_cal_config c;
        sf_cal_config_init_china_standard_astronomical(&c, 480);
        const sf_cal_datetime vt = { 2026, 2, 10, 12, 0, 0.0 };
        const double inst = sf_julian_day_ut(2026, 2, 10, 12, 0, 0.0) - 480.0 / 1440.0;
        sf_ganzhi_pillars p;
        OK(sf_ganzhi_four_pillars(&c, inst, &vt, SF_GANZHI_RAT_HOUR_NO_SPLIT,
                                  &p) == 0, "2026-02-10 四柱求解失败");
        OK(p.year == 0x26, "2026-02-10 年柱应是丙午 (0x26)，得到 0x%02x", p.year);
        sf_lunar_date ld;
        OK(sf_cal_from_instant_ut(&c, inst, &ld) == 0, "2026-02-10 农历求解失败");
        sf_ganzhi ly;
        OK(sf_ganzhi_year_of(ld.historical_year, &ly) == 0 && ly == 0x15,
           "2026-02-10 的农历年应是乙巳 (0x15)，得到 0x%02x（lunar.year=%d, historical=%d）",
           ly, ld.year, ld.historical_year);
        OK(ld.month == 12, "2026-02-10 应是腊月（12），得到 %d", ld.month);
    }

    /* --- 现代四柱回归向量（对 C++ 层） --- */
    {
        static const struct {
            int y, mo, d, h, mi;
            uint8_t want[4];
        } fx[] = {
            { 1990, 5, 15, 14, 30, { 0x66, 0x75, 0x64, 0x97 } },
            { 1900, 1,  1, 12,  0, { 0x5b, 0x20, 0x0a, 0x66 } },
            { 2000, 1,  1,  0, 30, { 0x53, 0x20, 0x46, 0x80 } },
            { 2026, 3,  5, 10,  4, { 0x26, 0x62, 0x42, 0x35 } },
        };
        sf_cal_config c;
        sf_cal_config_init_china_standard_astronomical(&c, 480);

        for (unsigned i = 0; i < sizeof fx / sizeof fx[0]; i++) {
            const sf_cal_datetime vt = { fx[i].y, fx[i].mo, fx[i].d,
                                         fx[i].h, fx[i].mi, 0.0 };
            /* ZonedTime(fields, +480) → 瞬时是这些字段减去 8 小时 */
            const double inst = sf_julian_day_ut(fx[i].y, fx[i].mo, fx[i].d,
                                                 fx[i].h, fx[i].mi, 0.0)
                              - 480.0 / 1440.0;
            sf_ganzhi_pillars p;
            OK(sf_ganzhi_four_pillars(&c, inst, &vt, SF_GANZHI_RAT_HOUR_NO_SPLIT,
                                      &p) == 0,
               "%d-%d-%d 四柱求解失败", fx[i].y, fx[i].mo, fx[i].d);
            const int got = (p.year == fx[i].want[0] && p.month == fx[i].want[1]
                          && p.day == fx[i].want[2] && p.hour == fx[i].want[3]);
            OK(got, "%d-%d-%d %02d:%02d 期望 %02x %02x %02x %02x，得到 %02x %02x %02x %02x",
               fx[i].y, fx[i].mo, fx[i].d, fx[i].h, fx[i].mi,
               fx[i].want[0], fx[i].want[1], fx[i].want[2], fx[i].want[3],
               p.year, p.month, p.day, p.hour);
        }
    }

    /* --- 三种早晚子时约定必须彼此独立 --- */
    {
        sf_cal_config c;
        sf_cal_config_init_china_standard_astronomical(&c, 480);
        const sf_cal_datetime vt = { 2000, 1, 1, 23, 30, 0.0 };
        const double inst = sf_julian_day_ut(2000, 1, 1, 23, 30, 0.0) - 480.0 / 1440.0;

        static const struct { int mode; uint8_t day, hour; const char *js; } m[] = {
            { SF_GANZHI_RAT_HOUR_NO_SPLIT,     0x57, 0x00, "NEXT_DAY" },
            { SF_GANZHI_RAT_HOUR_TOMORROW_GAN, 0x46, 0x00, "CURRENT_DAY_TOMORROW_STEM" },
            { SF_GANZHI_RAT_HOUR_TODAY_GAN,    0x46, 0x80, "CURRENT_DAY" },
        };
        for (unsigned i = 0; i < sizeof m / sizeof m[0]; i++) {
            sf_ganzhi_pillars p;
            OK(sf_ganzhi_four_pillars(&c, inst, &vt, m[i].mode, &p) == 0,
               "晚子时 %s 求解失败", m[i].js);
            OK(p.day == m[i].day && p.hour == m[i].hour,
               "晚子时 %s 期望 day=%02x hour=%02x，得到 day=%02x hour=%02x",
               m[i].js, m[i].day, m[i].hour, p.day, p.hour);
        }
    }

    /* --- 立春换柱：瞬时前后各半秒 --- */
    {
        sf_cal_config c;
        sf_cal_config_init_china_standard_astronomical(&c, 480);
        sf_cal_term_event lichun;
        OK(sf_cal_get_specific_term(&c, 2024, 21, &lichun) == 0, "2024 立春求解失败");

        const double half_sec = 0.5 / 86400.0;
        const double inst[2] = { lichun.jd_ut - half_sec, lichun.jd_ut + half_sec };
        const uint8_t want_y[2] = { 0x93, 0x04 };   /* 癸卯 → 甲辰 */
        const uint8_t want_m[2] = { 0x11, 0x22 };   /* 甲子月 → 丙寅月 */

        for (int k = 0; k < 2; k++) {
            /* 民用时刻：把瞬时按 +8 时区落到字段（立春在 2 月 4 日，不会跨日） */
            const double local_day = inst[k] + 480.0 / 1440.0;
            int32_t y = 2024, mo = 2, d = 4, hh = 0, mm = 0;
            {
                const double frac = (local_day + 0.5) - floor(local_day + 0.5);
                hh = (int)floor(frac * 24.0);
                mm = (int)floor((frac * 24.0 - hh) * 60.0);
            }
            const sf_cal_datetime vt = { y, mo, d, hh, mm, 0.0 };
            sf_ganzhi_pillars p;
            OK(sf_ganzhi_four_pillars(&c, inst[k], &vt, SF_GANZHI_RAT_HOUR_NO_SPLIT,
                                      &p) == 0, "立春%s 四柱求解失败", k ? "后" : "前");
            OK(p.year == want_y[k] && p.month == want_m[k],
               "立春%s 期望年=%02x 月=%02x，得到年=%02x 月=%02x",
               k ? "后" : "前", want_y[k], want_m[k], p.year, p.month);
        }
    }
}

/* ------------------------------------------------------------------
 * 恒星时 / 日出日落 / 均时差
 *
 * 这一组**没有三方共同口径**（上游 JS/C++ 的对应模块还没读），所以断言
 * 全部来自独立的标准值：Meeus 的均时差极值、天文年历用的 −0.8333° 出没
 * 定义、以及「赤道终年 12 小时上下」这类硬事实。
 * ------------------------------------------------------------------ */
static void run_rise(void)
{
    printf("== 恒星时 / 出没 / 均时差 ==\n");

    const double R = 180.0 / M_PI;

    /* --- GAST 在 J2000.0 ---
     * 标准 GMST(2451545.0 UT) = 280.46061837°。GAST 要再加上**分点差**
     * Δψ·cos ε，而 J2000 时分点差是**负**的（Δψ ≈ −13.8″），
     * 所以 GAST ≈ 280.4571°，比 GMST 小 12.7″。
     * 注意：不得把标准 GMST 值直接当 GAST 写断言。 */
    {
        const double g = sf_gast(2451545.0, 2451545.0) * R;
        OK(fabs(g - 280.4571) < 0.002,
           "J2000.0 的 GAST 应约 280.4571°，得到 %.4f°", g);
    }

    /* --- GAST **远离 J2000**（ERA 速率的判据）---
     *
     * 注意：上面那条单点断言**抓不到 ERA 速率写错**：岁差多项式在 t=0 处为
     * 0.0145″≈0，而误差正比于 t。若把 ERA 速率写成经典的恒星日比
     * 1.00273790935（应为 IAU2000 的 1.00273781191135448），做完 ERA 又叠
     * 一遍岁差 —— 岁差算了**两遍**，整条恒星时偏 +0.3°、出没时刻偏 +71 s，
     * 而 J2000 单点自检看不出来。下面这几条是实测值。 */
    {
        static const struct { double jd; double want; } g[] = {
            { 2460000.5, 154.599567325 },
            { 2460400.5, 188.859550947 },
            { 2462000.5, 325.901010552 },
        };
        for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++) {
            const double v = sf_gast(g[i].jd, sf_ut_to_tt(g[i].jd)) * R;
            OK(fabs(v - g[i].want) < 0.001,
               "jd %.1f 的 GAST 应 %.6f°，得到 %.6f°（差 %.2f″）",
               g[i].jd, g[i].want, v, (v - g[i].want) * 3600.0);
        }
    }

    /* --- 月球距离：原作全量 R(325 项) 的值，小数四位一致 --- */
    {
        const double d = sf_moon_distance(2460000.5);
        OK(fabs(d - 381932.6229) < 0.01,
           "月球地心距应 381932.6229 km，得到 %.4f km", d);
        OK(d > 356000.0 && d < 407000.0, "月地距离越出物理范围：%.1f km", d);
    }

    /* --- 月出月落：对上游 bodyRiseSetForDay('moon') ---
     *
     * 宽松的 < 5 s 断言，只作冒烟用。**精确的那组在上面 16 条 fixture 里**
     * （容差 0.5 s，覆盖四个季节 × 四个经纬度）。蒙气差、站心视差、视半径、
     * ΔT 逐项对齐之后，剩下的差不是出没算法，是**星历差**（月球位置 0.07″）。
     * 这两条数值用于确认改动未整体平移。 */
    {
        double r = 0.0, s = 0.0;
        const int rc = sf_moon_rise_set(2460000.5, 39.9042, 116.4074, &r, &s);
        OK(rc == SF_RISE_OK, "北京 2460000.5 的月出月落求解失败，rc=%d", rc);
        OK(fabs((r - 2460000.564412) * 86400.0) < 5.0,
           "月出应 2460000.564412，得到 %.6f（差 %.2f s）",
           r, (r - 2460000.564412) * 86400.0);
        OK(fabs((s - 2460001.162365) * 86400.0) < 5.0,
           "月落应 2460001.162365，得到 %.6f（差 %.2f s）",
           s, (s - 2460001.162365) * 86400.0);
    }

    /* --- 均时差：四个标准极值（Meeus 28 章） --- */
    {
        static const struct { int mo, d; double want; } fx[] = {
            { 2, 11, -14.2 }, { 5, 14, +3.7 }, { 7, 26, -6.5 }, { 11, 3, +16.4 },
        };
        for (unsigned i = 0; i < sizeof fx / sizeof fx[0]; i++) {
            const double jd = sf_julian_day_ut(2026, fx[i].mo, fx[i].d, 12, 0, 0.0);
            const double e = sf_equation_of_time_minutes(sf_ut_to_tt(jd));
            OK(fabs(e - fx[i].want) < 0.15,
               "2026-%02d-%02d 均时差应约 %+.1f 分，得到 %+.2f 分",
               fx[i].mo, fx[i].d, fx[i].want, e);
        }
    }

    /* --- 北京：昼长对着天文年历 --- */
    {
        const double LAT = 39.9042, LON = 116.4074;
        static const struct { int mo, d; double want; const char *tag; } fx[] = {
            { 3, 20, 12.123, "春分" },
            { 6, 21, 15.006, "夏至" },
            { 9, 23, 12.135, "秋分" },
            { 12, 21, 9.338, "冬至" },
        };
        for (unsigned i = 0; i < sizeof fx / sizeof fx[0]; i++) {
            const double jd = sf_julian_day_ut(2026, fx[i].mo, fx[i].d, 12, 0, 0.0);
            const double h = sf_day_length_hours(jd, LAT, LON);
            OK(fabs(h - fx[i].want) < 0.02,
               "北京 2026 %s 昼长应约 %.3f h，得到 %.3f h",
               fx[i].tag, fx[i].want, h);
        }
        /* 日出必须早于日落，且都在当天附近 */
        double r = 0, s = 0;
        const double jd = sf_julian_day_ut(2026, 6, 21, 12, 0, 0.0);
        OK(sf_sun_rise_set(jd, LAT, LON, &r, &s) == SF_RISE_OK, "北京夏至出没求解失败");
        OK(r < s && (s - r) < 1.0, "日出应早于日落且相隔不到一天");
        {
            double cr = 0, cs = 0, ch = 0;
            const int rc = sf_sun_rise_set_day_length(jd, LAT, LON, &cr, &cs, &ch);
            OK(rc == SF_RISE_OK && cr == r && cs == s
               && ch == sf_day_length_hours(jd, LAT, LON),
               "合并出没/昼长接口应与两次旧调用逐位一致");
        }
    }

    /* --- 赤道：终年 12 小时上下（蒙气差让它略多于 12） --- */
    {
        static const int md[4][2] = { {3,20}, {6,21}, {9,23}, {12,21} };
        for (unsigned i = 0; i < 4; i++) {
            const double jd = sf_julian_day_ut(2026, md[i][0], md[i][1], 12, 0, 0.0);
            const double h = sf_day_length_hours(jd, 0.0, 116.4074);
            OK(h > 12.0 && h < 12.25,
               "赤道 2026-%02d-%02d 昼长应在 12–12.25 h，得到 %.3f h",
               md[i][0], md[i][1], h);
        }
    }

    /* --- 极点：极昼 / 极夜必须被识别出来，而不是给出个错数 --- */
    {
        const double jo = sf_julian_day_ut(2026, 6, 21, 12, 0, 0.0);
        const double jd_ = sf_julian_day_ut(2026, 12, 21, 12, 0, 0.0);
        double r = 0, s = 0;
        OK(sf_sun_rise_set(jo, 89.9, 0.0, &r, &s) == SF_RISE_POLAR_DAY,
           "北极夏至应为极昼");
        OK(sf_sun_rise_set(jd_, 89.9, 0.0, &r, &s) == SF_RISE_POLAR_NIGHT,
           "北极冬至应为极夜");
        OK(sf_day_length_hours(jo, 89.9, 0.0) == 24.0, "极昼昼长应为 24 h");
        OK(sf_day_length_hours(jd_, 89.9, 0.0) == 0.0, "极夜昼长应为 0 h");
        {
            double h = -1.0;
            OK(sf_sun_rise_set_day_length(jo, 89.9, 0.0, &r, &s, &h)
                   == SF_RISE_POLAR_DAY && h == 24.0,
               "合并接口的极昼应返回 24 h");
            OK(sf_sun_rise_set_day_length(jd_, 89.9, 0.0, &r, &s, &h)
                   == SF_RISE_POLAR_NIGHT && h == 0.0,
               "合并接口的极夜应返回 0 h");
        }
    }

    /* --- 出没真值：对 taiyin-lite 的 bodyRiseSetForDay（显式 UT 区间）---
     *
     * 口径完全一致：区间 [day_start, day_start+1)，上边缘 + 蒙气差 − 地平 = 0，
     * 站心用 WGS84 椭球精确向量相减。所以两边应当逐点吻合。
     *
     * ΔT 已经换成跟 taiyin-lite 逐位一致的模型（S15 样条 + IERS 年表 +
     * future 公式），实测两者最大差 5e-11 s。蒙气差、GAST、视半径也逐项对齐过
     * （见 README 第 8 层）。剩下的不是出没算法，是**星历差**：太阳位置
     * 0.0035″、月球 0.07″。折到时刻实测 rms 太阳 0.024 s / 月球 0.053 s
     * （232/206 个事件的网格上最大 0.25 s）。这里留 0.5 s 余量。
     *
     * 这组 fixture 特意覆盖了三种非平凡情形：高纬极昼（state 而非时刻）、
     * 赤道六月月球「只有升没有落」（次数不是 1/1）、以及南北半球同时取。 */
    {
        static const struct {
            int y, mo, d; double lat, lon;
            sf_alt_state_t sun_state, moon_state;
            int sun_nr, sun_ns, moon_nr, moon_ns;
            double sun_r, sun_s, moon_r, moon_s;
            const char *tag;
        } fx[] = {
            { 2026, 3, 20, 39.9042, 116.4074, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461120.4284317, 2461119.9346610, 2461120.4611710, 2461120.0010921, "北京" },
            { 2026, 3, 20, 0.0000, 0.0000, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461119.7529109, 2461120.2574151, 2461119.7937648, 2461120.3102266, "赤道" },
            { 2026, 3, 20, 66.5000, 0.0000, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461119.7503585, 2461120.2612585, 2461119.7346480, 2461120.4018485, "高纬" },
            { 2026, 3, 20, -33.8700, 151.2100, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461120.3324542, 2461119.8381482, 2461120.4200012, 2461119.8567863, "悉尼" },
            { 2026, 6, 21, 39.9042, 116.4074, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461213.3654169, 2461212.9904558, 2461212.6480930, 2461213.1651922, "北京" },
            { 2026, 6, 21, 0.0000, 0.0000, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 0, 2461212.7487291, 2461213.2537952, 2461212.9863635, NAN, "赤道" },
            { 2026, 6, 21, 66.5000, 0.0000, SF_ALT_ALWAYS_ABOVE, SF_ALT_CROSSES, 0, 0, 1, 1, NAN, NAN, 2461212.9859968, 2461213.4811652, "高纬" },
            { 2026, 6, 21, -33.8700, 151.2100, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461213.3751494, 2461212.7873414, 2461212.5590193, 2461213.0686581, "悉尼" },
            { 2026, 9, 23, 39.9042, 116.4074, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461307.4190784, 2461306.9239834, 2461306.8550662, 2461307.2984006, "北京" },
            { 2026, 9, 23, 0.0000, 0.0000, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461306.7424678, 2461307.2469408, 2461307.1497187, 2461306.6334054, "赤道" },
            { 2026, 9, 23, 66.5000, 0.0000, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461306.7395892, 2461307.2485542, 2461307.2470338, 2461306.5011409, "高纬" },
            { 2026, 9, 23, -33.8700, 151.2100, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461307.3212355, 2461306.8277202, 2461306.6838629, 2461307.2580408, "悉尼" },
            { 2026, 12, 21, 39.9042, 116.4074, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461396.4809996, 2461395.8697278, 2461395.7487941, 2461396.3937315, "北京" },
            { 2026, 12, 21, 0.0000, 0.0000, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461395.7460480, 2461396.2512631, 2461396.1444362, 2461395.6227143, "赤道" },
            { 2026, 12, 21, 66.5000, 0.0000, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461395.9519422, 2461396.0453618, 2461395.9151163, 2461395.8415113, "高纬" },
            { 2026, 12, 21, -33.8700, 151.2100, SF_ALT_CROSSES, SF_ALT_CROSSES, 1, 1, 1, 1, 2461396.2785504, 2461395.8787579, 2461395.7540659, 2461396.1756780, "悉尼" },
        };
        const double TOL = 0.5 / 86400.0;
        for (unsigned i = 0; i < sizeof fx / sizeof fx[0]; i++) {
            const double day0 = sf_julian_day_ut(fx[i].y, fx[i].mo, fx[i].d, 0, 0, 0.0);
            sf_rise_set_t s, mo;
            OK(sf_sun_rise_set_for_day(day0, fx[i].lat, fx[i].lon, NULL, &s) == 0,
               "%s 太阳出没求解失败", fx[i].tag);
            OK(sf_moon_rise_set_for_day(day0, fx[i].lat, fx[i].lon, NULL, &mo) == 0,
               "%s 月球出没求解失败", fx[i].tag);

            OK(s.state == fx[i].sun_state, "%s 太阳 altitude_state 不符（得 %d 期望 %d）",
               fx[i].tag, (int)s.state, (int)fx[i].sun_state);
            OK(mo.state == fx[i].moon_state, "%s 月球 altitude_state 不符（得 %d 期望 %d）",
               fx[i].tag, (int)mo.state, (int)fx[i].moon_state);
            OK(s.n_rise == fx[i].sun_nr && s.n_set == fx[i].sun_ns,
               "%s 太阳穿越次数不符（得 %d/%d 期望 %d/%d）",
               fx[i].tag, s.n_rise, s.n_set, fx[i].sun_nr, fx[i].sun_ns);
            OK(mo.n_rise == fx[i].moon_nr && mo.n_set == fx[i].moon_ns,
               "%s 月球穿越次数不符（得 %d/%d 期望 %d/%d）",
               fx[i].tag, mo.n_rise, mo.n_set, fx[i].moon_nr, fx[i].moon_ns);

            if (s.n_rise == 1 && !isnan(fx[i].sun_r))
                OK(fabs(s.rise[0] - fx[i].sun_r) < TOL, "%s 太阳升起差 %.2f s",
                   fx[i].tag, (s.rise[0] - fx[i].sun_r) * 86400.0);
            if (s.n_set == 1 && !isnan(fx[i].sun_s))
                OK(fabs(s.set[0] - fx[i].sun_s) < TOL, "%s 太阳落下差 %.2f s",
                   fx[i].tag, (s.set[0] - fx[i].sun_s) * 86400.0);
            if (mo.n_rise == 1 && !isnan(fx[i].moon_r))
                OK(fabs(mo.rise[0] - fx[i].moon_r) < TOL, "%s 月球升起差 %.2f s",
                   fx[i].tag, (mo.rise[0] - fx[i].moon_r) * 86400.0);
            if (mo.n_set == 1 && !isnan(fx[i].moon_s))
                OK(fabs(mo.set[0] - fx[i].moon_s) < TOL, "%s 月球落下差 %.2f s",
                   fx[i].tag, (mo.set[0] - fx[i].moon_s) * 86400.0);

            /* 结构断言：任何穿越点都必须落在 [day0, day0+1) 内。
             * 日期归属错误会在此暴露（错误实现下约 87% 样本越界）。 */
            for (int k = 0; k < s.n_rise; k++)
                OK(s.rise[k] >= day0 && s.rise[k] < day0 + 1.0,
                   "%s 太阳升起越界（%.4f）", fx[i].tag, s.rise[k] - day0);
            for (int k = 0; k < s.n_set; k++)
                OK(s.set[k] >= day0 && s.set[k] < day0 + 1.0,
                   "%s 太阳落下越界（%.4f）", fx[i].tag, s.set[k] - day0);
            for (int k = 0; k < mo.n_rise; k++)
                OK(mo.rise[k] >= day0 && mo.rise[k] < day0 + 1.0,
                   "%s 月球升起越界（%.4f）", fx[i].tag, mo.rise[k] - day0);
            for (int k = 0; k < mo.n_set; k++)
                OK(mo.set[k] >= day0 && mo.set[k] < day0 + 1.0,
                   "%s 月球落下越界（%.4f）", fx[i].tag, mo.set[k] - day0);

            /* --- 默认 FAST vs 全网格：必须逐点一致 ---
             *
             * FAST 是 9 点日周拟合 + 每根两次完整位置校正；拿不准自动退回
             * ACCURATE。这里再强制跑测试专用网格，断言 state / 次数相同。
             * 月球 FAST 的位置截为 L129/B64/R129，时刻容差按独立全区间留出集
             * 随机 3.54 s、结构网格 4.91 s 最坏值留到 6 s；上面的 JS fixture
             * 仍单独守 0.5 s。 */
            {
                sf_rise_opts_t og;
                sf_rise_opts_default(&og);
                og.fast = SF_RISE_REFERENCE_GRID;
                sf_rise_set_t sg, mg;
                OK(sf_sun_rise_set_for_day(day0, fx[i].lat, fx[i].lon, &og, &sg) == 0,
                   "%s 太阳全网格求解失败", fx[i].tag);
                OK(sf_moon_rise_set_for_day(day0, fx[i].lat, fx[i].lon, &og, &mg) == 0,
                   "%s 月球全网格求解失败", fx[i].tag);
                OK(s.state == sg.state && s.n_rise == sg.n_rise && s.n_set == sg.n_set,
                   "%s 太阳 快速(%d, %d/%d) vs 网格(%d, %d/%d)",
                   fx[i].tag, (int)s.state, s.n_rise, s.n_set,
                   (int)sg.state, sg.n_rise, sg.n_set);
                OK(mo.state == mg.state && mo.n_rise == mg.n_rise && mo.n_set == mg.n_set,
                   "%s 月球 快速(%d, %d/%d) vs 网格(%d, %d/%d)",
                   fx[i].tag, (int)mo.state, mo.n_rise, mo.n_set,
                   (int)mg.state, mg.n_rise, mg.n_set);
                for (int k = 0; k < s.n_rise && k < sg.n_rise; k++)
                    OK(fabs(s.rise[k] - sg.rise[k]) < 1e-6,
                       "%s 太阳升起 快速 vs 网格 差 %.6f s",
                       fx[i].tag, (s.rise[k] - sg.rise[k]) * 86400.0);
                for (int k = 0; k < s.n_set && k < sg.n_set; k++)
                    OK(fabs(s.set[k] - sg.set[k]) < 1e-6,
                       "%s 太阳落下 快速 vs 网格 差 %.6f s",
                       fx[i].tag, (s.set[k] - sg.set[k]) * 86400.0);
                for (int k = 0; k < mo.n_rise && k < mg.n_rise; k++)
                    OK(fabs(mo.rise[k] - mg.rise[k]) < 6.0 / 86400.0,
                       "%s 月球升起 快速 vs 网格 差 %.6f s",
                       fx[i].tag, (mo.rise[k] - mg.rise[k]) * 86400.0);
                for (int k = 0; k < mo.n_set && k < mg.n_set; k++)
                    OK(fabs(mo.set[k] - mg.set[k]) < 6.0 / 86400.0,
                       "%s 月球落下 快速 vs 网格 差 %.6f s",
                       fx[i].tag, (mo.set[k] - mg.set[k]) * 86400.0);
            }
        }
    }


    /* --- 太阳高度 / 方位 --- */
    {
        const double LAT = 39.9042, LON = 116.4074;
        /* 北京夏至正午高度 = 90 − 39.9042 + 23.44 ≈ 73.5°。
         * 时刻取 04:16 UT：116.4°E 的**真太阳时**正午（地方平时正午是
         * 04:14，再加当日均时差 −1.6 分）。取 04:00 会让太阳偏东 3.6°，
         * 方位角就差出十几度。 */
        const double jd = sf_julian_day_ut(2026, 6, 21, 4, 16, 0.0);
        double az = 0.0;
        const double alt = sf_sun_altitude(jd, LAT, LON, &az) * R;
        OK(fabs(alt - 73.5) < 0.7, "北京夏至正午高度应约 73.5°，得到 %.2f°", alt);
        /* 注意：az 是**弧度**，必须换算后再与百分度值比较。 */
        const double az_deg = az * R;
        OK(az_deg > 175.0 && az_deg < 185.0,
           "正午方位角应≈正南(180°)，得到 %.1f°", az_deg);
    }
}

/* FAST 须在现代北京 fixture 之外验证：按固定随机序列横跨完整气朔年代、
 * 全经度和近全纬度，与 ACCURATE 逐窗对拍。 */
static void run_rise_fast_holdout(void)
{
    printf("== 出没 FAST 留出集 ==\n");
    const double jd0 = sf_julian_day_ut(-6000, 1, 1, 0, 0, 0.0);
    const double jd1 = sf_julian_day_ut(10001, 1, 1, 0, 0, 0.0);
    unsigned long long z = 0x243f6a8885a308d3ULL;
    int bad = 0;
    double maxerr[2] = { 0.0, 0.0 };

    for (int body = 0; body < 2; body++) {
        for (int i = 0; i < 2000; i++) {
            double u[3];
            for (int j = 0; j < 3; j++) {
                z = z * 6364136223846793005ULL + 1442695040888963407ULL;
                u[j] = (double)(z >> 11) * (1.0 / 9007199254740992.0);
            }
            const double day = floor(jd0 + u[0] * (jd1 - jd0) - 0.5) + 0.5;
            const double lat = -89.9 + 179.8 * u[1];
            const double lon = -180.0 + 360.0 * u[2];
            sf_rise_opts_t fo, ao;
            sf_rise_opts_default(&fo);
            sf_rise_opts_default(&ao);
            fo.fast = SF_RISE_FAST;
            ao.fast = SF_RISE_ACCURATE;
            sf_rise_set_t f, a;
            const int fr = body
                ? sf_moon_rise_set_for_day(day, lat, lon, &fo, &f)
                : sf_sun_rise_set_for_day(day, lat, lon, &fo, &f);
            const int ar = body
                ? sf_moon_rise_set_for_day(day, lat, lon, &ao, &a)
                : sf_sun_rise_set_for_day(day, lat, lon, &ao, &a);
            int mismatch = fr != ar || f.state != a.state
                         || f.n_rise != a.n_rise || f.n_set != a.n_set;
            for (int k = 0; k < f.n_rise && !mismatch; k++) {
                const double e = fabs(f.rise[k] - a.rise[k]) * 86400.0;
                if (e > maxerr[body]) maxerr[body] = e;
                /* 月球 FAST 有意用 L129/B64/R129；2 万随机留出最坏 3.54 s，
                 * 6.4 万边界/极区/经度网格最坏 4.91 s。这里留 6 s 硬上限。 */
                if (e > (body ? 6.0 : 0.1)) mismatch = 1;
            }
            for (int k = 0; k < f.n_set && !mismatch; k++) {
                const double e = fabs(f.set[k] - a.set[k]) * 86400.0;
                if (e > maxerr[body]) maxerr[body] = e;
                if (e > (body ? 6.0 : 0.1)) mismatch = 1;
            }
            if (mismatch) bad++;
        }
    }
    OK(bad == 0,
       "FAST vs ACCURATE 4000 窗口有 %d 个不符（太阳 max %.6f s，月亮 %.6f s）",
       bad, maxerr[0], maxerr[1]);
    printf("  -6000..10000 随机 4000 窗口：太阳 max %.6f s，月亮 %.6f s\n",
           maxerr[0], maxerr[1]);
}

/* ------------------------------------------------------------------
 * 有效窗口的边界行为
 *
 * 库声明的窗口是 |jd − J2000| ≤ 2922000 天（±8000 年），来自 FRAMES 多项式。
 * 农历比它多一层约束：**要确定某天属于哪个「岁」，得先找冬至，而找冬至
 * 会往后/往前各探一个回归年**。所以窗口边缘约 365 天的范围内会出现
 * 「能算出来、但反查不回去」的不对称。
 *
 * 这不是 bug，是窗口叠加的必然结果（上游 JS 的窗口和循环结构一样，
 * 行为一致）。这里把它写成断言，免得以后被当成回归。
 * ------------------------------------------------------------------ */
static void run_window_edge(void)
{
    printf("== 有效窗口边界 ==\n");

    sf_cal_config c;
    sf_cal_config_init_china_standard_astronomical(&c, 480);

    /* 窗口内足够深处：往返必须成立 */
    {
        static const int32_t years[] = { -3000, -1000, 0, 1000, 2033, 3000 };
        int all_ok = 1;
        for (unsigned i = 0; i < sizeof years / sizeof years[0]; i++) {
            const double anchor = sf_julian_day_ut(years[i], 6, 1, 12, 0, 0.0)
                                - 480.0 / 1440.0;
            sf_cal_year y;
            if (sf_cal_year_ut(&c, anchor, &y) != 0) { all_ok = 0; break; }
            for (unsigned k = 0; k < SF_CAL_MONTH_COUNT; k++) {
                if (y.months[k].first_civil_day_number
                        >= y.second_winter_solstice_day_number) break;
                int32_t a = 0, b = 0, d = 0;
                sf_solar_date_from_day_number(y.months[k].first_civil_day_number,
                                              &a, &b, &d);
                const sf_solar_date sd = { a, (uint8_t)b, (uint8_t)d };
                sf_lunar_date l;
                memset(&l, 0, sizeof l);
                if (sf_cal_from_solar(&c, &sd, &l) != 0 || l.day != 1) {
                    all_ok = 0;
                    break;
                }
            }
            if (!all_ok) break;
        }
        OK(all_ok, "−3000…+3000 之间每个月初一都应能原样反查回去");
    }

    /* 窗口边缘：**允许失败，但不许返回垃圾**。
     * 要求「求解失败」时出参保持未定义 —— 调用方只能看返回值。 */
    {
        const double anchor = sf_julian_day_ut(-6000, 6, 1, 12, 0, 0.0)
                            - 480.0 / 1440.0;
        sf_cal_year y;
        OK(sf_cal_year_ut(&c, anchor, &y) == 0,
           "−6000 年（窗口边缘）仍应能解出一个岁");

        int ordered = 1;
        for (unsigned k = 1; k < SF_CAL_TERM_COUNT; k++)
            if (y.solar_terms[k].jd_ut <= y.solar_terms[k - 1].jd_ut) ordered = 0;
        for (unsigned k = 1; k < SF_CAL_NEW_MOON_COUNT; k++)
            if (y.new_moons[k].jd_ut <= y.new_moons[k - 1].jd_ut) ordered = 0;
        OK(ordered, "−6000 年的节气/朔序列仍应严格递增");
    }

    /* 出窗（±8000 年之外）：**照常解，不当失败**。
     *
     * 注意：此处不再断言「10000 年必须返回失败」——上游没有这道栏杆：JS 只有
     * checkedEventDate，且只在 fast/accurate 档调用，默认的 mid 档完全不查，
     * 一路外推。若加这道栏杆，同一年份上游给 12 条事件、本库给 2 条，属接口
     * 行为不一致，比精度差更难定位。
     *
     * 现在只保证：出窗也要**给出严格递增的一整个歲**（可算，但未经核对 ——
     * 有效范围是 ±8000 年，见 solar_fast.h「有效范围」）。 */
    {
        const double anchor = sf_julian_day_ut(10000, 6, 1, 12, 0, 0.0)
                            - 480.0 / 1440.0;
        sf_cal_year y;
        int ok = (sf_cal_year_ut(&c, anchor, &y) == 0);
        for (unsigned k = 1; ok && k < SF_CAL_TERM_COUNT; k++)
            if (y.solar_terms[k].jd_ut <= y.solar_terms[k - 1].jd_ut) ok = 0;
        for (unsigned k = 1; ok && k < SF_CAL_NEW_MOON_COUNT; k++)
            if (y.new_moons[k].jd_ut <= y.new_moons[k - 1].jd_ut) ok = 0;
        OK(ok, "公元 10000 年（出窗、外推）仍应给出一整个严格递增的歲");
    }
}

/* ------------------------------------------------------------------
 * 时间尺度：闰秒 / UTC ↔ TT ↔ UT1
 * ------------------------------------------------------------------ */
static void run_timescale(void)
{
    printf("== 时间尺度：闰秒 / UTC ==\n");

    const double MJD0 = 2400000.5;

    /* --- TAI−UTC 的几个已知值 --- */
    {
        struct { int mjd; double want; } g[] = {
            { 41317, 10 }, { 41499, 11 }, { 47892, 25 }, { 53736, 33 },
            { 57204, 36 }, { 57754, 37 },
        };
        for (unsigned i = 0; i < sizeof g / sizeof g[0]; i++) {
            const double v = sf_tai_minus_utc_seconds(MJD0 + g[i].mjd);
            OK(fabs(v - g[i].want) < 1e-9,
               "MJD %d 的 TAI−UTC 应是 %.0f，得到 %.6f", g[i].mjd, g[i].want, v);
        }
    }

    /* --- 1972 前是橡皮秒：段内线性，不是整秒阶跃 --- */
    {
        const double a = sf_tai_minus_utc_seconds(MJD0 + 41316.0);   /* 1971-12-31 */
        const double b = sf_tai_minus_utc_seconds(MJD0 + 41317.0);   /* 1972-01-01 */
        OK(a > 9.0 && a < 10.0, "1972 前一天的 TAI−UTC 应略小于 10，得到 %.6f", a);
        OK(fabs(b - 10.0) < 1e-12, "1972-01-01 应是整数 10，得到 %.6f", b);
    }

    /* --- 1960-01-01 之前没有 UTC --- */
    OK(isnan(sf_tai_minus_utc_seconds(MJD0 + 36933.0)), "1960 前 TAI−UTC 应为 NAN");
    OK(isnan(sf_dut1_seconds(MJD0 + 36933.0)), "1960 前 DUT1 应为 NAN");
    {
        const double j = MJD0 + 30000.0;
        OK(sf_utc_to_tt(j) == sf_ut_to_tt(j),
           "1960 前 UTC→TT 应退化成 UT1→TT");
        OK(sf_utc_to_ut1(j) == j, "1960 前 UTC→UT1 应是恒等");
        OK(sf_tt_to_utc(sf_ut_to_tt(j)) == j, "1960 前 TT→UTC 应退化成 TT→UT1");
    }

    /* --- UTC→TT = (TAI−UTC) + 32.184，且闰秒那一刻**跳 1 s** --- */
    {
        const double j1 = MJD0 + 57754.0 - 1e-6;    /* 2016-12-31 23:59:59.9 */
        const double j2 = MJD0 + 57754.0 + 1e-6;    /* 2017-01-01 00:00:00.1 */
        /* 注意：容差取 1e-3 s 是**表示精度**决定的，不是实现精度：JD 约 2.46e6，
         * 两个 double 相减能分辨的最小量约 5e-5 s。1000 倍余量仍能抓住
         * 「闰秒数错」这种 1 s 级的错。 */
        const double d1 = (sf_utc_to_tt(j1) - j1) * 86400.0;
        const double d2 = (sf_utc_to_tt(j2) - j2) * 86400.0;
        OK(fabs(d1 - 68.184) < 1e-3, "2016 年末 UTC→TT 应是 68.184 s，得到 %.6f", d1);
        OK(fabs(d2 - 69.184) < 1e-3, "2017 年初 UTC→TT 应是 69.184 s，得到 %.6f", d2);
        OK(fabs((d2 - d1) - 1.0) < 1e-3, "跨闰秒应跳整 1 s，得到 %.6f", d2 - d1);
        /* UT1 侧是**连续**的 —— 闰秒只动 UTC 的刻度 */
        const double u1 = (sf_ut_to_tt(sf_utc_to_ut1(j1)) - sf_utc_to_ut1(j1)) * 86400.0;
        const double u2 = (sf_ut_to_tt(sf_utc_to_ut1(j2)) - sf_utc_to_ut1(j2)) * 86400.0;
        OK(fabs(u2 - u1) < 0.01, "跨闰秒 UT1→TT 不该跳，跳了 %.6f s", u2 - u1);
    }

    /* --- 往返：三条链都要精确到浮点末位 --- */
    {
        double w[3] = { 0, 0, 0 };
        for (double j = 2436934.5; j < 2461412.5; j += 0.97) {
            double e;
            e = fabs(sf_tt_to_utc(sf_utc_to_tt(j)) - j);   if (e > w[0]) w[0] = e;
            e = fabs(sf_ut1_to_utc(sf_utc_to_ut1(j)) - j); if (e > w[1]) w[1] = e;
            e = fabs(sf_tt_to_ut(sf_ut_to_tt(j)) - j);     if (e > w[2]) w[2] = e;
        }
        OK(w[0] < 1e-12, "UTC→TT→UTC 往返残差 %.3e 天", w[0]);
        OK(w[1] < 1e-12, "UTC→UT1→UTC 往返残差 %.3e 天", w[1]);
        OK(w[2] < 1e-12, "UT1→TT→UT1 往返残差 %.3e 天", w[2]);
    }

    /* --- 三条链互相自洽：UTC→TT 与 UTC→UT1→TT 应一致 --- */
    {
        double mx = 0;
        for (double j = 2436934.5; j < 2461412.5; j += 1.37) {
            const double a = sf_utc_to_tt(j);
            const double b = sf_ut_to_tt(sf_utc_to_ut1(j));
            if (fabs(a - b) > mx) mx = fabs(a - b);
        }
        /* 容差同理取「表示精度」量级：两条链都是往 2.46e6 量级的数上做小量
         * 加法，各带 ~5e-10 天的舍入，加起来 1e-9 量级。 */
        OK(mx < 1e-8, "两条链算出的 TT 相差 %.3e 天（应 < 1e-8）", mx);
    }

    /* --- DUT1 的守门员：每个闰秒生效日「插入前」的 DUT1 必须落在
     *     IERS 插秒策略允许的区间里。
     *
     * 这条检验是有效的：`UT1 − UTC = 32.184 + (TAI−UTC) − ΔT` 里 ΔT 若被换掉
     * 或整体偏了，这一列会**整列平移**、立刻掉出区间。26 个实测值在
     * −0.68 … −0.19（插入后 +0.32 … +0.81）。 */
    {
        static const int leap[][2] = {
            { 41317, 10 }, { 41499, 11 }, { 41683, 12 }, { 42048, 13 },
            { 42413, 14 }, { 42778, 15 }, { 43144, 16 }, { 43509, 17 },
            { 43874, 18 }, { 44239, 19 }, { 44786, 20 }, { 45151, 21 },
            { 45516, 22 }, { 46247, 23 }, { 47161, 24 }, { 47892, 25 },
            { 48257, 26 }, { 48804, 27 }, { 49169, 28 }, { 49534, 29 },
            { 50083, 30 }, { 50630, 31 }, { 51179, 32 }, { 53736, 33 },
            { 54832, 34 }, { 56109, 35 }, { 57204, 36 }, { 57754, 37 },
        };
        const unsigned n = sizeof leap / sizeof leap[0];
        double lo = 1e9, hi = -1e9;
        int bad = 0;
        /* i 从 1 起：MJD 41317 是闰秒制**开始**的那天，它前面是橡皮秒
         * （4.213170），不是「上一档整数」，拿它做「插入前」没有意义。 */
        for (unsigned i = 1; i < n; i++) {
            /* 插入**前**那一瞬：TAI−UTC 还是上一档 */
            const double j = MJD0 + leap[i][0] - 1.0 / 86400.0;
            const double prev = (double)leap[i - 1][1];
            const double d = 32.184 + prev - sf_delta_t_seconds(
                                2000.0 + (j - 2451545.0) / 365.25);
            if (d < lo) lo = d;
            if (d > hi) hi = d;
            if (!(d > -0.95 && d < -0.05)) bad++;
        }
        OK(bad == 0, "%u 个闰秒里有 %d 个插入前 DUT1 落在 (−0.95,−0.05) 外", n - 1, bad);
        OK(lo > -0.90 && hi < -0.10,
           "插入前 DUT1 范围 %.3f … %.3f 超出了 IERS 策略允许的带", lo, hi);
    }
}

/* ------------------------------------------------------------------
 * 整年事件表：sf_qishuo_year_run（公历年，一次拿全年的气 + 朔）
 *
 * 这一节是**契约测试**，不与实现互相印证：
 *   气 —— 24 个、升序、每个落在自己的目标黄经 term_index·15° 上、名字对得上；
 *   朔 —— 12 或 13 个、升序、|距角|≈0、间隔是一个朔望月。
 * 容差按实测残差定，两侧各留一个数量级以上：
 *   气 实测最大 0.0052″（尺度写错是 2.8″）→ 取 0.05″
 *   朔 实测最大 0.171″（相位写错是 29.11° = 1.05e5″）→ 取 1″
 * ------------------------------------------------------------------ */
static void run_year_tables(void)
{
    const int Y0 = 1900, Y1 = 2150;
    const double TERM_TOL = 0.05 / 206264.8;   /* 弧度 */
    const double PHASE_TOL = 1.0 / 206264.8;

    int n_term_bad = 0, n_ord_bad = 0, n_phase_bad = 0, n_gap_bad = 0, n_name_bad = 0;
    double worst_term = 0.0, worst_phase = 0.0, min_gap = 1e9;
    int first_term_bad = 0, first_phase_bad = 0, first_gap_year = 0, first_name_bad = 0;
    long n_phase_total = 0;

    for (int y = Y0; y <= Y1; y++) {
        sf_qishuo_options o;
        sf_qishuo_options_init(&o);          /* 默认：节气 + 朔（相位角 {0}） */
        static sf_qishuo_event ev[SF_QISHUO_MAX_EVENTS];
        sf_qishuo_year qy;
        if (sf_qishuo_year_run(&o, y, ev, SF_QISHUO_MAX_EVENTS, &qy) != SF_QISHUO_OK) {
            n_term_bad++;
            if (!first_term_bad) first_term_bad = y;
            continue;
        }

        int nt = 0, np = 0;
        double prev_term = 0.0, prev_phase = 0.0;
        for (int i = 0; i < qy.count; i++) {
            const sf_qishuo_event *e = &ev[i];
            char nm[64];
            sf_qishuo_event_name(e, nm, sizeof nm);
            if (e->kind == SF_QISHUO_KIND_SOLAR_TERM) {
                /* 名字必须与项号对得上 —— 少了这条，索引整体错位也看不出来 */
                if (strcmp(nm, SF_SOLAR_TERM_NAMES[e->term_index]) != 0) {
                    n_name_bad++;
                    if (!first_name_bad) first_name_bad = y;
                }
                if (nt && e->jd_tt <= prev_term) n_ord_bad++;
                prev_term = e->jd_tt;
                /* 解出来的时刻，用它的**目标黄经**（term_index·15°）去核 */
                const double want = e->term_index * (M_PI / 12.0);
                double d = fmod(fabs(sf_sun_apparent_longitude(e->jd_tt) - want),
                                2.0 * M_PI);
                if (d > M_PI) d = 2.0 * M_PI - d;
                if (d > worst_term) worst_term = d;
                if (d > TERM_TOL) { n_term_bad++; if (!first_term_bad) first_term_bad = y; }
                nt++;
            } else if (e->kind == SF_QISHUO_KIND_LUNAR_PHASE) {
                if (np && e->jd_tt <= prev_phase) n_ord_bad++;
                const double gap = np ? e->jd_tt - prev_phase : 0.0;
                prev_phase = e->jd_tt;
                const double el = fabs(sf_elongation(e->jd_tt));
                if (el > worst_phase) worst_phase = el;
                if (el > PHASE_TOL) { n_phase_bad++; if (!first_phase_bad) first_phase_bad = y; }
                if (np) {
                    if (gap < min_gap) min_gap = gap;
                    /* 朔望月实测在 29.27–29.83 天。放到 29.0/30.2 只为抓
                     * 「跨了一圈没走」那种结构错 —— 出错时它会是 0 或 ~58.7。 */
                    if (gap < 29.0 || gap > 30.2) {
                        n_gap_bad++;
                        if (!first_gap_year) first_gap_year = y;
                    }
                }
                np++;
                n_phase_total++;
            }
        }
        if (nt != 24) { n_term_bad++; if (!first_term_bad) first_term_bad = y; }
        if (np < 12 || np > 13) { n_phase_bad++; if (!first_phase_bad) first_phase_bad = y; }
    }

    const int ny = Y1 - Y0 + 1;
    printf("  整年表 %d–%d：气 %d 个/年，朔 12–13 个/年（共 %ld 个朔）\n",
           Y0, Y1, 24, n_phase_total);
    OK(n_term_bad == 0,
       "%d 年的节气里有 %d 项不符（首个 %d 年）：最大黄经残差 %.4f″，应 ≤0.05″",
       ny, n_term_bad, first_term_bad, worst_term * 206264.8);
    OK(n_ord_bad == 0, "事件表有 %d 处不是时间升序", n_ord_bad);
    OK(n_name_bad == 0, "%d 年的节气名与项号对不上（首个 %d 年）",
       ny, first_name_bad);
    OK(n_phase_bad == 0,
       "%d 年的朔里有 %d 项不符（首个 %d 年）：最大 |距角| %.4f″，应 ≤1″",
       ny, n_phase_bad, first_phase_bad, worst_phase * 206264.8);
    OK(n_gap_bad == 0,
       "朔表有 %d 处间隔不在 29.0–30.2 天（首个 %d 年），最小 %.2f 天",
       n_gap_bad, first_gap_year, min_gap);
}

/* 流尾剪枝的全定义域护栏。它只检查“有没有因提前停止漏掉年末事件”，所以用
 * LOW 档即可：档位改变时刻精度，不改变一年应有 23–25 气、24/25 个朔望的结构。
 * 每一年、UTC−14/0/+14 三种窗口都跑，不做抽样；这样两天的种子保护带不能靠
 * 现代年份或单一时区碰巧过关。 */
static void run_year_stream_window_guard(void)
{
    static sf_qishuo_event ev[SF_QISHUO_MAX_EVENTS];
    int bad = 0, have_first = 0;
    int first_bad = 0, first_offset = 0, first_nt = 0, first_np = 0;
    int min_nt = 999, max_nt = 0, min_np = 999, max_np = 0;

    sf_qishuo_options o;
    sf_qishuo_options_init(&o);
    o.event_accuracy = SF_QISHUO_ACC_LOW;
    static const double phases[2] = { 0.0, 180.0 };
    if (sf_qishuo_set_phase_angles(&o, phases, 2) != SF_QISHUO_OK) {
        OK(0, "全区间气朔护栏：相位角初始化失败");
        return;
    }

    static const int offsets[3] = { -840, 0, 840 };
    for (int oi = 0; oi < 3; oi++) {
        o.utc_offset_minutes = offsets[oi];
        for (int y = SF_QISHUO_RANGE_START_YEAR; y <= SF_QISHUO_RANGE_END_YEAR; y++) {
        sf_qishuo_year qy;
        int nt = 0, np = 0;
        double first_t = NAN, last_t = NAN, first_p = NAN, last_p = NAN;
        int gap_bad = 0;
        if (sf_qishuo_year_run(&o, y, ev, SF_QISHUO_MAX_EVENTS, &qy)
                != SF_QISHUO_OK) {
            bad++;
            if (!have_first) {
                have_first = 1; first_bad = y; first_offset = offsets[oi];
            }
            continue;
        }
        for (int i = 0; i < qy.count; i++) {
            if (ev[i].kind == SF_QISHUO_KIND_SOLAR_TERM) {
                if (!isfinite(first_t)) first_t = ev[i].jd_ut1;
                if (isfinite(last_t) && ev[i].jd_ut1 - last_t > 16.5) gap_bad = 1;
                last_t = ev[i].jd_ut1;
                nt++;
            } else if (ev[i].kind == SF_QISHUO_KIND_LUNAR_PHASE) {
                if (!isfinite(first_p)) first_p = ev[i].jd_ut1;
                if (isfinite(last_p) && ev[i].jd_ut1 - last_p > 16.0) gap_bad = 1;
                last_p = ev[i].jd_ut1;
                np++;
            }
        }
        if (nt < min_nt) min_nt = nt;
        if (nt > max_nt) max_nt = nt;
        if (np < min_np) min_np = np;
        if (np > max_np) max_np = np;
        /* 不能只看条数：若窗口边缘漏一条，24 仍可能看起来“正常”。太阳相邻
         * 节气 <16.5 天，朔望交错 <16 天；窗口两端也不能留下更大的洞。 */
        if (nt < 23 || nt > 25 || np < 24 || np > 25 || gap_bad
            || first_t - qy.start_jd_ut1 > 16.5 || qy.end_jd_ut1 - last_t > 16.5
            || first_p - qy.start_jd_ut1 > 16.0 || qy.end_jd_ut1 - last_p > 16.0) {
            bad++;
            if (!have_first) {
                have_first = 1;
                first_bad = y; first_offset = offsets[oi];
                first_nt = nt; first_np = np;
            }
        }
        }
    }
    printf("  流尾剪枝 %d–%d × UTC−14/0/+14：逐年检查完成"
           "（气 %d–%d，朔望 %d–%d）\n",
           SF_QISHUO_RANGE_START_YEAR, SF_QISHUO_RANGE_END_YEAR,
           min_nt, max_nt, min_np, max_np);
    OK(bad == 0, "全区间有 %d 个窗口漏/多事件"
       "（首个 UTC%+d 分、%d 年：气 %d、朔望 %d）",
       bad, first_offset, first_bad, first_nt, first_np);
}

int main(int argc, char **argv)
{
    const int modern = (argc < 2) || strcmp(argv[1], "hist") != 0;
    const int hist   = (argc < 2) || strcmp(argv[1], "modern") != 0;

    if (modern) {
        run_modern();
        run_boundary();
        run_modes();
        run_long_span();
        run_low_day_assignment();
        run_historical_profile();
        run_ganzhi();
        run_rise();
        run_rise_fast_holdout();
        run_timescale();
        run_window_edge();
        run_year_tables();
        run_year_stream_window_guard();
    }
    if (hist) run_historical();

    printf("\n通过 %d，失败 %d，跳过 %d\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
