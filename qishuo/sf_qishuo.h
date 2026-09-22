/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_qishuo.h —— 「一年的节气与月相」事件表
 *
 * 移植自 js-ephemeris-lite 的 `src/qi-shuo.js`：`getQiShuoYear(civilYear, options)`。
 * 给一个**公历年**和一组选项，一次拿到这一年的全部节气／七十二候／指定相位角的
 * 月相事件，每条自带时刻（UT1/TT/UTC/ΔT）、**当地民用钟面时刻**、
 * **历法归日**及其来源 —— 一屏需要的数据一次取全。
 *
 * 这是库里**唯一**按公历年汇总事件的入口。不得使用其他方式拼接：
 *   · 不得使用 sf_year_solar_terms / sf_year_lunar_phases（已删）：该入口不是
 *     公历年，且后者的目标角定义有误（一个朔望月里走遍整个相位循环）。
 *   · sf_cal_year_ut 是**冬至起的岁**（25 气 + 15 朔 + 14 月），也不是公历年。
 *   · sf_cal_get_specific_term 只给单个节气，要自己轮转索引、自己拼时间序。
 *
 * ------------------------------------------------------------------
 * 与 JS 的差异（有意为之，逐条列在这里）
 *
 * 1. **精度档叫 LOW/MED/HIGH，不是 JS 的 fast/mid/accurate**。
 *    JS 那三个是**三个不同算法**（定步长牛顿／低阶估计器+括根兜底／全视位置链），
 *    默认 `'mid'`；本库只有一个求解器，三档是**同一个求解器的三种截断**
 *    （预算表取前 N 项），默认 **HIGH**（最全，即库原来的默认）。
 *    名字不借用 JS 的：借用会让人以为 `fast` 那份精度与上游 fast 相同，
 *    而实测相反：上游 fast 对 PMO 历书 24/24 全对，本库截断到 LOW 只有 9/24。
 *
 *    实测（1900–2100 全部事件、各档相对 HIGH 自比；experimental/tier_ladder.py）：
 *
 *      档     节气 rms  节气 max   朔 rms   朔 max   预算（地 L/B/R · 月 L/B · 月 B）
 *      HIGH       0        0         0        0     L386/B50/R192 · ML639/MB277
 *      MED     3.23 s   12.93 s    2.96 s   9.89 s  L129/B16/R59  · ML129/MB64
 *      LOW    27.28 s  112.10 s   14.77 s  48.50 s  L48/B7/R16    · ML48/MB16
 *
 *    **每条轴都单调**（L/B/R 与月 L/月 B 各自递增）；若低档某一轴的预算反而
 *    多于中档，即为配置错误。
 *
 *    注意：**「项数少 3~4 倍」不等于「快 3~4 倍」。** 按预算表的行和算：
 *    135 / 397 / 1544 项，HIGH→MED 是 3.89×、MED→LOW 是 2.94×；但宿主实测
 *    （experimental/bench.c，1900–2099 整年表取平均）墙钟只快 **1.70× / 2.07×**。
 *    差的那部分是**不随预算变**的：章动、帧投影、求解器的括根迭代都不在这张
 *    预算表里。
 *
 *    这几个数也不得当作 MCU 上的倍率。级数内层是**全整数**（相位 int64 累加
 *    + Q31 查表 + int64 求和），而固定开销那边以 double 为主 —— ESP32-S3 没有
 *    双精度硬件、整数却是原生的，所以真机上**档位的收益只会比 1.7× 更小**。
 *    真机数字需把该 bench 编入 experimental/qemu_nut 那套固件中测量。
 *
 *    注意：**本表只列「事件」用得上的轴**（地 L/B/R 与月 L）。同一个符号在
 *    **求位置 / 出没**那条路上作用完全不同，不得把这里的结论搬过去：
 *
 *      量     气朔（距角）              求位置（赤纬）→ 出没时刻
 *      月 L   主导（L48 → 15 s）        主导（L7 → 414″ → 29.0 s）
 *      月 B   **零**：帧投影里 tanβ 的系数是 0，实测 1 µs
 *                                       主导（B7 → 248″ → 17.4 s；B64 → 7.9″ → 0.55 s）
 *      月 R   0.016 s（只从光行差 SF_LUNAR_ABERR×DM0/d 进，整条轴 3..325 项）
 *                                       地心赤纬几乎不动（R3 也只 0.003″）；
 *                                       它的战场是**视差** —— R129 → 0.6 s、R96 → 1.0 s
 *
 *    所以**月 B 在这里是成本杠杆，不是精度轴**：砍它对距角免费、只省项数
 *    （HIGH 的月 B 从 277 砍到 16 省 17% 项数）。它在求位置与出没上才是真
 *    精度轴，那两个 API 各自带 budget（`sf_moon_ra_dec_budget` 收 l/b/r、
 *    `sf_moon_distance_budget` 收 r），那边**没有缺口**。同理月球 R 不进
 *    `sf_elongation_budget` 的参数表，是因为距角那条路上它只值 0.016 s。
 *
 * 2. **事件名是个函数**，不是结构体里的字段。JS 的 `event.name` 对七十二候是
 *    `"冬至·三候"` 这种拼出来的串，C 中无法用静态指针表示而按事件存 24 字节又太贵。
 *    用 sf_qishuo_event_name() 现拼到调用方的缓冲里。
 *
 * 3. **越界返回负码**，不抛异常（JS 抛 TypeError/RangeError）。
 *
 *    注意：**范围边界两侧不一致**。JS 三条档位的可用年份**范围不同** ——
 *    `mid` 在 −6000 与 10000 都算得出，`fast` 只在 10000 抛 RangeError，
 *    `accurate` 两端都抛；本库三档照 SF_QISHUO_RANGE_* 一个口径收口，两端都给值。
 *    不分档的理由：**同一个公历年换个精度档就报错，是不应出现的意外**。
 *
 * 4. 事件表写进**调用方给的缓冲**，capacity 不够时返回 SF_QISHUO_ERR_CAPACITY 并在
 *    out->total 里报**上界**；要精确条数就先以 `events = NULL, capacity = 0` 询问一次。
 *
 * ------------------------------------------------------------------
 * 移植正确性：以对拍为验收依据
 *
 * experimental/qishuo_diff.py 拿 JS 的 getQiShuoYear 当 oracle，9900 组配置
 * （年 × mode × 时区 × 日界 × 开关 × 相位角）× 757791 条事件逐字段比：
 * 身份键集合、名字、序号、归日、当地日期、assignmentSource **逐字一致**，
 * 目标黄经与相位角**逐位相同**；差的全在时刻上，那是求解器精度差。
 * 细节与归因见 README §3 第 9 层。
 * ------------------------------------------------------------------ */
#ifndef SF_QISHUO_H
#define SF_QISHUO_H

#include <stddef.h>
#include <stdint.h>

#include "sf_calendar.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 年份范围，与 JS 的 QI_SHUO_INFO 逐字一致。 */
#define SF_QISHUO_RANGE_START_YEAR (-6000)
#define SF_QISHUO_RANGE_END_YEAR   (10000)

/* ------------------------------------------------------------------
 * 注意：精度说明 —— 表里那些 rms 只在**有效窗口**内成立
 *
 * 全库的有效范围是 **±8000 年**（τ = ±8，见 solar_fast.h「有效范围」）——
 * 级数排序窗口、速率多项式、帧投影表都按它定的。范围**两端**（比如 10000 年、
 * 前 6000 年）虽然照常给值，但那是**外推**：
 *
 *   · 系数是按整个 ±8000 年窗口排的，端点年份恰好在拟合的边上；
 *     退化幅度见 README §2「远期退化」表（±8000 年时几何黄经已到 0.19″ 量级，
 *     J2000 附近是 0.015″）。
 *   · 那两端的**外部真值不存在**：DE441 逐日真值只做了 J2000 ± 1000 年
 *     （730501 点），本库的精度表全部落在这段里。所以端点年份的绝对误差
 *     **没有实测数字**，不得把表里的 rms 套用到 10000 年。
 *   · 实用范围：1900–2100 可正常使用；±1000 年有真值背书；
 *     更远处「可计算，但无实测对照」。
 * ------------------------------------------------------------------ */
#define SF_QISHUO_DEFAULT_UTC_OFFSET_MINUTES 480

/* 一年的上界：24 气 + 72 候 + 8 个相位角 × 13 个朔望月 ≈ 200。
 * 取 256 留余量；事件结构约 150 B，所以调用方的缓冲约 38 KB —— 放得下就放栈上，
 * 放不下就静态分配。仿真只用 [0,180] 两个相位角，实际 50 条上下。 */
#define SF_QISHUO_MAX_EVENTS 256
/* JS 不限制相位角个数；这里封顶 8 —— 再多也没有显示价值（一年最多能塞
 * 8×13 = 104 条月相事件，已经超过 SF_QISHUO_MAX_EVENTS 的一半）。 */
#define SF_QISHUO_MAX_PHASE_ANGLES 8

/* 对应 JS 的 event.kind */
typedef enum {
    SF_QISHUO_KIND_SOLAR_TERM = 0,   /* 'solar-term'：24 个节气 */
    SF_QISHUO_KIND_PENTAD,           /* 'pentad'   ：七十二候 */
    SF_QISHUO_KIND_LUNAR_PHASE       /* 'lunar-phase'：指定相位角的月相 */
} sf_qishuo_kind;

/* 精度档。占的是 JS `eventAccuracy` 那个选项位，但**值由本库定义**：
 * 三档都是同一个求解器的截断，不是上游那三个算法。**默认 HIGH**。 */
typedef enum {
    SF_QISHUO_ACC_LOW = 0,      /* 最省：L48/B7/R16 · ML48/MB16 */
    SF_QISHUO_ACC_MED,          /* 中：  L129/B16/R59 · ML129/MB64 */
    SF_QISHUO_ACC_HIGH          /* 最全（默认，= 库原来的默认预算） */
} sf_qishuo_accuracy;

/* 对应 JS 的 event.assignmentSource */
typedef enum {
    SF_QISHUO_ASSIGN_CHINA = 0,      /* 'china-astronomical'：钉在 UTC+8 的民用日 */
    SF_QISHUO_ASSIGN_LOCAL,          /* 'local-astronomical' */
    SF_QISHUO_ASSIGN_HISTORICAL      /* 'historical-profile'：古历归日表 */
} sf_qishuo_assign_source;

/* 返回码。全部负数，0 = 成功。 */
typedef enum {
    SF_QISHUO_OK = 0,
    SF_QISHUO_ERR_YEAR = -1,               /* 年份不是 -6000..10000 的整数 */
    SF_QISHUO_ERR_OFFSET = -2,             /* utc_offset_minutes 不是 ±14 小时内的整数 */
    SF_QISHUO_ERR_MODE = -3,               /* mode 不是 SF_CAL_* 之一 */
    SF_QISHUO_ERR_ACCURACY = -4,           /* event_accuracy 不是三档之一 */
    SF_QISHUO_ERR_BOUNDARY = -5,           /* day_boundary_mode 不是两个之一 */
    SF_QISHUO_ERR_MERIDIAN = -6,           /* meridian_deg 超出 ±180 */
    SF_QISHUO_ERR_MERIDIAN_REQUIRED = -7,  /* 按经线定日界却没给 meridian_deg */
    SF_QISHUO_ERR_MERIDIAN_UNEXPECTED = -8,/* 按固定时区却给了 meridian_deg */
    SF_QISHUO_ERR_FLAGS = -9,              /* include_* 不是 0/1 */
    SF_QISHUO_ERR_ANGLES = -10,            /* 相位角个数越界或有非有限值 */
    SF_QISHUO_ERR_CAPACITY = -11,          /* capacity < 0 */
    SF_QISHUO_ERR_SOLVE = -12              /* 求解失败（该年一条事件都解不出来） */
} sf_qishuo_status;

/* 对应 JS 的 rawOptions。
 *
 * 注意：`meridian_deg` 用 **NAN 表示「没给」** —— JS 靠 `undefined` 区分
 * 「给了 0 度」和「没给」，C 里没有这个区分，NaN 是唯一干净的替身。
 * 直接 memset 清零会得到 meridian = 0.0（= 「给了 0 度」），所以在按固定时区
 * 定日界时会报 SF_QISHUO_ERR_MERIDIAN_UNEXPECTED —— 用 sf_qishuo_options_init()。 */
typedef struct {
    int32_t mode;                   /* SF_CAL_CHINA_STANDARD_HISTORICAL 等，同 sf_cal_config.mode */
    int32_t day_boundary_mode;      /* SF_CAL_FIXED_UTC_OFFSET / SF_CAL_MEAN_SOLAR_MERIDIAN */
    int32_t utc_offset_minutes;     /* 默认 480 */
    double  meridian_deg;           /* NAN = 未给 */
    int32_t event_accuracy;         /* sf_qishuo_accuracy，默认 HIGH */
    int     include_solar_terms;    /* 默认 1 */
    int     include_pentads;        /* 默认 0 */
    int     n_phase_angles;         /* 默认 1 */
    /* 归一（mod 360）、去重（1e-10）、升序 —— 由 sf_qishuo_set_phase_angles 保证 */
    double  phase_angles_deg[SF_QISHUO_MAX_PHASE_ANGLES];
} sf_qishuo_options;

/* 对应 JS events[] 的一条。时刻三件套 + 当地钟面 + 归日。 */
typedef struct {
    int32_t kind;                    /* sf_qishuo_kind */
    /* 注意：index 的语义**按 kind 不同**，不得当作唯一 ID：
     *   节气/候 —— 角度序号（0..23 或 0..71），同一个角度在一年里出现两次时**会重复**；
     *   月相   —— 该相位角自己那条流水线的序号 0,1,2…（四个角就是四条独立流水线）。 */
    int32_t index;
    int32_t term_index;              /* 0..23，i ↔ 黄经 i·15°，0 = 春分 */
    int32_t pentad_index;            /* 0..2；非候 = -1 */

    double  target_longitude_rad;    /* 节气/候：目标视黄经 */
    double  target_longitude_deg;
    double  phase_angle_rad;         /* 月相：目标距角 */
    double  phase_angle_deg;

    double  jd_ut1;                  /* 排序与窗口判据用这个 */
    double  jd_tt;                   /* 求解出来的**权威值**，其余都由它推 */
    double  jd_utc;
    double  delta_t_seconds;

    sf_cal_datetime local_time;      /* 按 utc_offset_minutes 的当地钟面 */
    int32_t local_offset_minutes;
    sf_solar_date   local_date;
    int64_t local_civil_day_number;

    int64_t assigned_civil_day_number;   /* 历法归日 */
    sf_solar_date assigned_date;
    int32_t assignment_source;           /* sf_qishuo_assign_source */
    int     assignment_differs_from_local_date;
} sf_qishuo_event;

/* 对应 JS 的返回对象（顶部那几个回显字段）。 */
typedef struct {
    int32_t civil_year;
    int32_t utc_offset_minutes;
    int32_t mode;
    int32_t event_accuracy;
    int32_t day_boundary_mode;
    double  meridian_deg;            /* 仍是 NAN 表示未给 */
    double  start_jd_ut1;            /* 窗口左端（含） */
    double  end_jd_ut1;              /* 窗口右端（不含） */
    int     count;                   /* 实际写进 events 的条数 */
    int     total;                   /* 解出来的总条数；> count 表示被容量截断 */
} sf_qishuo_year;

/* ------------------------------------------------------------------
 * 名表。index 0 = 春分，第 i 项对应 i·15° —— 与 JS 的 SOLAR_TERM_NAMES 同序。
 * SF_SOLAR_TERM_NAMES 在 solar_fast.h 里，索引规则相同。
 * ------------------------------------------------------------------ */
extern const char *const SF_PENTAD_SUFFIXES[3];   /* 初候 / 二候 / 三候 */

/* 相位名：0/90/180/270 → 朔/上弦/望/下弦；其余角度拼成 "<角度>°月相"
 * （角度按 toFixed(6) 打印，与 JS 逐字相同）。写进 buf 并返回 buf。
 * 注意：调用方给的 n 必须 **≥ 12**：最长是 "360.000000°月相"（16 字节），
 * 削掉尾零后最长 "359.999999°月相" = 11 字节 + NUL。给少了不会溢出
 * （snprintf 按字节截断），但会**截在多字节字的中间**留下半个 UTF-8 字符。
 * 角度超出 [0,360] 或 NaN 会被夹成正常值，不会吐出 "nan°月相"。 */
const char *sf_lunar_phase_name(double angle_deg, char *buf, size_t n);

/* 事件名，规则与 JS 的 event.name 逐字相同：
 *   节气 → "冬至"；候 → "冬至·三候"；月相 → 同 sf_lunar_phase_name。 */
const char *sf_qishuo_event_name(const sf_qishuo_event *ev, char *buf, size_t n);

/* ------------------------------------------------------------------
 * 选项
 * ------------------------------------------------------------------ */

/* 填默认值（照 JS 的 `rawOptions = {}`）：historical / 固定时区 UTC+8 /
 * 含节气 / 不含候 / 相位角 {0}（只要朔）/ 精度 HIGH。
 * 注意：必须先调用本函数（或自行把 meridian_deg 赋为 NAN），不得使用 memset。 */
void sf_qishuo_options_init(sf_qishuo_options *o);

/* 归一（mod 360）、去重（1e-10）、升序写入 o->phase_angles_deg。
 * n == 0 合法（= 不要任何月相事件）。有非有限值返回 SF_QISHUO_ERR_ANGLES。 */
int sf_qishuo_set_phase_angles(sf_qishuo_options *o, const double *deg, int n);

/* ------------------------------------------------------------------
 * 主入口
 *
 * 事件按 [start_jd_ut1, end_jd_ut1) 收集（左闭右开），排序同 JS：
 * jd_ut1 → kind 字典序（'lunar-phase' < 'pentad' < 'solar-term'）→ index。
 *
 * `events` 可为 NULL（只取 out 里的计数/窗口）；`capacity` 是它的长度。
 * 写不下就截断：写满 capacity 条，out->total 给出真实条数。
 * ------------------------------------------------------------------ */
int sf_qishuo_year_run(const sf_qishuo_options *o, int32_t civil_year,
                       sf_qishuo_event *events, int capacity,
                       sf_qishuo_year *out);

#ifdef __cplusplus
}
#endif
#endif /* SF_QISHUO_H */
