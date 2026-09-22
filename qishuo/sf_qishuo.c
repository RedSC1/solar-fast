/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_qishuo.c —— 一年的节气与月相事件表
 *
 * 对应 `taiyin-lite/src/qi-shuo.js` 的 `getQiShuoYear`。关键接口语义如下：
 *
 * 1. **窗口两端按 utcOffsetMinutes 的民用 1 月 1 日 00:00 定**，且必须
 *    「先算当地 JD，再减去偏移得到 UTC，再转 UT1」—— 不是「先 UTC 再减」。
 *    窗口左闭右开，判据一律用 **jdUT1**（对应 JS 的 `solved.jdUT1 < end`）。
 *
 * 2. **节气按角度逐个迭代**，不按时间顺序推。24 个目标角各自从
 *    「窗口起点 + 前向相位/2π × 回归年」起种，然后 `while (< end)` 反复发射，
 *    每轮把种子推进一整个回归年。所以**同一个角度在一年里可能出现两次**
 *    （民用年比回归年长时），`index` 会重复（JS 的注释亦记此条，
 *    测试里 `冬至·三候` 出现两次即属此情形）。
 *
 * 3. **七十二候用同一套、只是 stepCount = 72**（5° 一档）；开着 includeSolarTerms
 *    时把 `pentadIndex === 0` 的那些**丢掉**（它们与节气同角，重复）。所以
 *    气+候一起是 24 + 48 = 72 条，不是 96。
 *
 * 4. **月相按角度分流**：每个相位角一条独立的流水线，各自从种子起滚，
 *    `index` 是**该条流水线自己的序号**（0,1,2…），不是角度序号。四个相位角
 *    就是四条从 0 开始的独立序号。
 *
 * 5. **排序**：jdUT1 → kind 字典序（`lunar-phase` < `pentad` < `solar-term`，
 *    纯 ASCII 序）→ index。
 *
 * 6. **当地钟面用 jdUTC 拆**，不是 jdUT1（`ZonedTime.fromJulianTime` 里
 *    `calendarDateFromJulianDay(jdUTC + offset/1440)`）。两者差 DUT1（≤0.9 s），
 *    平时不可见，但秒的小数位会不一致。
 *
 * 7. **归日的结构偏移**：非 LOCAL_ASTRONOMICAL 一律钉在 UTC+8（480 分），
 *    与 localTime 用的 utcOffsetMinutes **无关** —— 两者不一样正是
 *    `assignmentDiffersFromLocalDate` 要报告的内容。历史档只在 mode 为
 *    historical **且**该事件有 historicalKind 时才查（节气与初候为
 *    'solarTerm'，只有 0° 的朔为 'newMoon'）。
 *
 * 档位（不同预算 = 不同精度）见 sf_qishuo.h 的实测表与 experimental/tier_ladder.py。
 * ================================================================== */
#include "sf_qishuo.h"

#include "sf_internal.h"
#include "solar_fast.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------
 * 名表那几个函数都是「写进**调用方给的** (buf, n)」这一形态。GCC 对
 * `snprintf(buf, n, ...)` 一律按 **n = 1** 的最坏情况算，于是每一条
 * 都报 `-Wformat-truncation`，而 **ESP-IDF 把这个警告当错误** ——
 * 不关的话整个工程编不过（这里是最先撞上的一处）。
 *
 * 该警告是**假阳性**：「缓冲小就截断」正是 snprintf 的契约，不是 bug。
 * n = 1 时它写 0 字节，绝不溢出。另一个来源是浮点：`%.6f` 一个 double
 * GCC 按全量程算最坏 318 个字符（DBL_MAX 有 309 位），而调用点上的值
 * 早已被夹到 [0,360) —— GCC 的浮点值域分析不做这种跨语句推导。
 *
 * 注意：**只对出问题的函数夹住诊断**，不关整个文件：关整文件会把将来
 * 真的截断一起吞掉。clang 不认这个 warning 名，所以只对 GCC 开。
 * ------------------------------------------------------------------ */
#if defined(__GNUC__) && !defined(__clang__)
#  define SF_NO_TRUNC_WARN_OPEN()  _Pragma("GCC diagnostic push") \
    _Pragma("GCC diagnostic ignored \"-Wformat-truncation\"")
#  define SF_NO_TRUNC_WARN_CLOSE() _Pragma("GCC diagnostic pop")
#else
#  define SF_NO_TRUNC_WARN_OPEN()
#  define SF_NO_TRUNC_WARN_CLOSE()
#endif

/* 照抄 qi-shuo.js:19-25 的常量 */
#define SF_QS_TWO_PI               6.2831853071795862
#define SF_QS_DAYS_PER_TROPICAL_YEAR 365.2422
#define SF_QS_DAYS_PER_SYNODIC_MONTH 29.53058886
#define SF_QS_CHINA_OFFSET_MINUTES 480
#define SF_QS_ROOT_EQUALITY_DAYS   1e-8
#define SF_QS_PHASE_DEDUP          1e-10
/* 从一个已收敛事件加平均周期得到下一次的种子。真实太阳回归差远小于一天，
 * 朔望月相对平均值的摆动约 0.3 天；留 2 天后若种子仍在窗口右端之外，真解
 * 不可能再掉回窗口。这样不必为每条流额外求一个“算完才丢”的事件。 */
#define SF_QS_RECURRENCE_GUARD_DAYS 2.0

/* ------------------------------------------------------------------
 * 精度档：同一个求解器的三种预算，默认最全那一档。
 * ------------------------------------------------------------------ */
typedef struct {
    int l, b, r;            /* 地球（节气与月相共用） */
    int moon_l, moon_b;     /* 月球（只有月相用） */
} sf_qs_tier_t;

/* 注意：**每一条轴都必须单调**。若低档某一轴的预算反而多于中档，
 * 即为配置错误，且使「往下调就是截断」不成立。
 * 数字是 1900–2100 全部事件相对 HIGH 自比的 rms/max（秒）。 */
static const sf_qs_tier_t SF_QS_TIERS[3] = {
    /* LOW  —— 节气 27.28 s(rms) 112.10 s(max) / 朔 14.77 s  48.50 s */
    { SF_EARTH_L_BUD_48,   SF_EARTH_B_BUD_7,  SF_EARTH_R_BUD_16,
      SF_MOON_L_BUD_48,    SF_MOON_B_BUD_16 },
    /* MED  —— 节气  3.23 s  12.93 s        / 朔  2.96 s   9.89 s */
    { SF_EARTH_L_BUD_129,  SF_EARTH_B_BUD_16, SF_EARTH_R_BUD_59,
      SF_MOON_L_BUD_129,   SF_MOON_B_BUD_64 },
    /* HIGH —— 全量，就是库原来的默认预算 */
    { SF_EARTH_L_BUD_FULL, SF_EARTH_B_BUD_FULL, SF_EARTH_R_BUD_192,
      SF_MOON_L_BUD_FULL,  SF_MOON_B_BUD_FULL },
};

/* 相位角最多 8 个 → 一条流水线最多 14 个朔望月。容量上界的余量。 */
#define SF_QS_PHASES_PER_ANGLE 14

/* ==================================================================
 * 名表 / 名字
 * ================================================================== */

const char *const SF_PENTAD_SUFFIXES[3] = { "初候", "二候", "三候" };

const char *sf_lunar_phase_name(double angle_deg, char *buf, size_t n)
{
    static const struct { double angle; const char *name; } CARD[4] = {
        {   0.0, "朔"   },
        {  90.0, "上弦" },
        { 180.0, "望"   },
        { 270.0, "下弦" },
    };
    if (!buf || n == 0) return buf;
    SF_NO_TRUNC_WARN_OPEN();
    for (int i = 0; i < 4; i++) {
        if (fabs(CARD[i].angle - angle_deg) < SF_QS_PHASE_DEDUP) {
            snprintf(buf, n, "%s", CARD[i].name);
            return buf;
        }
    }
    /* JS: `${Number(angleDeg.toFixed(6))}°月相` —— toFixed(6) 后过 Number()
     * 会把尾随零去掉。所以先只把**数字**格式化出来削尾，再拼后缀。
     * 注意：不得在拼好的整串上从末尾削零：末尾是「°月相」这几个多字节字，
     * 第一个字节就不是 '0'，循环立刻断掉，无法削除（对拍即由此发现：
     * 本库出「135.000000°月相」，JS 出「135°月相」）。 */
    /* 注意：相位角**先夹住再格式化**，两个理由：
     *   1. `%.6f` 一个 double 的最坏情况是 318 个字符（DBL_MAX 有 309 位），
     *      GCC 按全量程算，于是 40 字节的缓冲报「可能截断」——而 IDF 把
     *      `-Wformat-truncation` **当错误**，整个工程编不过。
     *   2. 这个函数是 public 的：直接喂 NaN / 1e300 进来，夹之前会吐出
     *      「nan°月相」或者一截 39 字节的畸形名字。夹之后是个正常值。
     * 对**合法**输入这不是行为改变：set_phase_angles 已经把每个角
     * positiveMod 到 [0,360) 了，夹逼在这里恒不触发。 */
    double a = angle_deg;
    if (!(a >= 0.0)) a = 0.0;          /* 恒假也收：NaN 走到这里 */
    else if (a > 360.0) a = 360.0;
    char num[40];
    snprintf(num, sizeof num, "%.6f", a);
    size_t len = strlen(num);
    while (len > 0 && num[len - 1] == '0') num[--len] = '\0';
    if (len > 0 && num[len - 1] == '.') num[--len] = '\0';
    if (len == 0) { num[0] = '0'; num[1] = '\0'; }
    snprintf(buf, n, "%s°月相", num);
    SF_NO_TRUNC_WARN_CLOSE();
    return buf;
}

const char *sf_qishuo_event_name(const sf_qishuo_event *ev, char *buf, size_t n)
{
    if (!buf || n == 0) return buf;
    buf[0] = '\0';
    if (!ev) return buf;
    if (ev->kind == SF_QISHUO_KIND_LUNAR_PHASE)
        return sf_lunar_phase_name(ev->phase_angle_deg, buf, n);
    if (ev->term_index < 0 || ev->term_index >= 24) return buf;
    SF_NO_TRUNC_WARN_OPEN();
    if (ev->kind == SF_QISHUO_KIND_PENTAD
        && ev->pentad_index >= 0 && ev->pentad_index < 3)
        snprintf(buf, n, "%s·%s", SF_SOLAR_TERM_NAMES[ev->term_index],
                 SF_PENTAD_SUFFIXES[ev->pentad_index]);
    else
        snprintf(buf, n, "%s", SF_SOLAR_TERM_NAMES[ev->term_index]);
    SF_NO_TRUNC_WARN_CLOSE();
    return buf;
}

/* ==================================================================
 * 选项
 * ================================================================== */

void sf_qishuo_options_init(sf_qishuo_options *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
    o->mode = SF_CAL_CHINA_STANDARD_HISTORICAL;   /* JS 默认 'historical' */
    o->day_boundary_mode = SF_CAL_FIXED_UTC_OFFSET;
    o->utc_offset_minutes = SF_QS_CHINA_OFFSET_MINUTES;
    o->meridian_deg = NAN;                        /* 注意：NAN = 未给，不是 0 */
    o->event_accuracy = SF_QISHUO_ACC_HIGH;       /* 注意：JS 是 mid；见头注差异 1 */
    o->include_solar_terms = 1;
    o->include_pentads = 0;
    o->n_phase_angles = 1;
    o->phase_angles_deg[0] = 0.0;                 /* JS 默认 [0] = 只要朔 */
}

int sf_qishuo_set_phase_angles(sf_qishuo_options *o, const double *deg, int n)
{
    if (!o || n < 0 || n > SF_QISHUO_MAX_PHASE_ANGLES) return SF_QISHUO_ERR_ANGLES;
    if (n > 0 && !deg) return SF_QISHUO_ERR_ANGLES;

    double tmp[SF_QISHUO_MAX_PHASE_ANGLES];
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!isfinite(deg[i])) return SF_QISHUO_ERR_ANGLES;
        /* 同 JS 的 positiveMod(value, 360)：()%m + m) % m，两次取模 */
        const double v = fmod(fmod(deg[i], 360.0) + 360.0, 360.0);
        int dup = 0;
        for (int j = 0; j < k; j++)
            if (fabs(tmp[j] - v) < SF_QS_PHASE_DEDUP) { dup = 1; break; }
        if (!dup) tmp[k++] = v;
    }
    /* 去重后升序（JS 是 .sort((a,b)=>a-b)）—— 顺序决定了四条流水线的发射次序，
     * 虽然最后还要整体排序，但 index 是按这个顺序分配的，所以必须一致。 */
    for (int i = 1; i < k; i++) {
        const double x = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > x) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = x;
    }
    o->n_phase_angles = k;
    for (int i = 0; i < k; i++) o->phase_angles_deg[i] = tmp[i];
    return SF_QISHUO_OK;
}

/* ==================================================================
 * 校验
 * ================================================================== */

static int qs_validate(int32_t year, const sf_qishuo_options *o, int capacity)
{
    if (!o) return SF_QISHUO_ERR_MODE;
    if (year < SF_QISHUO_RANGE_START_YEAR || year > SF_QISHUO_RANGE_END_YEAR)
        return SF_QISHUO_ERR_YEAR;
    if (o->utc_offset_minutes < -840 || o->utc_offset_minutes > 840)
        return SF_QISHUO_ERR_OFFSET;
    if (o->mode != SF_CAL_CHINA_STANDARD_HISTORICAL
        && o->mode != SF_CAL_LOCAL_ASTRONOMICAL
        && o->mode != SF_CAL_CHINA_STANDARD_ASTRONOMICAL)
        return SF_QISHUO_ERR_MODE;
    if (o->event_accuracy != SF_QISHUO_ACC_LOW
        && o->event_accuracy != SF_QISHUO_ACC_MED
        && o->event_accuracy != SF_QISHUO_ACC_HIGH)
        return SF_QISHUO_ERR_ACCURACY;
    if (o->day_boundary_mode != SF_CAL_FIXED_UTC_OFFSET
        && o->day_boundary_mode != SF_CAL_MEAN_SOLAR_MERIDIAN)
        return SF_QISHUO_ERR_BOUNDARY;

    const int has_meridian = !isnan(o->meridian_deg);
    if (has_meridian && !(fabs(o->meridian_deg) <= 180.0)) return SF_QISHUO_ERR_MERIDIAN;
    /* 注意：这两条 JS 在**所有 mode 下都跑**，包括根本不看 meridianDeg 的
     * china-astronomical —— 照抄，不得"优化"掉。 */
    if (o->day_boundary_mode == SF_CAL_MEAN_SOLAR_MERIDIAN && !has_meridian)
        return SF_QISHUO_ERR_MERIDIAN_REQUIRED;
    if (o->day_boundary_mode == SF_CAL_FIXED_UTC_OFFSET && has_meridian)
        return SF_QISHUO_ERR_MERIDIAN_UNEXPECTED;

    if ((o->include_solar_terms != 0 && o->include_solar_terms != 1)
        || (o->include_pentads != 0 && o->include_pentads != 1))
        return SF_QISHUO_ERR_FLAGS;
    if (o->n_phase_angles < 0 || o->n_phase_angles > SF_QISHUO_MAX_PHASE_ANGLES)
        return SF_QISHUO_ERR_ANGLES;
    for (int i = 0; i < o->n_phase_angles; i++)
        if (!isfinite(o->phase_angles_deg[i])) return SF_QISHUO_ERR_ANGLES;
    if (capacity < 0) return SF_QISHUO_ERR_CAPACITY;
    return SF_QISHUO_OK;
}

/* ==================================================================
 * 胶水
 * ================================================================== */

/* JS 的 structureOffset(mode, dayBoundaryMode, utcOffsetMinutes, meridianDeg) */
static double qs_structure_offset(const sf_qishuo_options *o)
{
    if (o->mode != SF_CAL_LOCAL_ASTRONOMICAL)
        return (double)SF_QS_CHINA_OFFSET_MINUTES / 1440.0;
    return (o->day_boundary_mode == SF_CAL_FIXED_UTC_OFFSET)
        ? (double)o->utc_offset_minutes / 1440.0
        : o->meridian_deg / 360.0;
}

static double qs_positive_mod(double v, double m)
{
    return fmod(fmod(v, m) + m, m);
}

/* ZonedTime.fromJulianTime(jdUT1, offset).toJSON() 的钟面部分 */
/* 注意：这里三样东西的来源**不一样**，不得图省事合成一个（对拍发现）：
 *
 *   localTime    ← jdUTC + 偏移   （JS 的 ZonedTime.fromJulianTime 内部走 jdUTC）
 *   localDay     ← jdUT1 + 偏移   （JS 的 civilDayNumber(jdUT1, off)，**不走 UTC**）
 *   localDate    ← **localDay**   （JS 是 dateFromDayNumber(localCivilDayNumber)，
 *                                  不是从 localTime 拆出来的）
 *
 * 平时 jdUT1 与 jdUTC 只差 DUT1（≤0.9 s），三种写法只在午夜前后 0.9 秒内才会
 * 分岔，所以任选其一都"看似正确"。但年份一大 ΔT 就显著（**10000 年时两者
 * 差 2.5 天**），此时按哪个算一目了然。
 *
 * 后果是极端的年份里 localDate 与 localTime 的日期**可以不一致** —— 那是上游
 * 的性质（它本身就不曾让两者一致），照抄，不得"修"。 */
static void qs_local_clock(double jd_utc, int32_t offset_minutes,
                           sf_cal_datetime *out)
{
    sf_cal_datetime_from_jd(jd_utc + (double)offset_minutes / 1440.0, out);
}

static void qs_decorate(const sf_qishuo_options *o, sf_qishuo_event *e,
                        int historical_kind)
{
    e->local_offset_minutes = o->utc_offset_minutes;
    qs_local_clock(e->jd_utc, o->utc_offset_minutes, &e->local_time);
    /* 日号按 **jdUT1** 算（见上面那段说明） */
    e->local_civil_day_number =
        sf_cal_civil_day_number(e->jd_ut1,
                                (double)o->utc_offset_minutes / 1440.0);
    /* 日期从**日号**来，不是从 localTime 拆 */
    {
        int32_t y = 0, mo = 0, d = 0;
        sf_solar_date_from_day_number(e->local_civil_day_number, &y, &mo, &d);
        e->local_date.year = y;
        e->local_date.month = (uint8_t)mo;
        e->local_date.day = (uint8_t)d;
    }

    e->assigned_civil_day_number =
        sf_cal_civil_day_number(e->jd_ut1, qs_structure_offset(o));
    e->assignment_source = (o->mode == SF_CAL_LOCAL_ASTRONOMICAL)
        ? SF_QISHUO_ASSIGN_LOCAL : SF_QISHUO_ASSIGN_CHINA;

    /* 历史档：只有 historical 模式 + 有 historicalKind 的事件才查。
     * 查不到（表外）就保持上面那套天文归日 —— sf_cal_historical_civil_day
     * 用 <0 表示"没有"，对应 JS 的 null。 */
    if (o->mode == SF_CAL_CHINA_STANDARD_HISTORICAL && historical_kind >= 0) {
        const int64_t h = sf_cal_historical_civil_day(historical_kind, e->jd_ut1);
        if (h >= 0) {
            e->assigned_civil_day_number = h;
            e->assignment_source = SF_QISHUO_ASSIGN_HISTORICAL;
        }
    }

    {
        int32_t y = 0, mo = 0, d = 0;
        sf_solar_date_from_day_number(e->assigned_civil_day_number, &y, &mo, &d);
        e->assigned_date.year = y;
        e->assigned_date.month = (uint8_t)mo;
        e->assigned_date.day = (uint8_t)d;
    }
    e->assignment_differs_from_local_date =
        (e->assigned_civil_day_number != e->local_civil_day_number);
}

typedef struct {
    const sf_qishuo_options *o;
    const sf_qs_tier_t      *tier;
    sf_qishuo_event         *events;
    int                      capacity;
    int                      total;      /* 解出来的总条数（可能 > capacity） */
    /* 开着 includeSolarTerms 时，七十二候里 pentadIndex==0 的那些与节气同角，
     * JS 直接把它们 filter 掉（不进 events，也不计数）—— 这里用同一个做法。 */
    int                      drop_pentad0;
} sf_qs_ctx;

/* 发射一条。容量满了就不存，但**照样计数** —— 这样 out->total 始终是真的，
 * 调用方可以按它重新分配再来一次（见 sf_qishuo_year_run 的容量约定）。 */
static void qs_emit(sf_qs_ctx *c, int32_t kind, int32_t index, int32_t term_index,
                    int32_t pentad_index, double target_rad, double target_deg,
                    double phase_rad, double phase_deg, double jd_tt,
                    int historical_kind)
{
    if (c->total < c->capacity) {
        sf_qishuo_event *e = &c->events[c->total];
        memset(e, 0, sizeof *e);
        e->kind = kind;
        e->index = index;
        e->term_index = term_index;
        e->pentad_index = pentad_index;
        e->target_longitude_rad = target_rad;
        e->target_longitude_deg = target_deg;
        e->phase_angle_rad = phase_rad;
        e->phase_angle_deg = phase_deg;
        /* jdTT 是权威值，UT1/UTC/ΔT 都由它推 —— 与 JS 一致
         * （JS 的 JulianTime.fromTT 也是先有 jdTT 再派生其余三个）。 */
        e->jd_tt = jd_tt;
        e->jd_ut1 = sf_tt_to_ut(jd_tt);
        e->jd_utc = sf_tt_to_utc(jd_tt);
        e->delta_t_seconds = (e->jd_tt - e->jd_ut1) * 86400.0;
        qs_decorate(c->o, e, historical_kind);
    }
    c->total++;
}

/* 平黄经/距角 的起点值 —— JS 的 solarLongitudeState / elongationState。 */
static void qs_solar_stream(sf_qs_ctx *c, double start, double end, int step_count)
{
    const double start_tt = sf_ut_to_tt(start);
    const double end_tt = sf_ut_to_tt(end);
    const double start_lon = sf_sun_apparent_longitude(start_tt);
    const sf_qs_tier_t *t = c->tier;

    for (int step = 0; step < step_count; step++) {
        const double target = (double)step / (double)step_count * SF_QS_TWO_PI;
        const double forward = qs_positive_mod(target - start_lon, SF_QS_TWO_PI);
        double near = start_tt + forward / SF_QS_TWO_PI * SF_QS_DAYS_PER_TROPICAL_YEAR;

        double jd_tt = sf_solve_solar_term_budget(target, near, t->l, t->b, t->r);
        if (!isfinite(jd_tt)) continue;
        /* 落在窗口左边就换到下一次（JS: solved.jdTT + DAYS_PER_TROPICAL_YEAR）。
         * 注意：之后每轮**都从上一个解**往前推一整个回归年，不从头重种 —— 种子
         * 贴着真解，sf_solve_tier 那个 ±170 天的邻居护栏才不会在平局带上乱跳。 */
        if (sf_tt_to_ut(jd_tt) < start - SF_QS_ROOT_EQUALITY_DAYS)
            jd_tt = sf_solve_solar_term_budget(target,
                        jd_tt + SF_QS_DAYS_PER_TROPICAL_YEAR, t->l, t->b, t->r);

        int guard = 0;
        while (isfinite(jd_tt) && guard++ < 8) {
            if (!(sf_tt_to_ut(jd_tt) < end - SF_QS_ROOT_EQUALITY_DAYS)) break;
            const int term_index = (step_count == 24) ? step : step / 3;
            const int pentad_index = (step_count == 72) ? (step % 3) : -1;
            /* 历史档案只认节气与初候（JS: stepCount===24 || pentadIndex===0） */
            const int hist = (step_count == 24 || pentad_index == 0)
                ? SF_CAL_HIST_SOLAR_TERM : -1;
            if (!(c->drop_pentad0 && pentad_index == 0))
                qs_emit(c,
                        step_count == 24 ? SF_QISHUO_KIND_SOLAR_TERM : SF_QISHUO_KIND_PENTAD,
                        step, term_index, pentad_index,
                        target, (double)step * 360.0 / (double)step_count,
                        NAN, NAN, jd_tt, hist);
            const double next_seed = jd_tt + SF_QS_DAYS_PER_TROPICAL_YEAR;
            if (next_seed >= end_tt + SF_QS_RECURRENCE_GUARD_DAYS) break;
            jd_tt = sf_solve_solar_term_budget(target, next_seed, t->l, t->b, t->r);
        }
    }
}

static void qs_lunar_stream(sf_qs_ctx *c, double start, double end)
{
    const double start_tt = sf_ut_to_tt(start);
    const double end_tt = sf_ut_to_tt(end);
    const double start_elong = sf_elongation(start_tt);
    const sf_qs_tier_t *t = c->tier;

    for (int ai = 0; ai < c->o->n_phase_angles; ai++) {
        const double angle_deg = c->o->phase_angles_deg[ai];
        const double target = angle_deg / 360.0 * SF_QS_TWO_PI;
        const double forward = qs_positive_mod(target - start_elong, SF_QS_TWO_PI);
        double near = start_tt + forward / SF_QS_TWO_PI * SF_QS_DAYS_PER_SYNODIC_MONTH;

        double jd_tt = sf_solve_lunar_phase_budget(target, near, t->moon_l, t->l,
                                                   t->moon_b, t->b, t->r);
        if (!isfinite(jd_tt)) continue;
        if (sf_tt_to_ut(jd_tt) < start - SF_QS_ROOT_EQUALITY_DAYS)
            jd_tt = sf_solve_lunar_phase_budget(target,
                        jd_tt + SF_QS_DAYS_PER_SYNODIC_MONTH,
                        t->moon_l, t->l, t->moon_b, t->b, t->r);

        int serial = 0, guard = 0;
        while (isfinite(jd_tt) && guard++ < SF_QS_PHASES_PER_ANGLE + 2) {
            if (!(sf_tt_to_ut(jd_tt) < end - SF_QS_ROOT_EQUALITY_DAYS)) break;
            /* 历史档案只认**正朔**（JS: |angleDeg| < 1e-10，归一化之后就是 == 0）。
             * 注意 360° 与 -360° 归一化后也是 0，一样算朔。 */
            const int hist = (angle_deg == 0.0) ? SF_CAL_HIST_NEW_MOON : -1;
            qs_emit(c, SF_QISHUO_KIND_LUNAR_PHASE, serial, -1, -1,
                    NAN, NAN, target, angle_deg, jd_tt, hist);
            serial++;
            const double next_seed = jd_tt + SF_QS_DAYS_PER_SYNODIC_MONTH;
            if (next_seed >= end_tt + SF_QS_RECURRENCE_GUARD_DAYS) break;
            jd_tt = sf_solve_lunar_phase_budget(target, next_seed,
                        t->moon_l, t->l, t->moon_b, t->b, t->r);
        }
    }
}

/* JS: events.sort((l, r) => l.time.jdUT1 - r.time.jdUT1
 *                        || l.kind.localeCompare(r.kind)
 *                        || l.index - r.index);
 * kind 的字典序是 'lunar-phase' < 'pentad' < 'solar-term'（纯 ASCII），
 * 而本库枚举值是 SOLAR_TERM=0 < PENTAD=1 < LUNAR_PHASE=2 —— 正好相反，
 * 所以按 (-kind) 比。插入排序：稳定，且 n 只有几十。 */
static int qs_kind_rank(int32_t kind)
{
    return 2 - kind;
}

static int qs_before(const sf_qishuo_event *a, const sf_qishuo_event *b)
{
    if (a->jd_ut1 != b->jd_ut1) return a->jd_ut1 < b->jd_ut1;
    const int ra = qs_kind_rank(a->kind), rb = qs_kind_rank(b->kind);
    if (ra != rb) return ra < rb;
    return a->index < b->index;
}

static void qs_sort(sf_qishuo_event *ev, int n)
{
    for (int i = 1; i < n; i++) {
        const sf_qishuo_event key = ev[i];
        int j = i - 1;
        while (j >= 0 && qs_before(&key, &ev[j])) { ev[j + 1] = ev[j]; j--; }
        ev[j + 1] = key;
    }
}

/* 解出来最多多少条 —— 用来做容量预检。每个角度最多两轮（民用年长于回归年/
 * 朔望月的边界情形），相位角每条流水线最多 14 个朔望月。 */
static int qs_upper_bound(const sf_qishuo_options *o)
{
    int n = 0;
    if (o->include_solar_terms) n += 24 * 2;
    if (o->include_pentads)     n += (o->include_solar_terms ? 48 : 72) * 2;
    n += o->n_phase_angles * SF_QS_PHASES_PER_ANGLE;
    return n;
}

/* ==================================================================
 * 主入口
 * ================================================================== */

int sf_qishuo_year_run(const sf_qishuo_options *o, int32_t civil_year,
                       sf_qishuo_event *events, int capacity,
                       sf_qishuo_year *out)
{
    const int rc = qs_validate(civil_year, o, capacity);
    if (rc != SF_QISHUO_OK) return rc;

    /* 窗口：当地 1 月 1 日 00:00 → 次年同日，先算当地 JD 再减偏移（见头注 1） */
    const double off = (double)o->utc_offset_minutes / 1440.0;
    const double start_ut1 =
        sf_utc_to_ut1(sf_julian_day_ut(civil_year, 1, 1, 0, 0, 0.0) - off);
    const double end_ut1 =
        sf_utc_to_ut1(sf_julian_day_ut(civil_year + 1, 1, 1, 0, 0, 0.0) - off);

    if (out) {
        memset(out, 0, sizeof *out);
        out->civil_year = civil_year;
        out->utc_offset_minutes = o->utc_offset_minutes;
        out->mode = o->mode;
        out->event_accuracy = o->event_accuracy;
        out->day_boundary_mode = o->day_boundary_mode;
        out->meridian_deg = o->meridian_deg;
        out->start_jd_ut1 = start_ut1;
        out->end_jd_ut1 = end_ut1;
    }
    if (!isfinite(start_ut1) || !isfinite(end_ut1) || !(start_ut1 < end_ut1))
        return SF_QISHUO_ERR_YEAR;

    /* 容量预检：解完再发现装不下只能丢掉一部分，而丢掉的是**发射顺序**
     * 而非时间顺序的子集 —— 这种「部分结果」在时间序上是错的，不应返回。
     * 所以先按上界要地方；events == NULL 则只当一次「问大小」的调用。 */
    const int need_max = qs_upper_bound(o);
    const int drop_p0 = o->include_solar_terms;
    if (!events || capacity == 0) {
        sf_qs_ctx probe = { o, &SF_QS_TIERS[o->event_accuracy], NULL, 0, 0, drop_p0 };
        if (o->include_solar_terms) qs_solar_stream(&probe, start_ut1, end_ut1, 24);
        if (o->include_pentads)     qs_solar_stream(&probe, start_ut1, end_ut1, 72);
        qs_lunar_stream(&probe, start_ut1, end_ut1);
        if (out) out->total = probe.total;
        return SF_QISHUO_OK;
    }
    if (capacity < need_max) {
        if (out) out->total = need_max;      /* 是上界，不是精确值 */
        return SF_QISHUO_ERR_CAPACITY;
    }

    sf_qs_ctx c = { o, &SF_QS_TIERS[o->event_accuracy], events, capacity, 0, drop_p0 };
    if (o->include_solar_terms) qs_solar_stream(&c, start_ut1, end_ut1, 24);
    if (o->include_pentads)     qs_solar_stream(&c, start_ut1, end_ut1, 72);
    qs_lunar_stream(&c, start_ut1, end_ut1);

    qs_sort(events, c.total);
    if (out) {
        out->count = c.total;
        out->total = c.total;
    }
    return SF_QISHUO_OK;
}
