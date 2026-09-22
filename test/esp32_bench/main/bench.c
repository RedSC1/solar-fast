/* ==================================================================
 * bench.c —— 在真芯片上测量 solar-fast 的绝对耗时
 *
 * 每一行均为 esp_timer 实测的墙上时间，非估算值，逐条打印到串口。
 *
 * ------------------------------------------------------------------
 * 测量方法（三条约束缺一不可）
 *
 *   1. 先预热再计时。首次调用需将代码段自 flash 载入 cache，该次耗时
 *      显著偏高，计入统计会污染结果。
 *   2. 每个操作重复 K 轮、每轮 N 遍，报告 min / 中位 / max。
 *      取 min 的依据：被测代码无依赖数据的分支，最快的一遍即未被中断
 *      干扰的一遍，最接近代码本身的执行时间。中位数作为旁证；max 用于
 *      观察抖动（cache 未命中 / WiFi 中断）。
 *   3. 每段之间 vTaskDelay。任务看门狗监视 IDLE 任务，长时间不让出会被
 *      判定饥饿并复位。
 *
 * 注意：两个影响基准有效性的编译开关写在 sdkconfig.defaults 中，不得删除 ——
 *   CPU 主频（ESP32-S3 的 IDF 默认值为 160 MHz，非 240）与优化级别
 *   （IDF 默认值为 -Og，非 -O2）。
 *
 * 可选的 UI 测试只在 HAS_UI_BENCH 定义且提供 UI 源码时编入。
 * 可选的 NREL SPA 对比只在本机提供参考源码时编入。
 * ================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
/* 私有头。IDF 5.5 将 esp_clk_cpu_freq 移入 esp_private/。
 * 使用它是因为主频必须实测而非读取 sdkconfig —— 表中每一行均以其为分母，
 * 取值错误会使整张表的量级失真。本工程未启用 DFS（动态调频），故实测值
 * 亦等于 sdkconfig 中的 240 MHz。 */
#include "esp_private/esp_clk.h"
#include "esp_idf_version.h"

#include "solar_fast.h"
#include "sf_calendar.h"
#include "sf_rise.h"
#include "sf_qishuo.h"
#include "sf_bazi.h"
#include "sf_fixed.h"
#ifdef SF_HAVE_LOCAL_SPA
#include "spa.h"
#endif

#ifdef HAS_UI_BENCH
#include "ui.h"
#endif

/* ==================================================================
 * 性能对比选项目关（测试完成后可置 0 或整段注释）
 * ================================================================== */
#ifdef SF_HAVE_LOCAL_SPA
#define ENABLE_SPA_COMPARE_BENCH 1   /* Local NREL SPA source present */
#else
#define ENABLE_SPA_COMPARE_BENCH 0
#endif
#define ENABLE_FIXED_TRIG_BENCH  1   /* 1 = 普通 double sin/cos vs 定点 Q31 查表性能测试 */
#define ENABLE_FLOAT_DIAG_BENCH  0   /* 0 = 关闭 double vs float 诊断表（阶段性测试，结论已定） */

/* ------------------------------------------------------------------
 * 输出：不使用 %f
 *
 * IDF 的 newlib 可能配置为 nano 格式化（此时 %f 无法输出），而本基准的
 * 每一行都只是「整数 + 两位小数」。自行拼接不依赖 sdkconfig 配置。
 * ------------------------------------------------------------------ */
static void pr2(double v)
{
    if (v < 0) { putchar('-'); v = -v; }
    long long ip = (long long)v;
    int fp = (int)((v - (double)ip) * 100.0 + 0.5);
    if (fp >= 100) { ip++; fp = 0; }      /* 四舍五入的进位边界 */
    printf("%lld.%02d", ip, fp);
}

/* ------------------------------------------------------------------
 * 计时骨架
 * ------------------------------------------------------------------ */
#define REPEATS 5

typedef void (*op_fn)(void);

static volatile double g_d;      /* 结果落地：不让编译器把整段优化掉 */
static volatile int    g_i;

/* 空函数 —— 测出计时器本身的开销（表中每一行均须减去该值才是净耗时） */
static void op_nop(void) { g_i = 1; }

static int cmp_i64(const void *a, const void *b)
{
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* 跑一轮基准：N 遍 × K 轮，打印每遍的 min / 中位 / max（单位 µs）。
 *
 * N（遍数）不是预先设定的。`want` 仅作为廉价操作的上界；实际遍数由
 * `iters_auto` 按一次调用的实测耗时反推，目标为「单轮 30 ms」：
 *   - 预先猜测会形成循环依赖 —— 「每个操作跑几遍」取决于「它有多贵」，
 *     而后者正是被测对象。猜大时，一个 200 ms 的整年表需运行十几秒；
 *     猜小时，一个 0.5 µs 的操作测得的结果全是计时器噪声。
 *   - 单轮 30 ms 远高于计时器自身抖动，故几遍一环均成立。 */
static int iters_auto(op_fn fn, int want)
{
    const int64_t t0 = esp_timer_get_time();
    fn();                                  /* 顺带当预热 */
    const int64_t one = esp_timer_get_time() - t0;
    if (one <= 0) return want;             /* 量不出来：按上界跑 */
    int n = (int)(30000 / one);
    if (n < 1)   n = 1;                    /* 贵到一轮就是一次调用 */
    if (n > want) n = want;
    return n;
}

static void bench(const char *name, int want, op_fn fn)
{
    int64_t t[REPEATS];
    const int iters = iters_auto(fn, want);
    for (int r = 0; r < REPEATS; r++) {
        const int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < iters; i++) fn();
        t[r] = esp_timer_get_time() - t0;
        vTaskDelay(1);                     /* 让 IDLE 跑一下，喂看门狗 */
    }
    int64_t s[REPEATS];
    memcpy(s, t, sizeof s);
    qsort(s, REPEATS, sizeof s[0], cmp_i64);

    printf("  %-30s ", name);
    pr2((double)s[0] / iters);       printf("  ");
    pr2((double)s[REPEATS / 2] / iters); printf("  ");
    pr2((double)s[REPEATS - 1] / iters); printf("   x%-6d\n", iters);
}

static void head(const char *title) { printf("\n== %s ==\n", title); }
static void cols(void)
{
    printf("  %-30s %8s  %8s  %8s   %s\n",
           "操作", "min", "中位", "max", "每轮遍数");
    printf("  %-30s %8s  %8s  %8s\n", "（单位 µs）", "", "", "");
}

/* ==================================================================
 * 被测对象的固定输入
 * ================================================================== */
static const double JD = 2460000.5;            /* 2023-02-25 00:00 UT */
static const double LAT = 39.9042, LON = 116.4074;
static double g_jd_tt;

static sf_cal_config    g_cfg;
static sf_bazi_options  g_bo;
static sf_bazi_chart    g_chart;
static sf_bazi_qiyun    g_qiyun;
static sf_bazi_dayun_entry g_dayun[10];
static sf_qishuo_event  g_ev[SF_QISHUO_MAX_EVENTS];
static double g_sink_d, g_sink_d2, g_sink_d3;

/* 出生读数：2000-01-01 12:00 +480（与 sim 的 shot_* 同一组，便于对照） */
static const sf_cal_datetime BIRTH = { 2000, 1, 1, 12, 0, 0.0 };

static void setup(void)
{
    g_jd_tt = sf_ut_to_tt(JD);
    sf_cal_config_init_china_standard_historical(&g_cfg, 480);

    sf_bazi_options_init(&g_bo);
    g_bo.cal             = g_cfg;
    g_bo.gender          = SF_BAZI_GENDER_MALE;
    g_bo.dayun_count     = SF_BAZI_DEFAULT_DAYUN_COUNT;
    g_bo.rat_hour_mode   = SF_GANZHI_RAT_HOUR_NO_SPLIT;
}

/* ==================================================================
 * §1 库单元操作
 * ================================================================== */
static void op_jd_ut(void)      { g_d = sf_julian_day_ut(2026, 9, 20, 12, 0, 0.0); }
static void op_tt_to_ut(void)   { g_d = sf_tt_to_ut(JD); }
static void op_utc_to_ut1(void) { g_d = sf_utc_to_ut1(JD); }
static void op_ut_to_tt(void)   { g_d = sf_ut_to_tt(JD); }

static void op_sunlon(void)     { g_d = sf_sun_apparent_longitude(JD); }
static void op_sunlat(void)     { g_d = sf_sun_apparent_latitude(JD); }
static void op_sundist(void)    { g_d = sf_sun_distance(JD); }
static void op_elong(void)      { g_d = sf_elongation(JD); }
static void op_sunradec(void)   { sf_sun_ra_dec(JD, &g_sink_d, &g_sink_d2); }
static void op_sunradecdist(void)
{
    sf_sun_ra_dec_dist(JD, &g_sink_d, &g_sink_d2, &g_sink_d3);
}
static void op_gast(void)       { g_d = sf_gast(JD, sf_ut_to_tt(JD)); }
static void op_sunalt(void)     { g_d = sf_sun_altitude(JD, LAT, LON, &g_sink_d); }
static void op_moonradec(void)
{
    sf_moon_ra_dec(JD, &g_sink_d, &g_sink_d2, &g_sink_d3);
}

/* 节气/朔：直接求值（给定目标黄经，一步算出时刻，不迭代） */
static void op_term_time(void)  { g_d = sf_solar_term_time(0.0); }
static void op_phase_time(void) { g_d = sf_lunar_phase_time(0.0); }

/* 节气/朔：完整求解（带迭代收敛，UI 实际使用的入口） */
static void op_solve_term(void)
{
    g_d = sf_solve_solar_term(0.0, JD);          /* 春分 */
}
static void op_solve_phase(void)
{
    g_d = sf_solve_lunar_phase(0.0, JD);         /* 朔 */
}

static void op_lunar(void)
{
    sf_lunar_date ld;
    g_i = sf_cal_from_instant_ut(&g_cfg, JD, &ld);
}

static void op_get_term(void)
{
    sf_cal_term_event ev;
    g_i = sf_cal_get_specific_term(&g_cfg, 2026, 21, &ev);   /* 立春 */
}
static void op_find_term(void)
{
    sf_cal_term_event ev;
    g_i = sf_cal_find_term(&g_cfg, JD, 1, SF_TERM_JIE, &ev);
}

static void op_four_pillars(void)
{
    sf_ganzhi_pillars p;
    sf_cal_datetime vt;
    sf_cal_datetime_from_jd(JD, &vt);
    g_i = sf_ganzhi_four_pillars(&g_cfg, JD, &vt,
                                 SF_GANZHI_RAT_HOUR_NO_SPLIT, &p);
}

static void op_rise(void)
{
    double r, s;
    g_i = sf_sun_rise_set(JD, LAT, LON, &r, &s);
}
static double bench_local_day_start(void)
{
    const double lo = LON / 360.0;
    return floor(JD + lo + 0.5) - lo - 0.5;
}
static void op_rise_accurate(void)
{
    sf_rise_opts_t o;
    sf_rise_set_t out;
    sf_rise_opts_default(&o);
    o.fast = SF_RISE_ACCURATE;
    g_i = sf_sun_rise_set_for_day(bench_local_day_start(), LAT, LON, &o, &out);
}
static void op_moon_rise(void)
{
    double r, s;
    g_i = sf_moon_rise_set(JD, LAT, LON, &r, &s);
}
static void op_moon_rise_accurate(void)
{
    sf_rise_opts_t o;
    sf_rise_set_t out;
    sf_rise_opts_default(&o);
    o.fast = SF_RISE_ACCURATE;
    g_i = sf_moon_rise_set_for_day(bench_local_day_start(), LAT, LON, &o, &out);
}
static void op_daylen(void) { g_d = sf_day_length_hours(JD, LAT, LON); }
static void op_rise_daylen(void)
{
    double r, s, h;
    g_i = sf_sun_rise_set_day_length(JD, LAT, LON, &r, &s, &h);
    g_d = h;
}

/* 整年气朔表 —— 设置页那两页每次换年都要重建一次（HIGH 档约 50 次全量求解） */
static void op_year_table(void)
{
    sf_qishuo_options o;
    sf_qishuo_options_init(&o);
    o.mode               = g_cfg.mode;
    o.day_boundary_mode  = g_cfg.day_boundary_mode;
    o.utc_offset_minutes = g_cfg.utc_offset_minutes;
    o.event_accuracy     = SF_QISHUO_ACC_HIGH;      /* 默认档 */
    o.include_solar_terms = 1;
    o.include_pentads     = 0;
    static const double PA[2] = { 0.0, 180.0 };     /* 朔 + 望 */
    (void)sf_qishuo_set_phase_angles(&o, PA, 2);
    sf_qishuo_year y;
    g_i = sf_qishuo_year_run(&o, 2026, g_ev, SF_QISHUO_MAX_EVENTS, &y);
}

/* 八字：排盘全链（出生页每次改动均需重算） */
static void op_bazi_chart(void)
{
    g_i = sf_bazi_chart_run(&g_bo, JD, &BIRTH, &g_chart);
}
static void op_bazi_qiyun(void)
{
    g_i = sf_bazi_qi_yun(&g_bo, JD, &BIRTH, g_chart.pillars.primary.year,
                         &g_qiyun);
}
static void op_bazi_dayun(void)
{
    g_i = sf_bazi_da_yun(&g_bo, &BIRTH, g_chart.pillars.primary.month,
                         &g_qiyun, g_dayun, 10);
}

/* 流运：纯整数运算，本组中最廉价的一档 */
static void op_flow_year(void)  { sf_ganzhi g; sf_bazi_flow_year(2035, &g); }
static void op_flow_month(void)
{
    sf_ganzhi g;
    sf_bazi_flow_month(g_chart.pillars.primary.year, 5, &g);
}
static void op_flow_day(void)
{
    sf_ganzhi g;
    sf_bazi_flow_day(&BIRTH, &g);
}
static void op_flow_hour(void)
{
    sf_ganzhi g;
    sf_bazi_flow_hour(g_chart.pillars.primary.day, 7, &g);
}

/* 运限「流日」层：本组 UI 中最昂贵的单个动作。
 * 与 sim 的 luck_days_rebuild 保持同一实现：解两个「节」（本月 + 下月），
 * 定出节气月的整数天区间，再逐日取柱。 */
static void op_luck_day_layer(void)
{
    /* 与 sim/ui_app.c 的 luck_days_rebuild 逐字一致，含两个 +1 边界条件
     * （丑月的节落在下一个公历年、子月的下一个月节同理）。不得简化 ——
     * 简化后的复刻已非同一被测对象，即使本次取值恰好相同。 */
    static const uint8_t kJie[12] = { 21, 23, 1, 3, 5, 7, 9, 11, 13, 15, 17, 19 };
    const int32_t year = 2032;              /* 流年卡的公历年 */
    const int mi = 2;                       /* 流月下标：0 = 寅月 */
    sf_cal_term_event a, b;
    if (sf_cal_get_specific_term(&g_cfg, year + (mi == 11 ? 1 : 0),
                                 kJie[mi], &a) != 0) return;
    if (sf_cal_get_specific_term(&g_cfg, year + (mi >= 10 ? 1 : 0),
                                 kJie[(mi + 1) % 12], &b) != 0) return;
    const double off = 480.0 / 1440.0;      /* 生效偏移 / 1440 */
    const int64_t first = (int64_t)floor(a.jd_ut + off + 0.5);
    const int64_t last  = (int64_t)floor(b.jd_ut + off + 0.5);
    for (int64_t jdn = first; jdn < last; jdn++) {
        int32_t y = 0, mo = 0, d = 0;
        sf_solar_date_from_day_number(jdn, &y, &mo, &d);
        sf_cal_datetime cd;
        memset(&cd, 0, sizeof cd);
        cd.year = y; cd.month = (uint8_t)mo; cd.day = (uint8_t)d;
        sf_ganzhi g;
        if (sf_bazi_flow_day(&cd, &g) != SF_BAZI_OK) return;
        g_i = (int)(g & 0x3f);              /* 结果落地，防止被优化掉 */
    }
}

static void section_lib(void)
{
    head("§1 库单元操作（含计时器开销，见 §0b）");
    cols();

    printf("  -- 时间换算 --\n");
    bench("sf_julian_day_ut",       2000, op_jd_ut);
    bench("sf_tt_to_ut",            2000, op_tt_to_ut);
    bench("sf_utc_to_ut1",          2000, op_utc_to_ut1);
    bench("sf_ut_to_tt",            2000, op_ut_to_tt);

    printf("  -- 日月位置（每帧都算的那几个）--\n");
    bench("sf_sun_apparent_longitude", 500, op_sunlon);
    bench("sf_sun_apparent_latitude",  500, op_sunlat);
    bench("sf_sun_distance",           500, op_sundist);
    bench("sf_elongation",             500, op_elong);
    bench("sf_sun_ra_dec",             500, op_sunradec);
    bench("sf_sun_ra_dec_dist",        500, op_sunradecdist);
    bench("sf_gast (+UT->TT)",          500, op_gast);
    bench("sf_sun_altitude",           500, op_sunalt);
    bench("sf_moon_ra_dec",            200, op_moonradec);

    printf("  -- 单个气/朔事件 --\n");
    bench("sf_solar_term_time (直接)",   500, op_term_time);
    bench("sf_lunar_phase_time (直接)",  500, op_phase_time);
    bench("sf_solve_solar_term (求解)",  100, op_solve_term);
    bench("sf_solve_lunar_phase (求解)", 100, op_solve_phase);

    printf("  -- 历法/节气 --\n");
    bench("sf_cal_from_instant_ut (农历)", 200, op_lunar);
    bench("sf_cal_get_specific_term",      200, op_get_term);
    bench("sf_cal_find_term",              200, op_find_term);
    bench("sf_ganzhi_four_pillars",        200, op_four_pillars);

    printf("  -- 出没 --\n");
    bench("sf_sun_rise_set FAST",   200, op_rise);
    bench("sf_sun_rise_set ACCURATE", 20, op_rise_accurate);
    bench("sf_moon_rise_set FAST",   50, op_moon_rise);
    bench("sf_moon_rise_set ACCURATE", 10, op_moon_rise_accurate);
    bench("sf_day_length_hours",    200, op_daylen);
    bench("sf_sun_rise_set_day_length", 200, op_rise_daylen);

    printf("  -- 整年气朔表（换年 / 改精度才重建）--\n");
    bench("sf_qishuo_year_run (HIGH)", 10, op_year_table);

    printf("  -- 八字 --\n");
    bench("sf_bazi_chart_run (排盘)",  100, op_bazi_chart);
    bench("sf_bazi_qi_yun (起运)",     100, op_bazi_qiyun);
    bench("sf_bazi_da_yun (大运 10 步)", 100, op_bazi_dayun);
    bench("sf_bazi_flow_year (纯整数)",   2000, op_flow_year);
    bench("sf_bazi_flow_month (纯整数)",  2000, op_flow_month);
    bench("sf_bazi_flow_day (纯整数)",    2000, op_flow_day);
    bench("sf_bazi_flow_hour (纯整数)",   2000, op_flow_hour);

    printf("  -- 运限「流日」整层（2 个节 + 整月逐日）--\n");
    bench("运限·流日那一层", 20, op_luck_day_layer);
}

#ifdef HAS_UI_BENCH
/* ==================================================================
 * §2 一帧（便携 UI 层，headless —— 不接屏、不接按键）
 *
 * tick 的失效与加载路径代价差着数量级，混在一起报平均没有意义：
 *   稳态      只有几个日月求值（60 fps 的常态）
 *   跨日      只置 daily dirty；当前页需要时才加载
 *   年表      进入气/朔页才整年重建
 *   太阳页    进入后才求当天日出日落
 *   排盘      八字全链
 *   流日      两个节 + 整月
 * 都用真的 dirty 标志触发，走的完全是真代码路径。
 * ================================================================== */
static uint16_t g_fb[UI_W * UI_H];
static ui_app   g_app;
static double   g_frame_jd;

/* 使 app 处于「有盘、表已建好、稳态」的状态。
 * 注意：出生信息与性别必须显式给出。ui_app_init 默认为「未选性别」，
 * 此时盘面只画「先选性别」两行提示，远低于画满六列的成本 —— 以该值
 * 作为「画一帧的成本」会严重偏低。 */
static void app_reset(void)
{
    ui_init(g_fb);
    ui_app_init(&g_app, JD);
    g_app.birth        = BIRTH;
    g_app.birth_offset = 480;
    g_app.gender       = SF_BAZI_GENDER_MALE;
    g_app.chart_dirty  = 1;
    ui_app_tick(&g_app, JD);          /* 第一次：建表 + 排盘 */
    ui_app_tick(&g_app, JD);          /* 第二次：稳态 */
    g_frame_jd = JD;
}

static void op_tick_steady(void)
{
    g_app.feature = UI_FEATURE_CALENDAR;
    g_app.page = UI_PAGE_TODAY;
    g_frame_jd += 1.0 / (86400.0 * 60.0);  /* 60 fps：每帧前进 1/60 秒 */
    ui_app_tick(&g_app, g_frame_jd);
}

/* 跨日只标 dirty：停在选择页时，不应偷算任何日/月/年表。 */
static void op_tick_day_invalidate(void)
{
    g_app.feature = -1;
    g_app.local_day_key = INT64_MIN;
    ui_app_tick(&g_app, JD + 2.0);
}

/* 主页可见时，跨日只懒加载“今日农历 + 日粒度月相朝向 + 当前四柱”。 */
static void op_tick_today_day(void)
{
    g_app.feature = UI_FEATURE_CALENDAR;
    g_app.page = UI_PAGE_TODAY;
    g_app.local_day_key = INT64_MIN;
    g_app.lunar_dirty = 1;
    g_app.moon_dirty = 1;
    g_app.pillars_ok = 0;
    ui_app_tick(&g_app, JD + 2.0);
}

/* 太阳页的按日冷缓存单列：它本来就比所有其它页面贵两个数量级。 */
static void op_tick_sun_day(void)
{
    g_app.feature = UI_FEATURE_CALENDAR;
    g_app.page = UI_PAGE_SUN;
    g_app.rise_cached = 0;
    ui_app_tick(&g_app, JD + 2.0);
}
static void op_tick_year_terms(void)
{
    g_app.feature = UI_FEATURE_CALENDAR;
    g_app.page = UI_PAGE_TERMS;
    g_app.year_term_dirty = 1;
    ui_app_tick(&g_app, JD);
}
static void op_tick_year_phases(void)
{
    g_app.feature = UI_FEATURE_CALENDAR;
    g_app.page = UI_PAGE_PHASES;
    g_app.year_phase_dirty = 1;
    ui_app_tick(&g_app, JD);
}
static void op_tick_chart(void)
{
    g_app.chart_dirty = 1;
    ui_app_tick(&g_app, JD);
}
static void op_tick_luck(void)
{
    g_app.luck_open  = UI_LUCK_LEVELS;    /* 开到流日那一层 */
    g_app.luck_dirty = 1;
    ui_app_tick(&g_app, JD);
}

static void render_page(int feature, int page)
{
    g_app.feature = feature;
    g_app.page    = page;
    ui_render(&g_app);
}

static void op_render_today(void)   { render_page(UI_FEATURE_CALENDAR, UI_PAGE_TODAY); }
static void op_render_lunar(void)   { render_page(UI_FEATURE_CALENDAR, UI_PAGE_LUNAR); }
static void op_render_terms(void)   { render_page(UI_FEATURE_CALENDAR, UI_PAGE_TERMS); }
static void op_render_phases(void)  { render_page(UI_FEATURE_CALENDAR, UI_PAGE_PHASES); }
static void op_render_sun(void)     { render_page(UI_FEATURE_CALENDAR, UI_PAGE_SUN); }
static void op_render_set(void)     { render_page(UI_FEATURE_CALENDAR, UI_PAGE_SETTINGS); }
static void op_render_birth(void)   { render_page(UI_FEATURE_BAZI, UI_BAZI_PAGE_BIRTH); }
static void op_render_chart(void)   { render_page(UI_FEATURE_BAZI, UI_BAZI_PAGE_CHART); }
static void op_render_luck(void)    { render_page(UI_FEATURE_BAZI, UI_BAZI_PAGE_LUCK); }
static void op_render_bset(void)    { render_page(UI_FEATURE_BAZI, UI_BAZI_PAGE_SETTINGS); }

/* 稳态帧 + 画出来 = 真机上「滚一帧」要做的全部 CPU 工作 */
static void op_frame_full(void)
{
    g_frame_jd += 1.0 / (86400.0 * 60.0);
    ui_app_tick(&g_app, g_frame_jd);
    ui_render(&g_app);
}

static void section_frame(void)
{
    head("§2 一帧（便携 UI 层，headless）");
    cols();

    printf("  -- ui_app_tick：失效与按页懒加载 --\n");
    app_reset(); bench("tick·稳态（每帧）",        200, op_tick_steady);
    app_reset(); bench("tick·跨日只置失效",          200, op_tick_day_invalidate);
    app_reset(); bench("tick·跨日主页懒加载",         50, op_tick_today_day);
    app_reset(); bench("tick·跨日太阳页冷加载",        20, op_tick_sun_day);
    app_reset(); bench("tick·进入节气年表",            20, op_tick_year_terms);
    app_reset(); bench("tick·进入朔望年表",            20, op_tick_year_phases);
    app_reset(); bench("tick·排盘（八字全链）",      50, op_tick_chart);
    app_reset(); bench("tick·流日（2 节 + 整月）",   50, op_tick_luck);

    printf("  -- ui_render 逐页（画进调用方给的帧缓冲）--\n");
    app_reset();
    for (int p = 0; p < UI_PAGE_COUNT; p++) {
        g_app.feature = UI_FEATURE_CALENDAR;
        g_app.page = p;
        ui_app_tick(&g_app, JD);
        char nm[48];
        snprintf(nm, sizeof nm, "render 万年历·第%d页", p);
        /* 各页共用一个 switch 分派表，避免逐页取函数指针 */
        static void (*const F[UI_PAGE_COUNT])(void) = {
            op_render_today, op_render_lunar, op_render_terms,
            op_render_phases, op_render_sun, op_render_set
        };
        bench(nm, 100, F[p]);
    }
    for (int p = 0; p < UI_BAZI_PAGE_COUNT; p++) {
        static void (*const F[UI_BAZI_PAGE_COUNT])(void) = {
            op_render_birth, op_render_chart, op_render_luck, op_render_bset
        };
        char nm[48];
        snprintf(nm, sizeof nm, "render 八字·第%d页", p);
        bench(nm, 100, F[p]);
    }

    printf("  -- 稳态整帧（tick + render）--\n");
    app_reset();
    g_app.feature = UI_FEATURE_CALENDAR;
    g_app.page = UI_PAGE_TODAY;
    bench("tick + render（主页一帧）", 100, op_frame_full);
}
#endif

/* ==================================================================
 * §5 精度档：级数截断的收益
 *
 * 两部分：
 *   A. 现有旋钮：「气朔精度」档（LOW/MED/HIGH）在 UI 上可切换，作用于整年表
 *      与单个气朔事件。
 *   B. 截断本身的空间：库提供一族 `_budget` 函数（诊断用），日月位置路径
 *      未接入 UI —— 测量接入后可节省的耗时。
 *
 * 三档档位在 sf_qishuo.h 中固定（LOW: L48/B7/R16 · MED: L129/B16/R59）。
 * 注意：截断会降低精度。本节只给出耗时上限；精度代价见 README 的截断曲线。
 * 接入 UI 前须按用途选档（阳历页可接受的截断，排盘未必可接受）。
 * ================================================================== */
static void op_year_low(void)
{
    sf_qishuo_options o;
    sf_qishuo_options_init(&o);
    o.mode               = g_cfg.mode;
    o.day_boundary_mode  = g_cfg.day_boundary_mode;
    o.utc_offset_minutes = g_cfg.utc_offset_minutes;
    o.event_accuracy     = SF_QISHUO_ACC_LOW;
    o.include_solar_terms = 1;
    o.include_pentads     = 0;
    static const double PA[2] = { 0.0, 180.0 };
    (void)sf_qishuo_set_phase_angles(&o, PA, 2);
    sf_qishuo_year y;
    g_i = sf_qishuo_year_run(&o, 2026, g_ev, SF_QISHUO_MAX_EVENTS, &y);
}
static void op_year_med(void)
{
    sf_qishuo_options o;
    sf_qishuo_options_init(&o);
    o.mode               = g_cfg.mode;
    o.day_boundary_mode  = g_cfg.day_boundary_mode;
    o.utc_offset_minutes = g_cfg.utc_offset_minutes;
    o.event_accuracy     = SF_QISHUO_ACC_MED;
    o.include_solar_terms = 1;
    o.include_pentads     = 0;
    static const double PA[2] = { 0.0, 180.0 };
    (void)sf_qishuo_set_phase_angles(&o, PA, 2);
    sf_qishuo_year y;
    g_i = sf_qishuo_year_run(&o, 2026, g_ev, SF_QISHUO_MAX_EVENTS, &y);
}

#define BUD_LOW   SF_EARTH_L_BUD_48,  SF_EARTH_B_BUD_7,  SF_EARTH_R_BUD_16
#define BUD_MED   SF_EARTH_L_BUD_129, SF_EARTH_B_BUD_16, SF_EARTH_R_BUD_59
#define BUD_HIGH  -1, -1, -1          /* -1 = 全量 */

static void op_sunlon_low(void)
{ g_d = sf_sun_apparent_longitude_budget(JD, SF_EARTH_L_BUD_48, 8, SF_EARTH_B_BUD_7, SF_EARTH_R_BUD_16); }
static void op_sunlon_med(void)
{ g_d = sf_sun_apparent_longitude_budget(JD, SF_EARTH_L_BUD_129, 20, SF_EARTH_B_BUD_16, SF_EARTH_R_BUD_59); }
static void op_sunlon_high(void)
{ g_d = sf_sun_apparent_longitude_budget(JD, -1, -1, -1, -1); }

static void op_elong_low(void)
{ g_d = sf_elongation_budget(JD, SF_MOON_L_BUD_48, SF_EARTH_L_BUD_48, SF_MOON_B_BUD_16, SF_EARTH_B_BUD_7, SF_EARTH_R_BUD_16); }
static void op_elong_med(void)
{ g_d = sf_elongation_budget(JD, SF_MOON_L_BUD_129, SF_EARTH_L_BUD_129, SF_MOON_B_BUD_64, SF_EARTH_B_BUD_16, SF_EARTH_R_BUD_59); }
static void op_elong_high(void)
{ g_d = sf_elongation_budget(JD, -1, -1, -1, -1, -1); }

static void op_moonradec_low(void)
{ sf_moon_ra_dec_budget(JD, &g_sink_d, &g_sink_d2, &g_sink_d3, SF_MOON_L_BUD_48, SF_MOON_B_BUD_16, SF_MOON_R_BUD_48); }
static void op_moonradec_med(void)
{ sf_moon_ra_dec_budget(JD, &g_sink_d, &g_sink_d2, &g_sink_d3, SF_MOON_L_BUD_129, SF_MOON_B_BUD_64, SF_MOON_R_BUD_129); }
static void op_moonradec_high(void)
{ sf_moon_ra_dec_budget(JD, &g_sink_d, &g_sink_d2, &g_sink_d3, -1, -1, -1); }

static void op_solve_term_low(void)
{ g_d = sf_solve_solar_term_budget(0.0, JD, SF_EARTH_L_BUD_48, SF_EARTH_B_BUD_7, SF_EARTH_R_BUD_16); }
static void op_solve_term_med(void)
{ g_d = sf_solve_solar_term_budget(0.0, JD, SF_EARTH_L_BUD_129, SF_EARTH_B_BUD_16, SF_EARTH_R_BUD_59); }
static void op_solve_term_high(void)
{ g_d = sf_solve_solar_term_budget(0.0, JD, -1, -1, -1); }
static void op_solve_term_qi_low(void)
{ g_d = sf_solve_solar_term_low(0.0, JD); }

static void op_solve_phase_low(void)
{ g_d = sf_solve_lunar_phase_budget(0.0, JD, SF_MOON_L_BUD_48, SF_EARTH_L_BUD_48, SF_MOON_B_BUD_16, SF_EARTH_B_BUD_7, SF_EARTH_R_BUD_16); }
static void op_solve_phase_med(void)
{ g_d = sf_solve_lunar_phase_budget(0.0, JD, SF_MOON_L_BUD_129, SF_EARTH_L_BUD_129, SF_MOON_B_BUD_64, SF_EARTH_B_BUD_16, SF_EARTH_R_BUD_59); }
static void op_solve_phase_high(void)
{ g_d = sf_solve_lunar_phase_budget(0.0, JD, -1, -1, -1, -1, -1); }
static void op_solve_phase_shuo_low(void)
{ g_d = sf_solve_lunar_phase_low(0.0, JD); }

static void section_accuracy(void)
{
    head("§5 精度档（LOW = 最省那档；HIGH = 现在 UI 用的默认）");
    cols();

    printf("  -- 现有旋钮：整年气朔表（UI「气朔精度」真能按到）--\n");
    bench("year_run ACC_LOW",  10, op_year_low);
    bench("year_run ACC_MED",  10, op_year_med);
    bench("year_run ACC_HIGH", 10, op_year_table);

    printf("  -- 太阳黄经（UI 这条路没有旋钮）--\n");
    bench("sunlon L48",  200, op_sunlon_low);
    bench("sunlon L129", 200, op_sunlon_med);
    bench("sunlon 全量", 200, op_sunlon_high);

    printf("  -- 距角（主页月相盘那块）--\n");
    bench("elong LOW",  100, op_elong_low);
    bench("elong MED",  100, op_elong_med);
    bench("elong 全量", 100, op_elong_high);

    printf("  -- 月亮位置（出没、月相朝向）--\n");
    bench("moon_ra_dec LOW",  100, op_moonradec_low);
    bench("moon_ra_dec MED",  100, op_moonradec_med);
    bench("moon_ra_dec 全量", 100, op_moonradec_high);

    printf("  -- 单个事件求解 --\n");
    bench("qi_low 短方程两步", 100, op_solve_term_qi_low);
    bench("solve_term LOW",   100, op_solve_term_low);
    bench("solve_term MED",   100, op_solve_term_med);
    bench("solve_term 全量",  100, op_solve_term_high);
    bench("shuo_low 短方程三步", 100, op_solve_phase_shuo_low);
    bench("solve_phase LOW",  50, op_solve_phase_low);
    bench("solve_phase MED",  50, op_solve_phase_med);
    bench("solve_phase 全量", 50, op_solve_phase_high);
}

/* ==================================================================
 * §7 归因：1166 µs 的构成
 *
 * 本节用于定位耗时所在阶段。级数内层（phase32 + Q31 查表 + int64 累加）与
 * 章动 sf_nutation_iau2000b 均为定点实现（见 sf_fixed.h），故「软浮点」这一
 * 总量结论（§3）虽成立，其分布位置仍须实测确定。当前测量指向组边界之后的外层
 * double 路径：帧投影的多项式替身、时间准备、组装配。
 *
 * 库内可直接测量的项：
 *   - 几何黄经 vs 视黄经：差值 = 章动 + 光行差段
 *   - 章动定点版 vs double 版：定点内层的节省量
 * ================================================================== */
static void op_nut_fixed(void)
{
    double dpsi, deps;
    sf_nutation_iau2000b(JD, SF_DEF_NUT, &dpsi, &deps);
}
static void op_nut_dbl(void)
{
    double dpsi, deps;
    sf_nutation_iau2000b_dbl(JD, SF_DEF_NUT, &dpsi, &deps);
}
static void op_nut9_fixed(void)
{
    double dpsi, deps;
    sf_nutation_iau2000b(JD, 9, &dpsi, &deps);
}
static void op_obliquity(void) { g_d = sf_mean_obliquity(JD); }
static void op_sunlon_geom(void)
{ g_d = sf_sun_geometric_longitude_budget(JD, -1, -1, -1); }
static void op_solar_rate(void) { g_d = sf_solar_rate(JD); }
static void op_elong_rate(void) { g_d = sf_elongation_rate(JD); }

static void section_attribution(void)
{
    head("§7 归因（把范围缩小到某一步）");
    cols();
    bench("sf_nutation_iau2000b (定点)", 200, op_nut_fixed);
    bench("sf_nutation_iau2000b_dbl",    200, op_nut_dbl);
    bench("章动只取 9 项（定点）",        200, op_nut9_fixed);
    bench("sf_mean_obliquity",           200, op_obliquity);
    bench("太阳黄经（几何，无章动）",      200, op_sunlon_geom);
    bench("sf_solar_rate",               200, op_solar_rate);
    bench("sf_elongation_rate",          200, op_elong_rate);
}

#if ENABLE_SPA_COMPARE_BENCH
/* ==================================================================
 * §3b NREL SPA 原版 (Reference) vs solar-fast 对齐基准对比
 *
 * 两侧使用同一 UT 瞬时与观测地点。计算范围存在以下差别：
 *   1. 太阳视位置 (Apparent RA/Dec/Dist)
 *      SPA: calculate_geocentric_sun_right_ascension_and_declination
 *           内部也计算恒星时。
 *      solar-fast: sf_sun_ra_dec_dist，输入对应的 TT。
 *
 *   2. 太阳地平坐标 (Topocentric Zenith / Azimuth / Elevation)
 *      SPA: spa_calculate (SPA_ZA 模式：算到天顶角/高度角/方位角)
 *      solar-fast: sf_sun_observed，另外计算均时差。
 * ================================================================== */
static spa_data g_spa_bench;

static void spa_bench_setup(void)
{
    memset(&g_spa_bench, 0, sizeof(g_spa_bench));
    g_spa_bench.year          = 2023;
    g_spa_bench.month         = 2;
    g_spa_bench.day           = 25;
    g_spa_bench.hour          = 0;
    g_spa_bench.minute        = 0;
    g_spa_bench.second        = 0;
    g_spa_bench.timezone      = 0.0;
    g_spa_bench.delta_ut1     = 0.0;
    g_spa_bench.delta_t       = (g_jd_tt - JD) * 86400.0;
    g_spa_bench.longitude     = 116.4074;
    g_spa_bench.latitude      = 39.9042;
    g_spa_bench.elevation     = 50.0;
    g_spa_bench.pressure      = 1013.25;
    g_spa_bench.temperature   = 15.0;
    g_spa_bench.slope         = 0.0;
    g_spa_bench.azm_rotation  = 0.0;
    g_spa_bench.atmos_refract = 0.5667;
    g_spa_bench.function      = SPA_ZA; // 只算到 Zenith / Azimuth，不跑 RTS 迭代
    
    // 提前计算一次确保 jd 与结构体字段填充
    spa_calculate(&g_spa_bench);
}

/* SPA 纯地心视赤经赤纬与距离 */
static void op_spa_apparent_pos(void)
{
    calculate_geocentric_sun_right_ascension_and_declination(&g_spa_bench);
    g_d = g_spa_bench.alpha + g_spa_bench.delta + g_spa_bench.r;
}

/* solar-fast 纯地心视赤经赤纬与距离 */
static void op_sf_apparent_pos(void)
{
    double ra, dec, dist;
    sf_sun_ra_dec_dist(g_jd_tt, &ra, &dec, &dist);
    g_d = ra + dec + dist;
}

/* SPA 地平坐标全链路（算到天顶角/高度角/方位角） */
static void op_spa_topo_full(void)
{
    spa_calculate(&g_spa_bench);
    g_d = g_spa_bench.zenith + g_spa_bench.azimuth;
}

/* solar-fast 地平坐标全要素合算（sf_sun_observed：周日视差/海拔/折射/天顶角/方位角/均时差） */
static void op_sf_observed_full(void)
{
    sf_sun_observed_t obs;
    sf_sun_observed(JD, 39.9042, 116.4074, 50.0, 1013.25, 15.0, &obs);
    g_d = obs.zenith_deg + obs.azimuth_deg + obs.equation_of_time_min;
}

/* 采光面入射角计算 */
static void op_sf_incidence(void)
{
    g_d = sf_solar_incidence_angle(45.0, 180.0, 30.0, 180.0);
}

/* 太阳正午上中天 */
static void op_sf_transit(void)
{
    g_d = sf_sun_transit(JD, 116.4074);
}

static void section_spa_compare(void)
{
    head("§3b NREL SPA 原版 (Reference) vs solar-fast 对比");
    spa_bench_setup();

    printf("  -- 1. 太阳地心视位置 (VSOP -> 章动 -> 光行差 -> 视赤经/赤纬/距离) --\n");
    cols();
    bench("NREL SPA 原版 (sun_ra_dec)",      40, op_spa_apparent_pos);
    bench("solar-fast (sf_sun_ra_dec_dist)", 40, op_sf_apparent_pos);

    printf("\n  -- 2. 站心太阳位置（本库另算均时差；非完全同范围）--\n");
    cols();
    bench("NREL SPA 原版 (spa_calculate ZA)", 40, op_spa_topo_full);
    bench("solar-fast (sf_sun_observed)",     40, op_sf_observed_full);

    printf("\n  -- 3. 太阳观测衍生计算 --\n");
    cols();
    bench("sf_solar_incidence_angle",       5000, op_sf_incidence);
    bench("sf_sun_transit (正午上中天)",     500, op_sf_transit);
}
#endif

#if ENABLE_FIXED_TRIG_BENCH
/* ==================================================================
 * §3 普通 sin/cos (libm double) vs 定点 Q31 查表 (sf_fixed)
 *
 * 对比维度：
 *   1. 普通 double (libm)：sin(rad), cos(rad) —— 走 LX7 软浮点
 *   2. 纯定点 Q31 查表：sf_sin_q31(phase), sf_cos_q31(phase)
 *      （级数 term inner loop 的真实情况：相位是纯整数累加，直接查表）
 *   3. 全链路定点：sf_angle_to_phase32(rad) -> sf_sin_q31 / cos_q31
 *      （从 double 弧度输入开始算，包含象限折叠与相位转换）
 * ================================================================== */
static double       g_trig_rad[64];
static sf_phase32_t g_trig_phase[64];

static void trig_bench_fill(void)
{
    for (int i = 0; i < 64; i++) {
        /* 覆盖 0 ~ 2π 四个象限 */
        g_trig_rad[i]   = (6.2831853071795864769 / 64.0) * (double)i + 0.05;
        g_trig_phase[i] = sf_angle_to_phase32(g_trig_rad[i]);
    }
}

/* 1. 普通 double libm */
static void trig_d_sin(void)
{
    double a = 0.0;
    for (int i = 0; i < 64; i++) a += sin(g_trig_rad[i]);
    g_d = a;
}
static void trig_d_cos(void)
{
    double a = 0.0;
    for (int i = 0; i < 64; i++) a += cos(g_trig_rad[i]);
    g_d = a;
}
static void trig_d_sincos(void)
{
    double a = 0.0;
    for (int i = 0; i < 64; i++) a += sin(g_trig_rad[i]) + cos(g_trig_rad[i]);
    g_d = a;
}

/* 2. 纯定点 Q31 查表（内层真实路径） */
static void trig_q31_sin(void)
{
    int32_t a = 0;
    for (int i = 0; i < 64; i++) a += sf_sin_q31(g_trig_phase[i]);
    g_d = (double)a;
}
static void trig_q31_cos(void)
{
    int32_t a = 0;
    for (int i = 0; i < 64; i++) a += sf_cos_q31(g_trig_phase[i]);
    g_d = (double)a;
}
static void trig_q31_sincos(void)
{
    int32_t a = 0;
    for (int i = 0; i < 64; i++)
        a += sf_sin_q31(g_trig_phase[i]) + sf_cos_q31(g_trig_phase[i]);
    g_d = (double)a;
}

/* 3. 全链路：double 弧度 -> sf_angle_to_phase32 -> Q31 查表 */
static void trig_full_sin(void)
{
    int32_t a = 0;
    for (int i = 0; i < 64; i++)
        a += sf_sin_q31(sf_angle_to_phase32(g_trig_rad[i]));
    g_d = (double)a;
}
static void trig_full_cos(void)
{
    int32_t a = 0;
    for (int i = 0; i < 64; i++)
        a += sf_cos_q31(sf_angle_to_phase32(g_trig_rad[i]));
    g_d = (double)a;
}
static void trig_full_sincos(void)
{
    int32_t a = 0;
    for (int i = 0; i < 64; i++) {
        const sf_phase32_t p = sf_angle_to_phase32(g_trig_rad[i]);
        a += sf_sin_q31(p) + sf_cos_q31(p);
    }
    g_d = (double)a;
}

static void trig_row(const char *name, op_fn fn_base, op_fn fn_target)
{
    const int IT = 200, N = 64, R = 5;
    int64_t b_base = INT64_MAX, b_target = INT64_MAX;
    fn_base(); fn_target();
    for (int r = 0; r < R; r++) {
        int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < IT; i++) fn_base();
        int64_t dt = esp_timer_get_time() - t0;
        if (dt < b_base) b_base = dt;

        t0 = esp_timer_get_time();
        for (int i = 0; i < IT; i++) fn_target();
        dt = esp_timer_get_time() - t0;
        if (dt < b_target) b_target = dt;
        vTaskDelay(1);
    }
    const double ns_base   = (double)b_base * 1000.0 / (double)(IT * N);
    const double ns_target = (double)b_target * 1000.0 / (double)(IT * N);

    printf("  %-32s 基准(libm) ", name); pr2(ns_base);
    printf(" ns   定点 ");              pr2(ns_target);
    printf(" ns   加速比 ");           pr2(ns_target > 0.0 ? ns_base / ns_target : 0.0);
    printf("x\n");
}

static void section_fixed_trig(void)
{
    head("§3 普通 sin/cos (libm double) vs 定点 Q31 查表（每元素，ns）");
    trig_bench_fill();

    printf("  -- 纯定点查表（输入已是 phase32，级数内层真实路径）--\n");
    trig_row("sin    (libm vs sf_sin_q31)",     trig_d_sin,    trig_q31_sin);
    trig_row("cos    (libm vs sf_cos_q31)",     trig_d_cos,    trig_q31_cos);
    trig_row("sincos (libm vs q31 组合)",       trig_d_sincos, trig_q31_sincos);

    printf("  -- 带弧度转换（double 弧度 -> sf_angle_to_phase32 -> 查表）--\n");
    trig_row("sin    (全链路含相位折叠)",        trig_d_sin,    trig_full_sin);
    trig_row("cos    (全链路含相位折叠)",        trig_d_cos,    trig_full_cos);
    trig_row("sincos (全链路含相位折叠)",        trig_d_sincos, trig_full_sincos);
}
#endif

#if ENABLE_FLOAT_DIAG_BENCH
/* ==================================================================
 * double vs float 诊断对比（默认关闭）
 * ================================================================== */
static double g_xd[64];
static float  g_xf[64];

static void fp_fill(void)
{
    for (int i = 0; i < 64; i++) {
        g_xd[i] = 0.5 + (double)i * 0.03125;
        g_xf[i] = (float)g_xd[i];
    }
}

static void fp_d_muladd(void) { double a = 1.000001; for (int i=0;i<64;i++) a = a * 1.0000001 + g_xd[i]; g_d = a; }
static void fp_f_muladd(void) { float  a = 1.000001f; for (int i=0;i<64;i++) a = a * 1.0000001f + g_xf[i]; g_d = (double)a; }
static void fp_d_div(void)    { double a = 1.0; for (int i=0;i<64;i++) a = (a + g_xd[i]) / 1.0000007; g_d = a; }
static void fp_f_div(void)    { float  a = 1.0f; for (int i=0;i<64;i++) a = (a + g_xf[i]) / 1.0000007f; g_d = (double)a; }
static void fp_d_sqrt(void)   { double a = 0.0; for (int i=0;i<64;i++) a += sqrt(g_xd[i] + 1.0); g_d = a; }
static void fp_f_sqrt(void)   { float  a = 0.0f; for (int i=0;i<64;i++) a += sqrtf(g_xf[i] + 1.0f); g_d = (double)a; }
static void fp_d_sin(void)    { double a = 0.0; for (int i=0;i<64;i++) a += sin(g_xd[i]); g_d = a; }
static void fp_f_sin(void)    { float  a = 0.0f; for (int i=0;i<64;i++) a += sinf(g_xf[i]); g_d = (double)a; }
static void fp_d_cos(void)    { double a = 0.0; for (int i=0;i<64;i++) a += cos(g_xd[i]); g_d = a; }
static void fp_f_cos(void)    { float  a = 0.0f; for (int i=0;i<64;i++) a += cosf(g_xf[i]); g_d = (double)a; }
static void fp_d_atan2(void)  { double a = 0.0; for (int i=0;i<64;i++) a += atan2(g_xd[i], 1.5); g_d = a; }
static void fp_f_atan2(void)  { float  a = 0.0f; for (int i=0;i<64;i++) a += atan2f(g_xf[i], 1.5f); g_d = (double)a; }
static void fp_d_exp(void)    { double a = 0.0; for (int i=0;i<64;i++) a += exp(g_xd[i] * 0.01); g_d = a; }
static void fp_f_exp(void)    { float  a = 0.0f; for (int i=0;i<64;i++) a += expf(g_xf[i] * 0.01f); g_d = (double)a; }
static void fp_d_pow(void)    { double a = 0.0; for (int i=0;i<64;i++) a += pow(g_xd[i], 1.5); g_d = a; }
static void fp_f_pow(void)    { float  a = 0.0f; for (int i=0;i<64;i++) a += powf(g_xf[i], 1.5f); g_d = (double)a; }

/* 每遍处理 64 个元素；表中数值为每元素 ns，非每遍 */
static void fp_row(const char *nm, op_fn fd, op_fn ff)
{
    const int IT = 200, N = 64, R = 5;
    int64_t bd = INT64_MAX, bf = INT64_MAX;
    fd(); ff();
    for (int r = 0; r < R; r++) {
        int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < IT; i++) fd();
        int64_t dt = esp_timer_get_time() - t0;
        if (dt < bd) bd = dt;

        t0 = esp_timer_get_time();
        for (int i = 0; i < IT; i++) ff();
        dt = esp_timer_get_time() - t0;
        if (dt < bf) bf = dt;
        vTaskDelay(1);
    }
    /* ns/元素 = µs * 1000 / (IT * N) */
    const double nd = (double)bd * 1000.0 / (double)(IT * N);
    const double nf = (double)bf * 1000.0 / (double)(IT * N);
    printf("  %-8s  double ", nm); pr2(nd);
    printf(" ns      float ");     pr2(nf);
    printf(" ns      倍数 ");      pr2(nf > 0.0 ? nd / nf : 0.0);
    printf("x\n");
}

static void section_fp(void)
{
    head("§3 double vs float（每元素，ns）");
    fp_fill();
    fp_row("乘加", fp_d_muladd, fp_f_muladd);
    fp_row("除法", fp_d_div,    fp_f_div);
    fp_row("sqrt", fp_d_sqrt,   fp_f_sqrt);
    fp_row("sin",  fp_d_sin,    fp_f_sin);
    fp_row("cos",  fp_d_cos,    fp_f_cos);
    fp_row("atan2", fp_d_atan2, fp_f_atan2);
    fp_row("exp",  fp_d_exp,    fp_f_exp);
    fp_row("pow",  fp_d_pow,    fp_f_pow);
}
#endif

/* ==================================================================
 * §0 / §4 环境与收尾
 * ================================================================== */
static const char *chip_name(esp_chip_model_t m)
{
    switch (m) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32C2: return "ESP32-C2";
    case CHIP_ESP32C6: return "ESP32-C6";
    case CHIP_ESP32H2: return "ESP32-H2";
    default:           return "?";
    }
}

static void section_env(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    printf("  %-22s %s rev v%d.%d，%d 核\n", "芯片", chip_name(ci.model),
           ci.revision / 100, ci.revision % 100, ci.cores);
    printf("  %-22s %d MHz\n", "CPU 主频", (int)(esp_clk_cpu_freq() / 1000000));
    printf("  %-22s %s\n", "IDF", esp_get_idf_version());
    /* cache 几何必须打进表头：级数引擎每求一次值要走约 100 KB 系数表，
     * 默认 I 16 KB 与 bigcache 的 32 KB 之间差 1.7~1.8 倍。那两行只写在
     * sdkconfig.bigcache 里，而 sdkconfig 是 gitignore 的展开产物 —— 重建
     * 仓库后会静默退回默认，数字就变了而源码一个字没动。 */
#ifdef CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE
    printf("  %-22s I %u KB / D %u KB\n", "Cache 口径",
           (unsigned)(CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE / 1024),
           (unsigned)(CONFIG_ESP32S3_DATA_CACHE_SIZE / 1024));
#endif
    printf("  %-22s %u B\n", "内部 RAM 空闲", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#ifdef HAS_UI_BENCH
    printf("  %-22s %u B\n", "sizeof(ui_app)", (unsigned)sizeof(ui_app));
    printf("  %-22s %u B = %u KB\n", "帧缓冲", (unsigned)sizeof(g_fb),
           (unsigned)(sizeof(g_fb) / 1024));
#endif
    printf("  %-22s %u B\n", "sizeof(sf_bazi_chart)", (unsigned)sizeof(sf_bazi_chart));
}

#ifdef HAS_UI_BENCH
static void section_spi_note(void)
{
    head("§4 没有量、只能算的那一项：把帧推给屏");
    /* 160×128×2 B = 40960 B；按常见 SPI 时钟 40 / 80 MHz 各算一次。
     * 非实测值 —— 板上未接屏。列出以补全「整帧」的账面。 */
    const double bytes = (double)(UI_W * UI_H * 2);
    const double bits  = bytes * 8.0;
    printf("  帧缓冲 %u B = %u bit（RGB565，跟 ST7735S 的像素格式一致，无需转换）\n",
           (unsigned)bytes, (unsigned)bits);
    printf("  SPI 40 MHz → ");
    pr2(bits / 40e6 * 1000.0);
    printf(" ms    SPI 80 MHz → ");
    pr2(bits / 80e6 * 1000.0);
    printf(" ms\n");
    printf("  注意：上表为推算值（按 DQ 满速），非实测值；真机还需叠加\n");
    printf("     驱动开销 + DMA 排队，实测须接屏后另行测量。\n");
}
#endif

void app_main(void)
{
    printf("\n");
    printf("=========================================================\n");
    printf("  solar-fast 真机基准 —— 值越小越好，min 那列最可信\n");
    printf("=========================================================\n");
    printf("\n== §0 环境 ==\n");
    section_env();

    setup();
#ifdef HAS_UI_BENCH
    ui_init(g_fb);
    ui_app_init(&g_app, JD);
#endif

    /* 计时器本身的开销：下面每一行都含它，量出来好让读者心里有数 */
    head("§0b 计时器开销（下面每一行都含它）");
    cols();
    bench("空函数（量的是 esp_timer 的噪声）", 20000, op_nop);

    section_lib();
    section_accuracy();
    section_attribution();
#ifdef HAS_UI_BENCH
    section_frame();
#endif
#if ENABLE_FIXED_TRIG_BENCH
    section_fixed_trig();
#endif
#if ENABLE_SPA_COMPARE_BENCH
    section_spa_compare();
#endif
#if ENABLE_FLOAT_DIAG_BENCH
    section_fp();
#endif
#ifdef HAS_UI_BENCH
    section_spi_note();
#endif

    printf("\n=========================================================\n");
    printf("  完。把整段串口输出贴回会话即可。\n");
    printf("=========================================================\n");
}
