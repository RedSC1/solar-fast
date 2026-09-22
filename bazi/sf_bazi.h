/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_bazi.h —— 八字的**装配层**：已知四柱 → 一列解读；流年 / 小运 / 大运柱
 *
 * 移植自 taiyin-lite/packages/bazi/src/chart.ts 的 analyzePillars() 与
 * fortune.ts 的流运那几个纯函数。纯规则那些在 sf_bazi_rules.h。
 *
 * 注意：这一层会碰天文（sf_bazi_chart_run 要解四柱），所以它链接求解器；
 * 只测纯规则时不得引入这个头 —— 那是 sf_bazi_rules 存在的理由。
 *
 * ------------------------------------------------------------------
 * 上游的 API 形状与这里的对应
 *
 *   analyzePillars({year,month,day,hour}, {earthPalaceMode})
 *       → sf_bazi_analyze()
 *   BaziChart.fromZonedTime(zoned, options)
 *       → sf_bazi_chart_run()   （注意：只覆盖 clockMode = civil，见下）
 *   calculateFlowYear / calculateXiaoYun / generateDaYunPillars
 *       → sf_bazi_flow_year / sf_bazi_xiao_yun / sf_bazi_da_yun_pillars
 *
 * 注意：钟表口径不在这层做：上游的 `clockMode`（平太阳时 / 真太阳时）在
 * 本库是**调用方**的事 —— 照 sf_calendar.h「钟表读数 → 四柱」那份配方，
 * 由调用方解析出 birth_chart_time，再把 instant 传进来。所以本模块
 * 对应的是上游 `clockMode = 'civil'` 那一条路径，**对拍也只覆盖这一条**。
 * （sf_rise.h 里有均时差/真太阳时那几个函数，但这里**不调** —— 保持
 * 「口径由调用方定」这条边界。）
 * ================================================================== */
#ifndef SF_BAZI_H
#define SF_BAZI_H

#include <stdint.h>

#include "sf_bazi_rules.h"
#include "sf_calendar.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 起运时刻模型。值与上游 QIYUN_TIME_MODEL 相同。 */
enum {
    SF_BAZI_QIYUN_TRADITIONAL_CALENDAR = 0,   /* 默认：三日折一年 + 民历加法 */
    SF_BAZI_QIYUN_JULIAN_YEAR          = 1,
    SF_BAZI_QIYUN_TROPICAL_YEAR        = 2
};

/* 大运边界模型。值与上游 DAYUN_BOUNDARY_MODEL 相同。 */
enum {
    SF_BAZI_DAYUN_CIVIL_YEARS    = 0,   /* 默认：每步 10 个**民历年**（步长不等长） */
    SF_BAZI_DAYUN_JULIAN_YEARS   = 1,   /* 每步 10 × 365.25 天 */
    SF_BAZI_DAYUN_TROPICAL_YEARS = 2    /* 每步 10 × 365.2422 天 */
};

/* 默认大运步数（上游 daYunCount 的默认值也是 8）。 */
#define SF_BAZI_DEFAULT_DAYUN_COUNT 8

/* 负返回码。上游在这些地方抛 TypeError / RangeError，本库按惯例换负码。 */
typedef enum {
    SF_BAZI_OK               = 0,
    SF_BAZI_ERR_STEM         = -1,   /* 天干不在 0..9 */
    SF_BAZI_ERR_BRANCH       = -2,   /* 地支不在 0..11 */
    SF_BAZI_ERR_PILLAR       = -3,   /* 不是合法干支（奇偶不符） */
    SF_BAZI_ERR_PALACE       = -4,   /* earth_palace_mode 不是两个之一 */
    SF_BAZI_ERR_GENDER       = -5,   /* 起运没给性别（上游抛 'Qi-Yun requires options.gender'） */
    SF_BAZI_ERR_TIME_MODEL   = -6,
    SF_BAZI_ERR_BOUNDARY     = -7,
    SF_BAZI_ERR_COUNT        = -8,   /* count 不是非负整数 */
    SF_BAZI_ERR_AGE          = -9,   /* 小运：age < 1 */
    SF_BAZI_ERR_DIRECTION    = -10,  /* direction 不是 ±1 */
    SF_BAZI_ERR_CHART_TIME   = -11,  /* 出生时刻回环校验失败（上游 validateCivilTime） */
    SF_BAZI_ERR_BIRTH_JD     = -12,  /* birth_jd_ut1 非有限 */
    SF_BAZI_ERR_TERM         = -13,  /* 节气搜索失败（getPreviousJie / getNextJie） */
    SF_BAZI_ERR_JIE_INTERVAL = -14,  /* 节间隔为 NaN 或 < -1e-10 */
    SF_BAZI_ERR_CAPACITY     = -15,  /* 出参缓冲装不下 —— **一条都不写** */
    SF_BAZI_ERR_ARG          = -16   /* 空指针 / 其它入参问题 */
} sf_bazi_status;

/* ------------------------------------------------------------------
 * 一柱的规则解读。对应上游 BaziColumn。
 *
 * 注意：名字类字段不落结构（上游的 stemName / branchName / name /
 * visibleTenGodName / hiddenTenGodNames / lifeStageName / key 全是派生串）。
 * 落进去会把结构从 17 字节撑到 200 多并引入 12 个指针 —— 嵌入式上不可接受，
 * 而且这正是 sf_qishuo 把 `event.name` 做成函数的同一个理由。
 * 要名字就现取：sf_bazi_ten_god_name() / sf_bazi_life_stage_name()。
 * ------------------------------------------------------------------ */
typedef struct {
    sf_ganzhi pillar;                  /* 打包：(干<<4)|支 */
    uint8_t   stem;                    /* 0..9  */
    uint8_t   branch;                  /* 0..11 */
    uint8_t   index;                   /* 六十甲子序号 0..59 */
    uint8_t   visible_ten_god;         /* 对日干。注意：日柱恒为 0（比肩），不是「日主」 */
    uint8_t   n_hidden;                /* 藏干个数 1..3 */
    uint8_t   hidden_stems[SF_BAZI_HIDDEN_STEM_CAPACITY];
    uint8_t   hidden_ten_gods[SF_BAZI_HIDDEN_STEM_CAPACITY];
    uint8_t   life_stage;              /* 十二长生 */
    uint8_t   nayin_id;                /* 0..29 */
    uint8_t   nayin_element;           /* 纳音的五行 —— 注意：不是干支本气五行 */
    uint8_t   stem_element;            /* 天干本气五行（非 JS 对齐面，派生） */
    uint8_t   branch_element;          /* 地支本气五行（非 JS 对齐面，派生） */
} sf_bazi_column;

/* 四柱 + 外四柱。extra 顺序 = 命宫 / 身宫 / 胎元 / 胎息。 */
typedef struct {
    sf_ganzhi_pillars primary;
    sf_ganzhi         extra[SF_BAZI_EXTRA_PILLAR_COUNT];
} sf_bazi_pillars;

/* 一张排盘（不含出生时刻 —— 那个在 options 之外单独传）。
 * 78 字节，**可以放栈上**。 */
typedef struct {
    sf_bazi_pillars pillars;
    sf_bazi_column  columns[SF_BAZI_PILLAR_COUNT];
    uint8_t         day_master;          /* 日干 0..9 */
    uint8_t         earth_palace_mode;   /* 回显 */
} sf_bazi_chart;

/* 选项。日历那部分**原样嵌入 sf_cal_config**，不重新摊平 —— 与 sim 的
 * year_fill 投影同一手法，而且 sf_bazi_chart_run 要把它透传给
 * sf_ganzhi_four_pillars。 */
typedef struct {
    sf_cal_config cal;
    int32_t earth_palace_mode;      /* 默认 FIRE_EARTH */
    int32_t gender;                 /* 默认 SF_BAZI_GENDER_NONE(-1)，同上游的 undefined */
    int32_t rat_hour_mode;          /* 透传给 sf_ganzhi_four_pillars */
    int32_t qiyun_time_model;       /* 默认 TRADITIONAL_CALENDAR */
    int32_t dayun_boundary_model;   /* 默认 CIVIL_YEARS */
    int32_t dayun_count;            /* 默认 8 */
} sf_bazi_options;

/* 使用此初始化函数设置默认值。全零初始化会得到 gender = 0（女），
 * 无法表示性别未指定；sf_qishuo_options 的 meridian_deg 同样需要显式默认值。 */
void sf_bazi_options_init(sf_bazi_options *o);

/* ------------------------------------------------------------------
 * 装配
 * ------------------------------------------------------------------ */

/* 出生读数是不是一个**真实存在**的时刻：返回 1 是、0 不是。
 *
 * 注意：判据是**回环**（把那一刻的正午拆回来，年月日还得是同一天），不是
 * 「月份天数表」—— 两者在 1582-10-05..14 那十天行为不同（那十天不存在）。
 *
 * 注意：非 JS 对齐面：上游有同名逻辑（fortune.ts 的 validateCivilTime），
 * 但它是私有的、失败时抛异常，没有导出。这一条是本库为了让调用方能在
 * 「用户填了 2 月 30 日」时给出提示而开的（sim 的出生信息页就用它）。 */
int sf_bazi_valid_civil_time(const sf_cal_datetime *v);

/* 只解读**已知四柱**，不涉及出生瞬间。对应上游 analyzePillars()。 */
int sf_bazi_analyze(const sf_ganzhi_pillars *raw, int32_t earth_palace_mode,
                    sf_bazi_chart *out);

/* 一次算全：四柱 + 外四柱 + 四列规则字段。**不含起运**（那个要节气）。
 *
 * birth_jd_ut1 是**同一时刻的 UT1 瞬时**（不是 UTC，差一个 DUT1）；
 * birth_chart_time 是用户填的钟表读数。两者必须分开给 —— 年柱月柱锚节气、
 * 只认物理时刻，日柱时柱认虚拟时刻。完整配方见 sf_calendar.h。 */
int sf_bazi_chart_run(const sf_bazi_options *o, double birth_jd_ut1,
                      const sf_cal_datetime *birth_chart_time,
                      sf_bazi_chart *out);

/* ------------------------------------------------------------------
 * 流运（纯整数，无天文）
 * ------------------------------------------------------------------ */

/* 流年：**按年号取干支**（甲子 = 1984）。与 sf_ganzhi_year_of 恒等 ——
 * 上游的 calculateFlowYear 就是 positiveMod(civilYear - 4, 60)，而
 * 1984 − 4 = 1980 = 33 × 60，两个锚点在同一格上。
 *
 * 注意：这是**年号的干支**，不是年柱：年柱锚立春，见 sf_ganzhi_year_of 的注。 */
int sf_bazi_flow_year(int32_t civil_year, sf_ganzhi *out);

/* 流月：**五虎遁**。对应上游 calculateFlowMonth()。
 *
 *   monthIndex = (monthBranch + 10) % 12            ← 上游 fortune.ts:48 先做这一步
 *   getMonthGanzhi(流年柱的天干, monthIndex)
 *
 * 注意：这一转换必须在这层做：`month_branch` 收的是月支（0=子…11=亥，寅=2），
 * 而 sf_ganzhi_get_month 收的是**月序**（0=寅）。直接传入月支会使
 * 非寅月的结果不正确。
 *
 * 注意：吃的是**流年柱**（sf_bazi_flow_year 的输出），不是年柱 —— 年柱锚立春、
 * 流年按年号，两者在立春到春节之间差一年。 */
int sf_bazi_flow_month(sf_ganzhi year_pillar, uint8_t month_branch,
                       sf_ganzhi *out);

/* 流日：就是 sf_ganzhi_day_pillar 的转发（锚点逐字核过：两边都是
 * 「日号 − J2000 − 6，模 60」）。留这个壳是为了跟上游 fortune.ts 的导出
 * 一一对应，对拍时也按同一个名字配对。
 *
 * 注意：只吃**年月日**，时分秒忽略。日期必须真实存在 —— 本函数不校验
 * （要校验用 sf_bazi_valid_civil_time）。 */
int sf_bazi_flow_day(const sf_cal_datetime *civil_date, sf_ganzhi *out);

/* 流时：**五鼠遁**。对应上游 calculateFlowHour()。
 *   getHourGanzhi(流日柱的天干, hourIndex)，hourIndex 0..11，0 = 子时。
 * 注意：吃的是**流日柱**，不是本命日柱。 */
int sf_bazi_flow_hour(sf_ganzhi day_pillar, uint8_t hour_index,
                      sf_ganzhi *out);

/* 小运：基数 = **时柱**，走 direction × age 步。age ≥ 1。
 *
 * 注意：收的是**裸的那一根柱**（时柱），不是整张盘 —— 上游是从 chart 里读
 * `.pillars.hour`，C 这边让调用方自己取出来更省事，也免得为了调一次
 * 去凑一个 sf_bazi_pillars。下面的大运柱同理（收月柱）。 */
int sf_bazi_xiao_yun(sf_ganzhi hour_pillar, int32_t direction, int32_t age,
                     sf_ganzhi *out);

/* 大运柱（纯规则，不含时刻）。对应上游 generateDaYunPillars()。
 *
 * 第 offset 步（0-based）：index = offset+1，start_virtual_age =
 * first_start_virtual_age + offset*10，end = start + 9，
 * 柱 = 月柱走 direction × index 步。
 *
 * 注意：end_virtual_age 恒 = start + 9，而**有天文的那一层**（A3 的
 * sf_bazi_da_yun）里 endCivilTime 是下一个十年边界 —— 两者**故意不同步**。 */
typedef struct {
    int32_t  index;                  /* 1..count */
    sf_ganzhi pillar;
    int32_t  start_virtual_age;
    int32_t  end_virtual_age;
} sf_bazi_dayun_pillar;

/* capacity 不足时返回 SF_BAZI_ERR_CAPACITY 且**一条都不写** —— 大运是固定
 * 小表，宁可整片空白也不给半张。 */
int sf_bazi_da_yun_pillars(sf_ganzhi month_pillar, int32_t direction,
                           int32_t count, int32_t first_start_virtual_age,
                           sf_bazi_dayun_pillar *out, int capacity);

/* ------------------------------------------------------------------
 * 起运与大运（要天文）
 * ------------------------------------------------------------------ */

/* 起运。「三日折一年」的结果拆成 年/月/日/时/分/秒 六个分量。
 *
 * 与上游 fortune.ts 对齐时需保留以下三项语义：
 *   1. 折算是 `intervalDays × 120` 然后在 360 日年上取整，**不是**
 *      `intervalDays / 3` —— 那样会丢掉月与时分的分辨率。
 *   2. 加进日历的是**未取整的余数** `remaining_month_days`，不是
 *      `offset.days`；用取整过的那个会丢掉时/分/秒。
 *   3. `start_jd_ut1 = birth_jd + elapsed_days`，而 elapsed_days 是
 *      **名义 JD 之差**（在出生时刻的钟点上算的），**不等于**
 *      start_civil_time 自己的 JD。两者传统模型下本来就不同步，不得对齐。 */
typedef struct {
    int32_t direction;                 /* ±1 */
    int32_t time_model;                /* 回显 */
    sf_cal_term_event reference_jie;   /* 参考的那个「节」 */
    double  jie_interval_days;
    double  start_age_years;           /* = jie_interval_days / 3（三模型同值） */
    int32_t trad_years, trad_months, trad_days, trad_hours, trad_minutes;
    double  trad_seconds;              /* 分解的零头 */
    double  start_jd_ut1;
    sf_cal_datetime start_civil_time;
} sf_bazi_qiyun;

/* 起运。
 *
 * 注意：顺逆吃的是**年柱**（锚立春），不是年号 —— 见 sf_bazi_luck_direction。
 * 注意：`o->gender` **必须给**（不能是 SF_BAZI_GENDER_NONE），否则回
 * SF_BAZI_ERR_GENDER。上游在这里抛 'Qi-Yun requires options.gender'。
 * 注意：出生正好落在「节」上时，**两个方向都停在 previous jie、间隔 0**
 * （上游是 `else if`，不是两个独立 if）。 */
int sf_bazi_qi_yun(const sf_bazi_options *o, double birth_jd_ut1,
                   const sf_cal_datetime *birth_chart_time,
                   sf_ganzhi year_pillar, sf_bazi_qiyun *out);

/* 一步大运。8 步约 96 B × 8 = 768 B。 */
typedef struct {
    int32_t index;                     /* 1..count */
    sf_ganzhi pillar;
    int32_t start_virtual_age, end_virtual_age;
    double  start_jd_ut1, end_jd_ut1;
    sf_cal_datetime start_civil_time, end_civil_time;
} sf_bazi_dayun_entry;

/* 大运表。count 与边界模型取 `o->dayun_count` / `o->dayun_boundary_model`。
 *
 * 注意：`start_virtual_age = start_civil_time.year − birth_chart_time.year + 1`
 * —— 是**虚岁**，而且基数是**虚拟钟表**的年份，不是 birth_jd 也不是
 * `start_age_years`（那个是带小数的「岁数」，两个是不同的量，极易混用）。
 * 注意：`end_virtual_age` 恒 = start + 9，而 `end_civil_time` 是下一个十年边界
 * —— **故意不同步**。
 * 注意：capacity 不足时返回 ERR_CAPACITY 且一条都不写。 */
int sf_bazi_da_yun(const sf_bazi_options *o,
                   const sf_cal_datetime *birth_chart_time,
                   sf_ganzhi month_pillar, const sf_bazi_qiyun *qi_yun,
                   sf_bazi_dayun_entry *out, int capacity);

#ifdef __cplusplus
}
#endif

#endif /* SF_BAZI_H */
