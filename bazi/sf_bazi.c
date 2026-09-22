/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_bazi.c —— 八字的装配层与流运
 *
 * 移植自 taiyin-lite/packages/bazi/src/chart.ts 的 analyzePillars() 与
 * fortune.ts 的流运纯函数。逐函数对应，注释里引了上游行号。
 * ================================================================== */
#include <math.h>
#include <string.h>

#include "sf_bazi.h"

static uint8_t stem_of(sf_ganzhi v)   { return (uint8_t)(v >> 4); }
static uint8_t branch_of(sf_ganzhi v) { return (uint8_t)(v & 0x0fu); }

static int32_t pos_mod(int32_t v, int32_t m)
{
    const int32_t r = v % m;
    return r < 0 ? r + m : r;
}

/* 求解器「同一个根」的地板。同 sf_calendar.c 里的那一个语义：
 * 精修两次得到的同一个节气可能差几百微秒，这不是民用时间容差。 */
#define SF_BAZI_ROOT_EPS     1e-10
#define SF_BAZI_DAYS_JULIAN   365.25
#define SF_BAZI_DAYS_TROPICAL 365.2422

/* ------------------------------------------------------------------
 * 选项
 * ------------------------------------------------------------------ */
void sf_bazi_options_init(sf_bazi_options *o)
{
    if (!o) return;
    memset(o, 0, sizeof *o);
    sf_cal_config_init(&o->cal);
    o->earth_palace_mode    = SF_BAZI_EARTH_PALACE_FIRE_EARTH;
    /* 注意：全零 = FEMALE。这一行是整个 _init 存在的理由 —— 见头注。 */
    o->gender               = SF_BAZI_GENDER_NONE;
    /* 上游 ratHourMode 默认 'next-day' = 23:00 起整根柱算次日 = NO_SPLIT */
    o->rat_hour_mode        = SF_GANZHI_RAT_HOUR_NO_SPLIT;
    o->qiyun_time_model     = SF_BAZI_QIYUN_TRADITIONAL_CALENDAR;
    o->dayun_boundary_model = SF_BAZI_DAYUN_CIVIL_YEARS;
    o->dayun_count          = SF_BAZI_DEFAULT_DAYUN_COUNT;
}

/* ------------------------------------------------------------------
 * 装配：chart.ts:121 analyzePillars()
 *
 * 上游对每根柱做 unpackPillar()，再做五件事（chart.ts:136-140）：
 *   visibleTenGod  = getTenGod(dayMaster, stem)
 *   hiddenStems    = getHiddenStems(branch)
 *   hiddenTenGods  = 每个藏干再调一次 getTenGod
 *   lifeStage      = getLifeStage(dayMaster, branch, earthPalaceMode)
 *   nayinId        = getNayinId(value)
 * 顺序与「日主」那件事都在头注里说了：日柱算出来恒为 0（比肩）。
 * ------------------------------------------------------------------ */
int sf_bazi_analyze(const sf_ganzhi_pillars *raw, int32_t earth_palace_mode,
                    sf_bazi_chart *out)
{
    if (!raw || !out) return SF_BAZI_ERR_ARG;
    if (earth_palace_mode != SF_BAZI_EARTH_PALACE_FIRE_EARTH
        && earth_palace_mode != SF_BAZI_EARTH_PALACE_WATER_EARTH)
        return SF_BAZI_ERR_PALACE;

    const sf_ganzhi vals[SF_BAZI_PILLAR_COUNT] = {
        raw->year, raw->month, raw->day, raw->hour,
    };

    /* 先把四根柱都验一遍再动手 —— 上游是对每根调 ganzhiStem() 触发检查。
     * 这样失败时 out 一个字节都没被改。 */
    for (int i = 0; i < SF_BAZI_PILLAR_COUNT; i++) {
        int32_t idx = 0;
        if (sf_ganzhi_index(vals[i], &idx) != 0) return SF_BAZI_ERR_PILLAR;
    }

    memset(out, 0, sizeof *out);
    out->pillars.primary = *raw;
    if (sf_bazi_extra_pillars(raw, out->pillars.extra) != 0)
        return SF_BAZI_ERR_PILLAR;

    out->day_master        = stem_of(raw->day);
    out->earth_palace_mode = (uint8_t)earth_palace_mode;

    for (int i = 0; i < SF_BAZI_PILLAR_COUNT; i++) {
        sf_bazi_column *c = &out->columns[i];
        int32_t idx = 0;
        int n = 0;

        c->pillar = vals[i];
        c->stem   = stem_of(vals[i]);
        c->branch = branch_of(vals[i]);
        (void)sf_ganzhi_index(vals[i], &idx);      /* 上面已经验过，不会失败 */
        c->index  = (uint8_t)idx;

        if (sf_bazi_ten_god(out->day_master, c->stem, &c->visible_ten_god) != 0)
            return SF_BAZI_ERR_STEM;

        if (sf_bazi_hidden_stems(c->branch, c->hidden_stems, &n) != 0)
            return SF_BAZI_ERR_BRANCH;
        c->n_hidden = (uint8_t)n;
        /* 藏干十神只用 [0, n) 这几格；补位那几格留 0（memset 已经清了）。
         * 注意：不得把补位的 0 也当数据用 —— 甲(0) 是合法天干，二者看起来一样，
         * 只有 c->n_hidden 能区分。 */
        for (int j = 0; j < n; j++) {
            if (sf_bazi_ten_god(out->day_master, c->hidden_stems[j],
                                &c->hidden_ten_gods[j]) != 0)
                return SF_BAZI_ERR_STEM;
        }

        if (sf_bazi_life_stage(out->day_master, c->branch, earth_palace_mode,
                               &c->life_stage) != 0)
            return SF_BAZI_ERR_PALACE;

        (void)sf_ganzhi_nayin_id(vals[i], &c->nayin_id);
        (void)sf_ganzhi_nayin_element(vals[i], &c->nayin_element);
        (void)sf_bazi_stem_element(c->stem, &c->stem_element);
        (void)sf_bazi_branch_element(c->branch, &c->branch_element);
    }
    return SF_BAZI_OK;
}

/* ------------------------------------------------------------------
 * chart.ts:152 BaziChart.fromZonedTime()（只覆盖 clockMode = civil）
 * ------------------------------------------------------------------ */
int sf_bazi_chart_run(const sf_bazi_options *o, double birth_jd_ut1,
                      const sf_cal_datetime *birth_chart_time,
                      sf_bazi_chart *out)
{
    if (!o || !birth_chart_time || !out) return SF_BAZI_ERR_ARG;
    if (!isfinite(birth_jd_ut1)) return SF_BAZI_ERR_BIRTH_JD;

    sf_ganzhi_pillars p;
    /* 注意：年柱月柱走 instant、日柱时柱走 virtual_time —— 两个参数**必须分开**，
     * 理由见 sf_calendar.h「钟表读数 → 四柱」。 */
    if (sf_ganzhi_four_pillars(&o->cal, birth_jd_ut1, birth_chart_time,
                               o->rat_hour_mode, &p) != 0)
        return SF_BAZI_ERR_PILLAR;
    return sf_bazi_analyze(&p, o->earth_palace_mode, out);
}

/* ------------------------------------------------------------------
 * 流运（fortune.ts:38 起）
 * ------------------------------------------------------------------ */

/* fortune.ts:38 calculateFlowYear()
 *   index = positiveMod(civilYear - 4, 60)
 * 与 sf_ganzhi_year_of 恒等（那边的锚是 1984，1984−4 = 1980 = 33×60，
 * 同一格）—— 所以直接转发，不重抄一遍取模。 */
int sf_bazi_flow_year(int32_t civil_year, sf_ganzhi *out)
{
    if (!out) return SF_BAZI_ERR_ARG;
    return sf_ganzhi_year_of(civil_year, out) == 0 ? SF_BAZI_OK : SF_BAZI_ERR_ARG;
}

/* fortune.ts:44 calculateFlowMonth()
 *
 *   monthIndex = (monthBranch + 10) % 12
 *   getMonthGanzhi(yearStem, monthIndex)
 *
 * 月支 → 月序的转换在这一层完成；sf_ganzhi_get_month 接收月序
 * （0 = 寅），直接传入月支会使非寅月的结果不正确。 */
int sf_bazi_flow_month(sf_ganzhi year_pillar, uint8_t month_branch, sf_ganzhi *out)
{
    if (!out) return SF_BAZI_ERR_ARG;
    if (month_branch > 11) return SF_BAZI_ERR_PILLAR;
    const int month_index = ((int)month_branch + 10) % 12;
    return sf_ganzhi_get_month((uint8_t)((year_pillar >> 4) & 0x0f),
                               (uint8_t)month_index, out) == 0
        ? SF_BAZI_OK : SF_BAZI_ERR_PILLAR;
}

/* fortune.ts:52 calculateFlowDay() —— 转给 sf_ganzhi_day_pillar 的那个壳 */
int sf_bazi_flow_day(const sf_cal_datetime *civil_date, sf_ganzhi *out)
{
    if (!civil_date || !out) return SF_BAZI_ERR_ARG;
    return sf_ganzhi_day_pillar(civil_date, out) == 0 ? SF_BAZI_OK
                                                      : SF_BAZI_ERR_ARG;
}

/* fortune.ts:56 calculateFlowHour()
 *   getHourGanzhi(dayStem, hourIndex)，hourIndex 0..11（0 = 子时） */
int sf_bazi_flow_hour(sf_ganzhi day_pillar, uint8_t hour_index, sf_ganzhi *out)
{
    if (!out) return SF_BAZI_ERR_ARG;
    if (hour_index > 11) return SF_BAZI_ERR_PILLAR;
    return sf_ganzhi_get_hour((uint8_t)((day_pillar >> 4) & 0x0f),
                              hour_index, out) == 0
        ? SF_BAZI_OK : SF_BAZI_ERR_PILLAR;
}

/* fortune.ts:65 calculateXiaoYun() —— 基数 = **时柱** */
int sf_bazi_xiao_yun(sf_ganzhi hour_pillar, int32_t direction, int32_t age,
                     sf_ganzhi *out)
{
    if (!out) return SF_BAZI_ERR_ARG;
    if (direction != 1 && direction != -1) return SF_BAZI_ERR_DIRECTION;
    if (age < 1) return SF_BAZI_ERR_AGE;
    return sf_ganzhi_advance(hour_pillar, direction * age, out) == 0
        ? SF_BAZI_OK : SF_BAZI_ERR_PILLAR;
}

/* fortune.ts:97 generateDaYunPillars()
 *
 *   index           = offset + 1        （**从 1 起**，不是 0）
 *   startVirtualAge = first + offset*10
 *   endVirtualAge   = start + 9
 *   pillar          = advanceGanzhi(月柱, direction × index)
 *
 * 注意：`endVirtualAge = start + 9` 与「有天文那一层」的 endCivilTime（下一个
 * 十年边界）采用不同定义，不应合并。
 *
 * 注意：capacity 不足时**一条都不写**（与 sf_qishuo 的「写满 + 报总数」相反）。
 * 大运是固定小表，给半张比不给更糟。 */
int sf_bazi_da_yun_pillars(sf_ganzhi month_pillar, int32_t direction,
                           int32_t count, int32_t first_start_virtual_age,
                           sf_bazi_dayun_pillar *out, int capacity)
{
    if (!out) return SF_BAZI_ERR_ARG;
    if (direction != 1 && direction != -1) return SF_BAZI_ERR_DIRECTION;
    if (count < 0) return SF_BAZI_ERR_COUNT;
    if (count > capacity) return SF_BAZI_ERR_CAPACITY;

    for (int32_t off = 0; off < count; off++) {
        const int32_t index = off + 1;
        sf_ganzhi g;
        const int32_t start = first_start_virtual_age + off * 10;
        /* 先全部算进局部，最后一次性落盘 —— 中途失败就不留半张表 */
        if (sf_ganzhi_advance(month_pillar, direction * index, &g) != 0)
            return SF_BAZI_ERR_PILLAR;
        out[off].index             = index;
        out[off].pillar            = g;
        out[off].start_virtual_age = start;
        out[off].end_virtual_age   = start + 9;
    }
    return SF_BAZI_OK;
}

/* ------------------------------------------------------------------
 * 起运与大运（fortune.ts:147 起）
 * ------------------------------------------------------------------ */

/* fortune.ts:147 validateCivilTime()
 *
 * 注意：这是**回环验证**，不是「月份天数表」：把那一刻的正午拆回来，年月日
 * 必须还是同一天。两者在 **1582-10-05..14**（不存在的十天）行为不同 ——
 * 天数表会说 10 月 10 日合法，回环会说它拆不回来。不得图省事换掉。 */
static int valid_civil_time(const sf_cal_datetime *v)
{
    if (!v) return 0;
    if (v->month < 1 || v->month > 12) return 0;
    if (v->day   < 1 || v->day   > 31) return 0;
    if (v->hour  < 0 || v->hour  > 23) return 0;
    if (v->minute < 0 || v->minute > 59) return 0;
    if (!(v->second >= 0.0 && v->second < 60.0)) return 0;

    const double noon = sf_julian_day_ut(v->year, v->month, v->day, 12, 0, 0.0);
    sf_cal_datetime back;
    sf_cal_datetime_from_jd(noon, &back);
    return back.year == v->year && back.month == v->month && back.day == v->day;
}

/* fortune.ts:163 decomposeTraditionalOffset()
 *
 * 注意：核心是 `intervalDays × 120`，**不是** `/ 3`。一个实日对应虚拟年
 * （360 日）里的 120 日，所以 3 实日 = 1 虚拟年、1 实日 = 4 虚拟月。
 * 写成 `/3` 取年就丢了月与时分的分辨率。
 *
 * 注意：出参 `remaining_month_days` 是**未取整**的 `after_months`，
 * 它才是要加进日历的那个量 —— 用取整过的 days 会丢掉时/分/秒。 */
static void decompose_traditional(double interval_days,
                                  int32_t *years, int32_t *months, int32_t *days,
                                  int32_t *hours, int32_t *minutes, double *seconds,
                                  double *remaining_month_days)
{
    const double scaled = interval_days * 120.0;
    const int32_t y = (int32_t)floor(scaled / 360.0);
    const double after_years = scaled - (double)y * 360.0;
    const int32_t mo = (int32_t)floor(after_years / 30.0);
    const double after_months = after_years - (double)mo * 30.0;
    const int32_t d = (int32_t)floor(after_months);

    double secs = (after_months - (double)d) * 86400.0;
    const int32_t h = (int32_t)floor(secs / 3600.0);
    secs -= (double)h * 3600.0;
    const int32_t mi = (int32_t)floor(secs / 60.0);

    *years = y; *months = mo; *days = d; *hours = h; *minutes = mi;
    *seconds = secs - (double)mi * 60.0;
    *remaining_month_days = after_months;
}

/* fortune.ts:184 addCalendarComponents()
 *
 * 以下三项与上游 fortune.ts 的日历运算语义一致：
 *   1. 年/月按**民历分量**加，余数 remaining_days 按**实日**加；
 *   2. `base(当月 1 日) + origin.day − 1` **允许溢出到下个月** ——
 *      2/29 加一年会得到 3/1。**不得加月末夹取**；
 *   3. `elapsed_days` 是**名义 JD 之差**（两个 JD 都在 origin 的钟点上算），
 *      不等于结果 civil_time 自己的 JD。 */
static void add_calendar_components(const sf_cal_datetime *origin,
                                    int32_t years, int32_t months,
                                    double remaining_days,
                                    sf_cal_datetime *out, double *elapsed_days)
{
    const int32_t month_index = origin->month - 1 + months;
    const int32_t target_year = origin->year + years
                              + (int32_t)floor((double)month_index / 12.0);
    const int32_t target_month = pos_mod(month_index, 12) + 1;

    const double origin_jd = sf_julian_day_ut(origin->year, origin->month, origin->day,
                                              origin->hour, origin->minute, origin->second);
    const double base_jd = sf_julian_day_ut(target_year, target_month, 1,
                                            origin->hour, origin->minute, origin->second);
    const double result_jd = base_jd + (double)origin->day - 1.0 + remaining_days;

    sf_cal_datetime_from_jd(result_jd, out);
    if (elapsed_days) *elapsed_days = result_jd - origin_jd;
}

int sf_bazi_valid_civil_time(const sf_cal_datetime *v)
{
    return valid_civil_time(v);
}

/* fortune.ts:213 calculateQiYun() */
int sf_bazi_qi_yun(const sf_bazi_options *o, double birth_jd_ut1,
                   const sf_cal_datetime *birth_chart_time,
                   sf_ganzhi year_pillar, sf_bazi_qiyun *out)
{
    if (!o || !birth_chart_time || !out) return SF_BAZI_ERR_ARG;
    if (!isfinite(birth_jd_ut1)) return SF_BAZI_ERR_BIRTH_JD;
    if (!valid_civil_time(birth_chart_time)) return SF_BAZI_ERR_CHART_TIME;

    const int32_t tm = o->qiyun_time_model;
    if (tm != SF_BAZI_QIYUN_TRADITIONAL_CALENDAR
        && tm != SF_BAZI_QIYUN_JULIAN_YEAR
        && tm != SF_BAZI_QIYUN_TROPICAL_YEAR) return SF_BAZI_ERR_TIME_MODEL;

    /* 性别**先单独判**，好把「没给 / 非法性别」与「年柱不合法」两条失败
     * 分开报。上游对前者的措辞是 'unknown gender'（未给与非法是同一类）。
     * 判完之后 sf_bazi_luck_direction 只可能因年柱失败。 */
    if (o->gender != SF_BAZI_GENDER_FEMALE && o->gender != SF_BAZI_GENDER_MALE)
        return SF_BAZI_ERR_GENDER;

    int32_t direction = 0;
    if (sf_bazi_luck_direction(year_pillar, o->gender, &direction) != 0)
        return SF_BAZI_ERR_PILLAR;

    /* 参考节：默认取**上一个**节；间隔为 0（出生正好落在节上）时**不换**；
     * 顺行时才改取**下一个**节。这是 `else if` 结构 —— 拆成两个独立 if 就会
     * 在「正好落在节上 + 顺行」时去取 next，得到一整个月的间隔。 */
    sf_cal_term_event jie;
    if (sf_cal_find_term(&o->cal, birth_jd_ut1, 0, SF_TERM_JIE, &jie) != 0)
        return SF_BAZI_ERR_TERM;
    double interval = birth_jd_ut1 - jie.jd_ut;
    if (fabs(interval) <= SF_BAZI_ROOT_EPS) {
        interval = 0.0;
    } else if (direction > 0) {
        if (sf_cal_find_term(&o->cal, birth_jd_ut1, 1, SF_TERM_JIE, &jie) != 0)
            return SF_BAZI_ERR_TERM;
        interval = jie.jd_ut - birth_jd_ut1;
    }
    if (!isfinite(interval) || interval < -SF_BAZI_ROOT_EPS)
        return SF_BAZI_ERR_JIE_INTERVAL;
    if (interval < 0.0) interval = 0.0;

    int32_t ty = 0, tmo = 0, td = 0, th = 0, tmi = 0;
    double ts = 0.0, remaining_month_days = 0.0;
    decompose_traditional(interval, &ty, &tmo, &td, &th, &tmi, &ts,
                          &remaining_month_days);

    out->direction        = direction;
    out->time_model       = tm;
    out->reference_jie    = jie;
    out->jie_interval_days = interval;
    out->start_age_years  = interval / 3.0;
    out->trad_years = ty;  out->trad_months = tmo;
    out->trad_days  = td;  out->trad_hours  = th;
    out->trad_minutes = tmi; out->trad_seconds = ts;

    if (tm == SF_BAZI_QIYUN_TRADITIONAL_CALENDAR) {
        double elapsed = 0.0;
        add_calendar_components(birth_chart_time, ty, tmo, remaining_month_days,
                                &out->start_civil_time, &elapsed);
        /* 注意：这里是 birth_jd_ut1 + elapsed，**不是** start_civil_time 的 JD */
        out->start_jd_ut1 = birth_jd_ut1 + elapsed;
    } else {
        const double year_days = (tm == SF_BAZI_QIYUN_JULIAN_YEAR)
            ? SF_BAZI_DAYS_JULIAN : SF_BAZI_DAYS_TROPICAL;
        const double elapsed = interval * year_days / 3.0;
        const double origin_jd = sf_julian_day_ut(birth_chart_time->year, birth_chart_time->month,
                                                  birth_chart_time->day, birth_chart_time->hour,
                                                  birth_chart_time->minute, birth_chart_time->second);
        out->start_jd_ut1 = birth_jd_ut1 + elapsed;
        sf_cal_datetime_from_jd(origin_jd + elapsed, &out->start_civil_time);
    }
    return SF_BAZI_OK;
}

/* fortune.ts:284 generateDaYun()
 *
 * 第 offset 步：startYears = offset×10、endYears = (offset+1)×10，
 * 两种边界模型：
 *   CIVIL_YEARS  从起运的**民用时刻**逐年加（步长是整数民历年，闰年会
 *                给出 3652/3653 这种不等长的十年）—— 默认
 *   JULIAN/TROP  每步 offset×10×365.25 或 ×365.2422 天
 *
 * 注意：年长常数是 **365.2422**（不是 .2425），且 `JULIAN ? 365.25 : 365.2422`
 * 这个三元表达式在起运那边也有一份 —— 两处必须一致。
 */
int sf_bazi_da_yun(const sf_bazi_options *o,
                   const sf_cal_datetime *birth_chart_time,
                   sf_ganzhi month_pillar, const sf_bazi_qiyun *qi_yun,
                   sf_bazi_dayun_entry *out, int capacity)
{
    if (!o || !birth_chart_time || !qi_yun || !out) return SF_BAZI_ERR_ARG;
    if (!valid_civil_time(birth_chart_time)) return SF_BAZI_ERR_CHART_TIME;

    const int32_t count = o->dayun_count;
    const int32_t bm = o->dayun_boundary_model;
    if (count < 0) return SF_BAZI_ERR_COUNT;
    if (bm != SF_BAZI_DAYUN_CIVIL_YEARS && bm != SF_BAZI_DAYUN_JULIAN_YEARS
        && bm != SF_BAZI_DAYUN_TROPICAL_YEARS) return SF_BAZI_ERR_BOUNDARY;
    if (count > capacity) return SF_BAZI_ERR_CAPACITY;

    const double cont_days = (bm == SF_BAZI_DAYUN_JULIAN_YEARS)
        ? SF_BAZI_DAYS_JULIAN : SF_BAZI_DAYS_TROPICAL;

    for (int32_t off = 0; off < count; off++) {
        const int32_t index = off + 1;
        const int32_t start_years = off * 10;
        const int32_t end_years = index * 10;
        sf_cal_datetime sc, ec;
        double start_jd = 0.0, end_jd = 0.0;

        if (bm == SF_BAZI_DAYUN_CIVIL_YEARS) {
            double se = 0.0, ee = 0.0;
            add_calendar_components(&qi_yun->start_civil_time, start_years, 0, 0.0, &sc, &se);
            add_calendar_components(&qi_yun->start_civil_time, end_years, 0, 0.0, &ec, &ee);
            start_jd = qi_yun->start_jd_ut1 + se;
            end_jd   = qi_yun->start_jd_ut1 + ee;
        } else {
            const double sd = (double)start_years * cont_days;
            const double ed = (double)end_years * cont_days;
            const sf_cal_datetime *o2 = &qi_yun->start_civil_time;
            const double origin_jd = sf_julian_day_ut(o2->year, o2->month, o2->day,
                                                      o2->hour, o2->minute, o2->second);
            start_jd = qi_yun->start_jd_ut1 + sd;
            end_jd   = qi_yun->start_jd_ut1 + ed;
            sf_cal_datetime_from_jd(origin_jd + sd, &sc);
            sf_cal_datetime_from_jd(origin_jd + ed, &ec);
        }

        sf_ganzhi pillar;
        if (sf_ganzhi_advance(month_pillar, qi_yun->direction * index, &pillar) != 0)
            return SF_BAZI_ERR_PILLAR;

        /* 注意：虚岁 = 起始民历年 − 出生民历年 + 1。基数是**钟表年份**，
         * 不是 birth_jd_ut1，更不是 start_age_years。 */
        const int32_t start_age = sc.year - birth_chart_time->year + 1;

        out[off].index             = index;
        out[off].pillar            = pillar;
        out[off].start_virtual_age = start_age;
        out[off].end_virtual_age   = start_age + 9;
        out[off].start_jd_ut1      = start_jd;
        out[off].end_jd_ut1        = end_jd;
        out[off].start_civil_time  = sc;
        out[off].end_civil_time    = ec;
    }
    return SF_BAZI_OK;
}
