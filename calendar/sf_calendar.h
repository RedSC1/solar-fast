/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_calendar.h —— 农历（定朔定气法）、干支、古历归日
 *
 * 接口继承自
 *   taiyin-ephemeris/include/taiyin/c/chinese_calendar.h
 * 命名、枚举、结构字段沿用上游，只把前缀 taiyin_ → sf_，并去掉四个在源码级
 * 库里无意义的东西（见下）。
 * 实现移植自 taiyin-ephemeris/src/chinese_calendar/calendar.cpp，
 * 天文部分换成本库自己的 sf_solve_*。
 *
 * ------------------------------------------------------------------
 * 注意：这一层收的是 **JD(UT)**，不是 TT
 *
 * solar_fast.h 的立场是「全部按 JD(TT)，ΔT 由调用方处理」。日历不能这么办：
 * 农历的归日是**民用日**判定，本质上活在 UT 里。所以这一层自己带 ΔT 模型，
 * 收 UT、内部转 TT 交给 sf_solve_*。solar_fast.h 未作任何改动。
 *
 * ------------------------------------------------------------------
 * 与上游 C API 的四点偏离（均为被迫，不是重新设计）
 *
 *   上游                          这里              为什么
 *   taiyin_split_julian_date      普通 double       本库全库都是 double JD
 *   uint32_t struct_size          ——                源码级库，没有二进制 ABI
 *   Status / taiyin_call_result   0 / -1           沿用 sf_solve_* 的约定
 *   taiyin_ephemeris_diagnostic*  ——                同上
 * ================================================================== */
#ifndef SF_CALENDAR_H
#define SF_CALENDAR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 25 节气 / 15 朔 / 14 月 —— 不是 24/13/12。
 * 多出来的是为了把窗口闭合：节气要跑到下一个冬至，月要多一个才能算出
 * 最后一个月的天数。照抄上游，不得「优化」成 24/13/12。 */
#define SF_CAL_TERM_COUNT     25u
#define SF_CAL_NEW_MOON_COUNT 15u
#define SF_CAL_MONTH_COUNT    14u

/* ------------------------------------------------------------------
 * 配置
 * ------------------------------------------------------------------ */
enum {
    /* 默认。前 722 – 1960 年用古历归日表，1960 之后退化成天文计算。
     * 结构日界**锁定东经 120°**，与显示时区无关。 */
    SF_CAL_CHINA_STANDARD_HISTORICAL   = 0,
    /* 整个结构按观者的地方日界重建（月界、冬至日、闰月判定都会变） */
    SF_CAL_LOCAL_ASTRONOMICAL          = 1,
    /* 纯天文农历（现行官方口径），不带古历表 */
    SF_CAL_CHINA_STANDARD_ASTRONOMICAL = 2
};

enum {
    SF_CAL_FIXED_UTC_OFFSET   = 0,   /* 按 utc_offset_minutes */
    SF_CAL_MEAN_SOLAR_MERIDIAN = 1   /* 按 calendar_meridian_deg */
};

enum {
    SF_MONTH_NAME_NORMAL = 0,
    SF_MONTH_NAME_THIRTEEN = 1,      /* 十三月（春秋/战国岁末闰月） */
    SF_MONTH_NAME_LATER_NINE = 2,    /* 后九月（秦汉颛顼历岁末闰月） */
    SF_MONTH_NAME_ALT_TWELVE = 3,    /* 建丑→建寅回改处的十二月 */
    SF_MONTH_NAME_ALT_ONE = 4,       /* 武周：与建子岁首共存的普通正月 */
    SF_MONTH_NAME_LATER_SAME_NAME = 5 /* 同名后月（非闰） */
};

/* 四柱的节气边界取「天文时刻」还是「古历指定的民用日」。
 *
 * 注意：这**不是**「立春 vs 正月初一」那个岁首之争 —— 年柱永远锚在立春，
 * 上游两条代码路径都没有「正月初一换年柱」。不要被名字误导。 */
enum {
    SF_PILLAR_HISTORICAL_FOLLOW_CALENDAR = 0,   /* 默认：跟着日历模式走 */
    SF_PILLAR_HISTORICAL_OFF = 1,               /* 一律用天文时刻 */
    SF_PILLAR_HISTORICAL_ON  = 2                /* 一律用古历指定日 */
};

typedef struct {
    int32_t mode;
    int32_t day_boundary_mode;
    int32_t utc_offset_minutes;      /* 显示用；结构只在 LOCAL_ASTRONOMICAL 下跟它走 */
    int32_t pillar_historical_mode;
    double  calendar_meridian_deg;   /* 只在 MEAN_SOLAR_MERIDIAN 下有效 */
} sf_cal_config;

/* ------------------------------------------------------------------
 * 日期
 * ------------------------------------------------------------------ */
typedef struct { int32_t year; uint8_t month, day; } sf_solar_date;

typedef struct {
    int32_t year;
    uint8_t month;
    uint8_t day;
    uint8_t is_leap;
    uint8_t month_days;
    uint8_t month_name;
    /* 实际纪年，改历窗口里可能不等于 year —— 见 sf_calendar.c 的说明。
     * 不得把这两个合成一个。 */
    int32_t historical_year;
} sf_lunar_date;

typedef struct {
    uint8_t  index_from_winter_solstice;   /* 0 = 冬至 */
    double   target_longitude_rad;
    double   jd_ut;
    int64_t  civil_day_number;
} sf_cal_term_event;

typedef struct {
    double  jd_ut;
    int64_t civil_day_number;
} sf_cal_new_moon;

typedef struct {
    int32_t lunar_year;
    uint8_t month;
    uint8_t is_leap;
    uint8_t day_count;
    uint8_t month_name;
    uint8_t month_building_branch;         /* 月建，0 = 子 */
    int64_t first_civil_day_number;
    double  astronomical_new_moon_jd_ut;
    int32_t historical_year;
} sf_cal_month;

typedef struct {
    sf_cal_term_event solar_terms[SF_CAL_TERM_COUNT];
    sf_cal_new_moon   new_moons[SF_CAL_NEW_MOON_COUNT];
    sf_cal_month      months[SF_CAL_MONTH_COUNT];
    uint8_t solar_term_count;
    uint8_t new_moon_count;
    uint8_t month_count;
    int8_t  leap_month_index;              /* **-1 表示无闰** */
    int64_t first_winter_solstice_day_number;
    int64_t second_winter_solstice_day_number;
} sf_cal_year;

/* ------------------------------------------------------------------
 * 配置初始化
 * ------------------------------------------------------------------ */
void sf_cal_config_init(sf_cal_config *c);
void sf_cal_config_init_china_standard_historical(sf_cal_config *c,
                                                  int32_t local_utc_offset_minutes);
void sf_cal_config_init_china_standard_astronomical(sf_cal_config *c,
                                                    int32_t local_utc_offset_minutes);
void sf_cal_config_init_local_astronomical_utc_offset(sf_cal_config *c,
                                                      int32_t utc_offset_minutes);
void sf_cal_config_init_local_astronomical_meridian(sf_cal_config *c,
                                                    double longitude_deg);

/* ------------------------------------------------------------------
 * 主查询。全部返回 0 成功 / -1 失败（出参内容未定义）
 * ------------------------------------------------------------------ */

/* 一个「岁」的全部结构：25 气 + 15 朔 + 14 月。
 * jd_ut 落在哪个岁由它自己找 ∂ 至锚点决定。 */
int sf_cal_year_ut(const sf_cal_config *c, double jd_ut, sf_cal_year *out);

/* 公历日 → 农历日。
 * 日期换算只需要事件“归到哪一天”，内部先走低项数气朔；仅当估计时刻距
 * **历法结构日界** 30 分钟内才精算。CHINA 模式的结构日界固定东经 120°，
 * LOCAL 模式按配置的历法经度/结构偏移；不是显示用的当地时区。
 * sf_cal_year_ut 会返回天文事件时刻，因此不走这条近似路径。 */
int sf_cal_from_solar(const sf_cal_config *c, const sf_solar_date *s,
                      sf_lunar_date *out);
/* 瞬时 → 农历日。**这是唯一用到 utc_offset_minutes 的地方** */
int sf_cal_from_instant_ut(const sf_cal_config *c, double jd_ut,
                           sf_lunar_date *out);
/* 农历日 → 公历日。l->month_name 也要带上，否则同月同闰的两行无法区分 */
int sf_cal_from_lunar(const sf_cal_config *c, const sf_lunar_date *l,
                      sf_solar_date *out);
/* 某农历月有多少天 */
int sf_cal_month_days(const sf_cal_config *c, int32_t lunar_year,
                      uint8_t month, int is_leap, uint8_t *out_day_count);

/* ------------------------------------------------------------------
 * 古历归日表的直接查询（诊断 / 核对用）
 *
 * 上游 JS 导出了同名函数 historicalEventCivilDay，这里对齐。
 * estimate_jd_ut 是**天文估计**（表用它定位事件序号，不要求精确）；
 * 表不负责判断「调用方看的是哪一次事件」，只回答「那个序号归到哪一天」。
 * 超出表的范围（前 722 / 前 221 之前，或 1960 之后）返回 -1。
 * ------------------------------------------------------------------ */
enum { SF_CAL_HIST_NEW_MOON = 0, SF_CAL_HIST_SOLAR_TERM = 1 };

int64_t sf_cal_historical_civil_day(int kind, double estimate_jd_ut);

/* 民用日号 = floor(jdUT1 + day_offset + 0.5)。移植自 chinese-calendar.js 的
 * `civilDayNumber`。day_offset 是**日**不是分（时区偏移 /1440、经线 /360）。
 * day_offset 非有限时返回 -1（JS 那边抛 TypeError）。 */
int64_t sf_cal_civil_day_number(double jd_ut, double day_offset);

/* ------------------------------------------------------------------
 * 节气查询
 * ------------------------------------------------------------------ */
enum { SF_TERM_ANY = 0, SF_TERM_JIE = 1, SF_TERM_QI = 2 };

/* 找一个节气。next=1 往后、0 往前；filter 选「节」(奇数序号) / 「中气」。
 *
 * 注意：边界语义照抄上游：往前时**包含**正好落在 jd_ut 上的那个节气，
 * 往后时不含（要严格晚于）。四柱的月柱就靠这个。 */
int sf_cal_find_term(const sf_cal_config *c, double jd_ut, int next,
                     int filter, sf_cal_term_event *out);

/* 指定公历年的某个节气。
 * term_index_from_vernal_equinox：0=春分、18=冬至；19..23 是**同一年**的
 * 小寒…惊蛰。注意 18 是冬至、19 是下一个月的小寒，不是同一序列。
 * 立春 = 21。 */
int sf_cal_get_specific_term(const sf_cal_config *c, int32_t civil_year,
                             uint8_t term_index_from_vernal_equinox,
                             sf_cal_term_event *out);

/* ------------------------------------------------------------------
 * 干支 / 四柱
 *
 * 编码与上游一致：8 位打包 (天干<<4 | 地支)，天干 0=甲…9=癸，
 * 地支 0=子…11=亥。天干地支奇偶性必须相同，否则不是合法的干支。
 * ------------------------------------------------------------------ */
typedef uint8_t sf_ganzhi;

#define SF_GANZHI_INVALID       0xffu
#define SF_GANZHI_INVALID_NAYIN 0xffu

extern const char *const SF_HEAVENLY_STEMS[10];
extern const char *const SF_EARTHLY_BRANCHES[12];

/* 早晚子时怎么算。**取值范围和 JS 一致、名字和 C++ 一致**：
 *   NO_SPLIT   = 0（默认）  23:00 起算次日  ← JS 叫 NEXT_DAY
 *   TODAY_GAN  = 1          日柱用当日，时干也跟当日
 *   TOMORROW_GAN = 2        日柱用当日，时干跟次日
 * 注意：JS 的枚举顺序和这里不同，不得按名字对号入座，须按**行为**对应。 */
enum {
    SF_GANZHI_RAT_HOUR_NO_SPLIT     = 0,
    SF_GANZHI_RAT_HOUR_TODAY_GAN    = 1,
    SF_GANZHI_RAT_HOUR_TOMORROW_GAN = 2
};

enum {
    SF_WUXING_WATER = 0, SF_WUXING_WOOD = 1, SF_WUXING_METAL = 2,
    SF_WUXING_EARTH = 3, SF_WUXING_FIRE  = 4
};

/* 民用时刻。四柱的「虚拟时间」——闰秒、时区都不在这里管：**用户读数是
 * 多少就填多少**，怎么从它推出 instant_jd_ut 见上面「调用配方」。 */
typedef struct {
    int32_t year, month, day, hour, minute;
    double  second;
} sf_cal_datetime;

typedef struct {
    sf_ganzhi year, month, day, hour;
} sf_ganzhi_pillars;

int sf_ganzhi_make(uint8_t stem_id, uint8_t branch_id, sf_ganzhi *out);
int sf_ganzhi_advance(sf_ganzhi v, int32_t delta, sf_ganzhi *out);
int sf_ganzhi_index(sf_ganzhi v, int32_t *out_index);      /* 0..59 */

/* 按**年号**取干支（甲子 = 1984）。注意：**非 JS 对齐面**：上游有同一个公式
 * （`ganzhi.js` 的 `JIA_ZI_YEAR`），但它内联在年柱那条路里、不导出，所以
 * 同名函数在 JS 侧没有对应物。
 *
 * 注意：**这跟「年柱」不是一回事，不得互相替换**：
 *   年柱（sf_ganzhi_four_pillars 的 .year）锚**立春**，1 月到立春前算上一年；
 *   本函数是「第 N 年的干支」，跟**正月初一**换 —— 农历年 / 农历日旁边配的
 *   是这一个。2026 年立春 2/4、春节 2/17，中间那 13 天两者不同：年柱已是
 *   丙午，农历还在乙巳年腊月。配错会印出「丙午年 腊月廿三」这种不存在的日期。
 *
 * 年号取连续纪年（`sf_lunar_date.historical_year` 那个），不是被改历窗口
 * 保留下来的 `year` 标签。 */
int sf_ganzhi_year_of(int32_t year, sf_ganzhi *out);
int sf_ganzhi_get_month(uint8_t year_stem_id, uint8_t month_index, sf_ganzhi *out);
int sf_ganzhi_get_hour(uint8_t day_stem_id, uint8_t hour_index, sf_ganzhi *out);
/* 日柱。只吃民用日期，不需要天文 —— 但要**混合儒略/格里历**。 */
int sf_ganzhi_day_pillar(const sf_cal_datetime *civil_date, sf_ganzhi *out);
int sf_ganzhi_nayin_element(sf_ganzhi v, uint8_t *out_element_id);
int sf_ganzhi_nayin_id(sf_ganzhi v, uint8_t *out_nayin_id);

/* 纳音名（「海中金」那 30 个），按 `sf_ganzhi_nayin_id` 的编号索引。
 *
 * 注意：**非 JS 对齐面**：上游只有编号与五行，没有名字表（全域 grep 零命中），
 * 这三个汉字是本库补的。写法取三命通会那一套的通行本；异体（砂/沙、
 * 覆/佛、蜡/腊）各家不一，不承诺逐字。
 *
 * 越界（≥30）返回 NULL。返回的是 rodata 里的静态串，调用方不用给缓冲。 */
const char *sf_ganzhi_nayin_name(uint8_t nayin_id);

/* 四柱。virtual_time 给民用时刻，instant_jd_ut 给同一时刻的 UT 儒略日 ——
 * 年柱和月柱看的是**瞬时**落在哪个节气区间里，两者缺一不可。
 *
 * 实际上 instant_jd_ut 相对 virtual_time 是冗余的（可以从它算），但上游
 * 就是这么传的：调用方手上有 UT 瞬时，而 virtual_time 可能已经被时区或
 * 「早晚子时」调整过了。照抄。
 *
 * 注意：instant_jd_ut 是 **UT1**（本库统一口径）；virtual_time 按用户给的
 * 口径算，钟表时 / 真太阳时 / 地方平时都行。**两个参数各算各的，不得从一个
 * 推另一个** —— 见上面「钟表读数 → 四柱」的调用配方。 */
int sf_ganzhi_four_pillars(const sf_cal_config *c, double instant_jd_ut,
                           const sf_cal_datetime *virtual_time,
                           int32_t rat_hour_mode, sf_ganzhi_pillars *out);

/* ------------------------------------------------------------------
 * 时间尺度（本层自用，也对外）
 *
 * 三套尺度，不得混用：
 *     TT    力学时。节气 / 合朔的求解在这套上（solar_fast.h 那层全是 TT）
 *     UT1   地球自转角。恒星时、出没时刻用它
 *     UTC   民用时。**调用方手上通常就是这个** —— 手机时戳、日志、
 *           用户填的出生时间
 *
 * 三条换算链：
 *     TT  = UT1 + ΔT                    sf_ut_to_tt    / sf_tt_to_ut
 *     TT  = UTC + (TAI−UTC) + 32.184    sf_utc_to_tt   / sf_tt_to_utc
 *     UT1 = UTC + DUT1                  sf_utc_to_ut1  / sf_ut1_to_utc
 *
 * 注意：**本库其余所有 `jd_ut` 参数都是 UT1，不是 UTC。** 把 UTC 直接传入
 * 会差一个 DUT1（≤0.9 s）：恒星时 1 s ≈ 15″、出没时刻 1 s、朔/气时刻 1 s。
 * 手中是 UTC 时应先过 sf_utc_to_ut1。
 *
 * 注意：**1960-01-01 之前没有 UTC**，四个 UTC 系列函数一律退化成 UTC≈UT1
 * （那时民用时本来就是 UT1）。更早的标准时之前是 LMT，那不在本库范围内。
 *
 * ------------------------------------------------------------------
 * 「钟表读数 → 四柱」的调用配方
 *
 * **本库不做时区库。** 历史时区、夏令时、战时制编不出一份可信的表，而用户
 * 知道自己出生那年当地用的到底是什么时间 —— 所以这件事的输入由用户给，
 * 库只负责把给定口径算准。用户手上是钟表读数，走三步：
 *
 *   1. 钟表读数 (y,mo,d,h,mi,s) **本身就是** sf_cal_datetime 要的东西，
 *      直接填入，无需改动。
 *      注意：用户声明的那个偏移是**当时当地**的偏移（含夏令时），不是今天的。
 *   2. 同一组读数减掉偏移得到 UTC，再转 UT1：
 *          jd_utc = sf_julian_day_ut(y, mo, d, h, mi, s) - offset_min / 1440
 *          jd_ut1 = sf_utc_to_ut1(jd_utc)
 *   3. sf_ganzhi_four_pillars(&cfg, jd_ut1, &读数字段, rat_mode, &out);
 *
 * 注意：第 2 步**必须落到 UT1 再传入** —— 本库所有 jd_ut 参数都是 UT1，
 * 直接把 UTC 递进去会差一个 DUT1（见上）。
 *
 * 使用**真太阳时**时：第 1 步的字段换成真太阳时的日期 + 时刻，
 * 第 2 步的 jd_ut1 **原样不动**：
 *      jd_tst = jd_ut1 + lon_east/360 + sf_equation_of_time_minutes(jd_tt)/1440
 *             → 拆成 y/mo/d/h/mi 当虚拟时间
 * 年柱月柱锚的是**节气**，须用真瞬时；两个参数一起换成真太阳时会使其偏移。
 * 「地方平时」（不带均时差）同理，只是少最后一项。
 *
 * 注意：一个亚秒级的残留：本库判定**归日**用的是 `jd_ut + offset`，而真正的
 * 民用日是 `UTC + offset`，两者差一个 DUT1（≤0.9 s）。只在午夜前后 0.9 秒
 * 之内会改变归日，上游 JS 也是这个行为，照抄不动。
 * ------------------------------------------------------------------ */

/* ΔT = TT − UT1，秒。1953–2027 是 IERS C04 导出的年表（Catmull-Rom），
 * −720–1953 是 S15 三次样条，更早/更晚是长期抛物线 + future 公式。
 * 复刻 taiyin-lite `time.js` 的 deltaTSeconds，分支顺序也一致。 */
double sf_delta_t_seconds(double decimal_year);

/* UT1 ↔ TT */
double sf_ut_to_tt(double jd_ut);
double sf_tt_to_ut(double jd_tt);

/* TAI − UTC，秒。**1960-01-01 之前返回 NAN**。
 * 1972 前含橡皮秒（段内线性漂移），1972 起是整秒阶跃。
 * 表编制时有效到 2027-06-28，之后的闰秒要靠 Bulletin C 更新（见 sf_calendar.c）。 */
double sf_tai_minus_utc_seconds(double jd_utc);

/* UTC ↔ TT = UTC + (TAI−UTC) + 32.184。1960 前退化成 UTC≈UT1。
 * sf_tt_to_utc **表达不了 23:59:60 这个标签**（闰秒那一秒会落到前后两点之一）。 */
double sf_utc_to_tt(double jd_utc);
double sf_tt_to_utc(double jd_tt);

/* DUT1 = UT1 − UTC，秒。由 ΔT 的定义反推：
 *     UT1 − UTC = 32.184 + (TAI−UTC) − ΔT
 * 精度受 ΔT 年表采样限制（量级 0.05 s），**没有**跟 C04 逐日对过；
 * 要更高精度请自己接 IERS 的 finals2000A。1960 前返回 NAN。 */
double sf_dut1_seconds(double jd_utc);

/* UTC ↔ UT1。1960 前退化成 UTC≈UT1。 */
double sf_utc_to_ut1(double jd_utc);
double sf_ut1_to_utc(double jd_ut1);

/* 混合儒略/格里历（1582-10-15 切换）↔ JD。**必须用混合历**：
 * 古历数据一路到公元前 722 年，那全在儒略历区间，用纯格里历会整体偏几天。 */
double  sf_julian_day_ut(int32_t y, int32_t mo, int32_t d,
                         int32_t h, int32_t mi, double s);
int64_t sf_solar_day_number(int32_t y, int32_t mo, int32_t d);
/* JD（**任意时标**，函数不管时标）→ 带小数的公历钟面。
 * 移植自 time.js:317 calendarDateFromJulianDay，含 1582-10-15 的混合儒略/格里切换。
 * 要当地民用钟面就喂 jd_utc + offset/1440（同 JS 的 ZonedTime.fromJulianTime）。 */
void    sf_cal_datetime_from_jd(double jd, sf_cal_datetime *out);

void    sf_solar_date_from_day_number(int64_t jdn, int32_t *y,
                                      int32_t *mo, int32_t *d);

#ifdef __cplusplus
}
#endif
#endif /* SF_CALENDAR_H */
