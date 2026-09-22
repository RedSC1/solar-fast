/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_ganzhi.c —— 干支（四柱）
 *
 * 移植自 taiyin-ephemeris/src/chinese_calendar/ganzhi.cpp（445 行）与
 * ganzhi_rules.cpp（86 行）。规则部分逐函数对应，注释引用上游行号。
 *
 * ------------------------------------------------------------------
 * 陷阱：两套枚举名字相反
 *
 *   JS 的 RAT_HOUR_MODE        C++ 的 GanzhiRatHourMode
 *   NEXT_DAY      （默认）      NO_SPLIT        = 0（默认）
 *   CURRENT_DAY                 TODAY_GAN       = 1
 *   CURRENT_DAY_TOMORROW_STEM   TOMORROW_GAN    = 2
 *
 * **名字对不上，行为对得上**。这里用 C++ 的名字（移植源），值也一样，
 * 所以默认都是 0 —— 但不得按名字猜语义：
 *   NO_SPLIT      23:00 起整根柱子算**次日**的
 *   TODAY_GAN     日柱用当日、时干也跟当日
 *   TOMORROW_GAN  日柱用当日、时干跟次日
 *
 * ------------------------------------------------------------------
 * 澄清：不存在「立春 vs 正月初一」之争
 *
 * 年柱**永远**锚在立春。上游没有任何一条走「正月初一换年柱」的代码路径。
 * 该争议在本库中不存在。
 * ================================================================== */
#include <math.h>
#include <string.h>

#include "sf_calendar.h"

#define SF_CHINA_OFF   (480.0 / 1440.0)   /* 东经 120°，与 sf_calendar.c 同一常量 */
#define SF_JIA_ZI_YEAR 1984               /* 1984 = 甲子年 */
#define SF_J2000_JDN   2451545
/* 求解器的「同一个根」地板：精修两次得到的同一个节气可能差几百微秒。
 * 这不是民用时间的容差窗口。 */
#define SF_ROOT_EPS    1e-10

/* 上游 ganzhi.cpp:21 kNayinElementBySexagenaryIndex，按**六十甲子序号**索引
 * （不是按纳音编号）。五行取值见 SF_WUXING_*。 */
static const uint8_t kNayin[60] = {
    2, 2, 4, 4, 1, 1, 3, 3, 2, 2,
    4, 4, 0, 0, 3, 3, 2, 2, 1, 1,
    0, 0, 3, 3, 4, 4, 1, 1, 0, 0,
    2, 2, 4, 4, 1, 1, 3, 3, 2, 2,
    4, 4, 0, 0, 3, 3, 2, 2, 1, 1,
    0, 0, 3, 3, 4, 4, 1, 1, 0, 0,
};

const char *const SF_HEAVENLY_STEMS[10] = {
    "甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸"
};
const char *const SF_EARTHLY_BRANCHES[12] = {
    "子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥"
};

static int positive_mod(int v, int m)
{
    const int r = v % m;
    return r < 0 ? r + m : r;
}

static int valid_stem(uint8_t v)   { return v < 10u; }
static int valid_branch(uint8_t v) { return v < 12u; }
static uint8_t stem_of(sf_ganzhi v)   { return (uint8_t)(v >> 4); }
static uint8_t branch_of(sf_ganzhi v) { return (uint8_t)(v & 0x0fu); }

/* 天干地支的奇偶性必须相同，否则甲丑这种组合不是合法的干支 */
static int valid_ganzhi(sf_ganzhi v)
{
    return valid_stem(stem_of(v)) && valid_branch(branch_of(v))
        && ((stem_of(v) & 1u) == (branch_of(v) & 1u));
}

/* 六十甲子序号。6·干 − 5·支 是那个经典的线性反解。 */
static int index_of(sf_ganzhi v, int32_t *out_index)
{
    if (!out_index || !valid_ganzhi(v)) return -1;
    *out_index = positive_mod(6 * (int)stem_of(v) - 5 * (int)branch_of(v), 60);
    return 0;
}

/* ------------------------------------------------------------------
 * 规则（上游 ganzhi_rules.cpp）
 * ------------------------------------------------------------------ */
int sf_ganzhi_make(uint8_t stem_id, uint8_t branch_id, sf_ganzhi *out)
{
    if (!out || !valid_stem(stem_id) || !valid_branch(branch_id)) return -1;
    if ((stem_id & 1u) != (branch_id & 1u)) return -1;
    *out = (sf_ganzhi)((stem_id << 4) | branch_id);
    return 0;
}

int sf_ganzhi_advance(sf_ganzhi v, int32_t delta, sf_ganzhi *out)
{
    int32_t index = 0;
    if (!out || index_of(v, &index) != 0) return -1;
    const int32_t next = positive_mod(index + delta % 60, 60);
    /* 序号同时对 10 和 12 取模就得到新的干支，不必查表 */
    return sf_ganzhi_make((uint8_t)(next % 10), (uint8_t)(next % 12), out);
}

int sf_ganzhi_index(sf_ganzhi v, int32_t *out_index)
{
    return index_of(v, out_index);
}

/* 纳音名（三十个，两个甲子一组）。
 *
 * 注意：**上游没有这张表**：`taiyin-lite` 全域 grep「海中金」零命中，JS 侧只有
 * `getNayinId`（0..29 的编号）与 `getNayinElement`（五行）。所以这是
 * **非 JS 对齐面** —— 名字本身是本库补的，不是从上游抄的。
 *
 * 用**定宽 char[N][10] 而不是指针数组**：30 项纳音在 32 位 MCU 上是
 * 120 字节指针 + 300 字节串；定宽只要 300 字节，而且可以直接返回指针，
 * 调用方不用给缓冲。列宽 10 = 三个汉字（9 字节）+ NUL。
 *
 * 名字取**三命通会**那一套的通行写法（「沙中金」「覆灯火」「白蜡金」）。
 * 各家抄本有异体（砂/沙、覆/佛、蜡/腊），这里不承诺与哪一本逐字相同 ——
 * 但**末字必须与五行表对得上**，那条有断言守着（test/sf_bazi_check.c）。 */
static const char kNayinNames[30][10] = {
    "海中金", "炉中火", "大林木", "路旁土", "剑锋金",
    "山头火", "涧下水", "城头土", "白蜡金", "杨柳木",
    "泉中水", "屋上土", "霹雳火", "松柏木", "长流水",
    "沙中金", "山下火", "平地木", "壁上土", "金箔金",
    "覆灯火", "天河水", "大驿土", "钗钏金", "桑柘木",
    "大溪水", "沙中土", "天上火", "石榴木", "大海水",
};

const char *sf_ganzhi_nayin_name(uint8_t nayin_id)
{
    return nayin_id < 30u ? kNayinNames[nayin_id] : NULL;
}

/* 按**年号**取干支（甲子 = 1984）。年号是纯计数，没有任何天文量，
 * 所以这个函数不需要 JD、不需要 config。
 *
 * 注意：**这不是年柱。** 年柱锚**立春**，同一年的 1 月到立春前算上一年，
 * 要拿 JD 去定界（见 sf_ganzhi_four_pillars）；这里只是「第 N 年的干支」，
 * 也就是农历年 / 农历日旁边该配的那个年 —— 它跟**正月初一**换。
 *
 * 两者的差是**实际的**：2026 年立春 2/4、春节 2/17，所以 2/10 那天
 * 年柱已是丙午，而农历还在乙巳年腊月。把那天的年柱印在农历月日旁边，
 * 会显示为「丙午年 腊月廿三」—— 丙午年根本没有腊月。 */
int sf_ganzhi_year_of(int32_t year, sf_ganzhi *out)
{
    if (!out) return -1;
    const int32_t index = positive_mod(year - SF_JIA_ZI_YEAR, 60);
    return sf_ganzhi_make((uint8_t)(index % 10), (uint8_t)(index % 12), out);
}

/* 五虎遁：年干定月干。月序 0=寅 … 10=子, 11=丑 */
int sf_ganzhi_get_month(uint8_t year_stem_id, uint8_t month_index, sf_ganzhi *out)
{
    if (!valid_stem(year_stem_id) || month_index >= 12u) return -1;
    const uint8_t start = (uint8_t)(((year_stem_id % 5u) * 2u + 2u) % 10u);
    return sf_ganzhi_make((uint8_t)((start + month_index) % 10u),
                          (uint8_t)((month_index + 2u) % 12u), out);
}

/* 五鼠遁：日干定时干。时支 0=子 … 11=亥 */
int sf_ganzhi_get_hour(uint8_t day_stem_id, uint8_t hour_index, sf_ganzhi *out)
{
    if (!valid_stem(day_stem_id) || hour_index >= 12u) return -1;
    const uint8_t start = (uint8_t)((day_stem_id % 5u) * 2u);
    return sf_ganzhi_make((uint8_t)((start + hour_index) % 10u), hour_index, out);
}

int sf_ganzhi_nayin_element(sf_ganzhi v, uint8_t *out_element_id)
{
    int32_t index = 0;
    if (!out_element_id || index_of(v, &index) != 0) return -1;
    *out_element_id = kNayin[index];
    return 0;
}

int sf_ganzhi_nayin_id(sf_ganzhi v, uint8_t *out_nayin_id)
{
    int32_t index = 0;
    if (!out_nayin_id || index_of(v, &index) != 0) return -1;
    *out_nayin_id = (uint8_t)(index / 2);
    return 0;
}

/* 日柱。上游 ganzhi.cpp:325。
 *
 * 锚点：甲子日 ⇔ 日号 ≡ 2451551 (mod 60)。用**正午**的日号算，避开
 * 时分秒；这也意味着日柱在民用日的 00:00 换柱，与「几点算新的一天」
 * 无关 —— 那是早/晚子时的活，在 day_and_hour_pillars 里做。 */
int sf_ganzhi_day_pillar(const sf_cal_datetime *civil_date, sf_ganzhi *out)
{
    if (!civil_date || !out) return -1;

    const int64_t jdn = sf_solar_day_number(civil_date->year,
                                            civil_date->month, civil_date->day);
    const int64_t offset = jdn - SF_J2000_JDN;
    const int32_t index = positive_mod((int32_t)(offset % 60) - 6, 60);
    return sf_ganzhi_make((uint8_t)(index % 10), (uint8_t)(index % 12), out);
}

/* ------------------------------------------------------------------
 * 四柱
 * ------------------------------------------------------------------ */

/* 上游 ganzhi.cpp:36。默认 FOLLOW_CALENDAR = 跟着日历模式走。 */
static int use_historical_pillar_terms(const sf_cal_config *c)
{
    if (c->pillar_historical_mode == SF_PILLAR_HISTORICAL_ON)  return 1;
    if (c->pillar_historical_mode == SF_PILLAR_HISTORICAL_OFF) return 0;
    return c->mode == SF_CAL_CHINA_STANDARD_HISTORICAL;
}

/* 古历模式下，柱的节气边界取「归日的东经 120° 零点」，**不是**天文时刻。
 * 使用貌似精确的古代节气时刻没有意义 —— 表里存的本來就是日期。
 * 表里没有这个节气就返回 0，让调用方保留天文时刻。 */
static int historical_pillar_boundary(const sf_cal_config *c, double term_jd_ut,
                                      double *out_boundary)
{
    if (!use_historical_pillar_terms(c)) return 0;
    const int64_t day = sf_cal_historical_civil_day(SF_CAL_HIST_SOLAR_TERM, term_jd_ut);
    if (day < 0) return 0;
    /* 归日 D 的当地零点 = D − 0.5 − 480/1440（JD UT） */
    *out_boundary = (double)day - 0.5 - SF_CHINA_OFF;
    return 1;
}

static int year_pillar(const sf_cal_config *c, double instant_jd_ut,
                       const sf_cal_datetime *vt, sf_ganzhi *out)
{
    sf_cal_term_event lichun;
    /* 21 = 立春（从春分起算） */
    if (sf_cal_get_specific_term(c, vt->year, 21, &lichun) != 0) return -1;

    double boundary = lichun.jd_ut;
    (void)historical_pillar_boundary(c, lichun.jd_ut, &boundary);

    /* 立春之前算上一年。年柱**永远**锚立春。 */
    const int32_t pillar_year =
        vt->year + ((instant_jd_ut - boundary < -SF_ROOT_EPS) ? -1 : 0);
    /* 定完年号之后与 sf_ganzhi_year_of 同一条路 —— 两处共用一份实现，
     * 避免「年号取干支」这类基础换算在两处各写一遍而各自偏离。 */
    return sf_ganzhi_year_of(pillar_year, out);
}

static int month_pillar(const sf_cal_config *c, double instant_jd_ut,
                        sf_ganzhi year_p, sf_ganzhi *out)
{
    /* 古历模式按「归日」粒度判断，所以查询点往后挪一天：某个节气的精确
     * 时刻可能在今天稍晚，但它的柱月从今天 00:00 就开始了。 */
    double query = instant_jd_ut;
    if (use_historical_pillar_terms(c)) query += 1.0;

    sf_cal_term_event jie;
    if (sf_cal_find_term(c, query, 0, SF_TERM_JIE, &jie) != 0) return -1;

    double boundary = jie.jd_ut;
    const int historical = historical_pillar_boundary(c, jie.jd_ut, &boundary);

    /* 古历分支下按归日比，非古历分支下按瞬时比 —— 两条判据不一样，
     * 上游也是分开写的。 */
    const int future = historical
        ? (floor(boundary + SF_CHINA_OFF + 0.5) > floor(instant_jd_ut + SF_CHINA_OFF + 0.5))
        : (jie.jd_ut - instant_jd_ut > SF_ROOT_EPS);

    if (future) {
        /* 往回退 10 天再找一个。上游只退这一次。 */
        if (sf_cal_find_term(c, jie.jd_ut - 10.0, 0, SF_TERM_JIE, &jie) != 0)
            return -1;
        boundary = jie.jd_ut;
        (void)historical_pillar_boundary(c, jie.jd_ut, &boundary);
    }

    const uint8_t index = jie.index_from_winter_solstice;
    if ((index & 1u) == 0u) return -1;      /* 必须是「节」，不能是「中气」 */

    /* 上游的规则用 0=寅 … 11=丑。冬至序号 0 → 月序 11（子月）… 立春 3 → 0（寅月） */
    const uint8_t month_index = (uint8_t)(((index + 21u) / 2u) % 12u);
    return sf_ganzhi_get_month((uint8_t)(year_p >> 4), month_index, out);
}

static int day_and_hour_pillars(const sf_cal_datetime *vt, int32_t rat_hour_mode,
                                sf_ganzhi *out_day, sf_ganzhi *out_hour)
{
    if (rat_hour_mode < 0 || rat_hour_mode > 2) return -1;

    sf_cal_datetime anchor = *vt;
    const int late = vt->hour >= 23;

    if (late && rat_hour_mode == SF_GANZHI_RAT_HOUR_NO_SPLIT) {
        /* 整根柱子算次日的：把锚点挪到明天 */
        const int64_t jdn = sf_solar_day_number(vt->year, vt->month, vt->day);
        int32_t y = 0, mo = 0, d = 0;
        sf_solar_date_from_day_number(jdn + 1, &y, &mo, &d);
        anchor.year = y; anchor.month = mo; anchor.day = d;
        anchor.hour = 0; anchor.minute = 0; anchor.second = 0.0;
    }
    if (sf_ganzhi_day_pillar(&anchor, out_day) != 0) return -1;

    /* 时支恒为 ((hour+1)/2) % 12：23 点和 0 点都落到子（0），1 点到丑 */
    const uint8_t hour_branch = (uint8_t)(((vt->hour + 1) / 2) % 12);
    uint8_t stem = (uint8_t)(*out_day >> 4);

    if (late && rat_hour_mode == SF_GANZHI_RAT_HOUR_TOMORROW_GAN) {
        sf_ganzhi tomorrow;
        if (sf_ganzhi_advance(*out_day, 1, &tomorrow) != 0) return -1;
        stem = (uint8_t)(tomorrow >> 4);
    }
    return sf_ganzhi_get_hour(stem, hour_branch, out_hour);
}

/* 上游 ganzhi.cpp:221 normalize_chart_virtual_time。
 *
 * 它的处理很特殊但必要：一个 JD 来回转换之后，本该落在整点边界上的时刻
 * 可能变成 23:59:59.9999（或反过来），于是年/月/日柱整体串一天。
 * 这个函数只认**精确的二进制位模式**，不用任何容差窗口 —— 容差会让正常的
 * 23:59 也被吸到边界上去。
 *
 * 注意：偏离：上游查三种表示（拆分 JD、拆分回环、标量）。本库全程用标量
 * double JD，没有拆分表示，所以只认标量那一种。对使用本库 API 的调用方
 * 没有影响 —— 其 virtual_time 本来就是普通字段，不是拆 JD 变来的。 */
static int normalize_chart_virtual_time(const sf_cal_datetime *in,
                                        sf_cal_datetime *out)
{
    *out = *in;
    if (in->minute != 0 && in->minute != 59) return 0;

    const double source = sf_julian_day_ut(in->year, in->month, in->day,
                                           in->hour, in->minute, in->second);
    const double midnight = sf_julian_day_ut(in->year, in->month, in->day,
                                             0, 0, 0.0);
    const int64_t jdn = sf_solar_day_number(in->year, in->month, in->day);

    for (int hour = 0; hour <= 24; hour++) {
        sf_cal_datetime canonical;
        if (hour < 24) {
            canonical = (sf_cal_datetime){ in->year, in->month, in->day,
                                           hour, 0, 0.0 };
        } else {
            /* 第 24 个边界就是次日的 00:00 —— 正是「23:59:59.999 实际是
             * 明天零点」那种情况要落到的位置 */
            int32_t y = 0, mo = 0, d = 0;
            sf_solar_date_from_day_number(jdn + 1, &y, &mo, &d);
            canonical = (sf_cal_datetime){ y, mo, d, 0, 0, 0.0 };
        }
        const double boundary = midnight + (double)hour / 24.0;
        const double spelling = sf_julian_day_ut(canonical.year, canonical.month,
                                                 canonical.day, canonical.hour,
                                                 0, 0.0);
        if (source == boundary || source == spelling) {
            *out = canonical;
            return 0;
        }
    }
    return 0;
}

int sf_ganzhi_four_pillars(const sf_cal_config *c, double instant_jd_ut,
                           const sf_cal_datetime *virtual_time,
                           int32_t rat_hour_mode, sf_ganzhi_pillars *out)
{
    if (!c || !virtual_time || !out || !isfinite(instant_jd_ut)) return -1;

    sf_cal_datetime vt;
    if (normalize_chart_virtual_time(virtual_time, &vt) != 0) return -1;

    memset(out, 0, sizeof *out);
    if (year_pillar(c, instant_jd_ut, &vt, &out->year) != 0) return -1;
    if (month_pillar(c, instant_jd_ut, out->year, &out->month) != 0) return -1;
    if (day_and_hour_pillars(&vt, rat_hour_mode, &out->day, &out->hour) != 0)
        return -1;
    return 0;
}
