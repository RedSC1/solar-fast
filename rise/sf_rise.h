/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_rise.h —— 恒星时、日出日落、均时差
 *
 * 和 sf_calendar.h 一样，这一层收的是 **JD(UT)**，因为出没现象本来就
 * 活在 UT 里（地球自转角的定义就用 UT1）。solar_fast.h 的「纯 TT」契约
 * 不受影响。
 *
 * 经度**东正西负**（北京 = +116.4）。所有角度参数用度，返回的角用弧度。
 * ================================================================== */
#ifndef SF_RISE_H
#define SF_RISE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * 恒星时
 * ------------------------------------------------------------------ */

/* 格林尼治**视**恒星时，弧度，[0, 2π)。
 *
 * = ERA(UT1) + 岁差多项式(TT) + Δψ·cos(ε_true)
 * 末项即「分点差」；Δψ 由 sf_sun_ra_dec 计算但不对外返回。
 *
 * 注意：两个时间尺度都要给：ERA 取 UT，岁差和章动取 TT。传成相同的值也能
 * 运行，但 1900 年前后 ΔT 达一两分钟量级，恒星时会差 0.1″ 左右 —— 日出日落
 * 用不出差别（被 34′ 蒙气差盖住），但不得用于对表。 */
double sf_gast(double jd_ut, double jd_tt);

/* 地方视恒星时 = GAST + 东经。弧度，[0, 2π)。 */
double sf_last(double jd_ut, double jd_tt, double lon_east_deg);

/* ------------------------------------------------------------------
 * 大气 / 蒙气差
 * ------------------------------------------------------------------ */

/* 默认大气。注意：两套默认值**不一样**，取自上游口径，不得"统一"：
 *   出没路径（sf_rise_opts_default）   1013.25 mbar / 15 °C
 *   蒙气差函数自身的签名默认            1010    mbar / 10 °C
 * 分别对应 JS 的 observerSettings 与 hybridAtmosphericRefraction。 */
#define SF_ATMO_PRESSURE_MBAR        1013.25
#define SF_ATMO_TEMPERATURE_C        15.0
#define SF_REFRACTION_PRESSURE_MBAR  1010.0
#define SF_REFRACTION_TEMPERATURE_C  10.0

/* 蒙气差（hybrid 模型）：Bennett/Saemundsson 支 <=14 度、Smart 支 >=16 度、
 * 14-16 度之间线性混合，最后乘气压温度因子 pressure/1010 * 283/(273+T)。
 *
 * apparent_alt_rad 是**视**高度角，返回"加到几何高度上才得到视高度"的量；
 * 进出都是**弧度**。视高度 < -1 度硬返回 0（与 JS 一致）。
 *
 * 注意命名：上游把 1.02/tan(h + 10.3/(h+5.11)) 叫 "Bennett"，但实际是
 * Saemundsson (1986) 的式子；Bennett (1982) 是 1/tan(h + 7.31/(h+4.4))。
 * 这里按式子归类，注释两个名字都留。 */
double sf_refraction(double apparent_alt_rad, double pressure_mbar,
                     double temperature_c);

/* 同上，用蒙气差函数自身的签名默认大气（1010 mbar / 10 °C）。 */
double sf_refraction_default(double apparent_alt_rad);

/* ------------------------------------------------------------------
 * 出没
 * ------------------------------------------------------------------ */

/* 极昼 / 极夜的返回码 */
enum {
    SF_RISE_OK          = 0,
    SF_RISE_POLAR_NIGHT = 1,   /* 整天不升 */
    SF_RISE_POLAR_DAY   = 2,   /* 整天不落 */
    SF_RISE_ERROR       = -1
};

/* 一天里最多几次穿越。高纬掠射时日出/日落可能各两次。 */
#define SF_RISE_MAX 2

/* 与 JS SOLAR_ALTITUDE_STATE 一一对应 */
typedef enum {
    SF_ALT_NOT_FOUND    = 0,   /* 没找到穿越，且不是单调（掠射未穿透） */
    SF_ALT_CROSSES      = 1,
    SF_ALT_ALWAYS_ABOVE = 2,
    SF_ALT_ALWAYS_BELOW = 3,
    SF_ALT_TANGENT      = 4    /* 擦到地平线但没穿过去 */
} sf_alt_state_t;

/* 与 JS SOLAR_LIMB 一一对应 */
typedef enum {
    SF_LIMB_UPPER  = 0,   /* 默认：上边缘 */
    SF_LIMB_CENTER = 1,
    SF_LIMB_LOWER  = 2
} sf_limb_t;

/* 出没算法档。保留 `fast` 字段名，避免已有初始化代码失效；值请用这里的名字。
 * REFERENCE_GRID 只用于测试复现上游 144 点网格，不应放在产品路径。 */
typedef enum {
    SF_RISE_REFERENCE_GRID = -1,
    SF_RISE_ACCURATE       = 0,
    SF_RISE_FAST           = 1
} sf_rise_mode_t;

typedef struct {
    sf_alt_state_t state;
    int    n_rise, n_set;
    double rise[SF_RISE_MAX];   /* UT 儒略日，升序 */
    double set[SF_RISE_MAX];
} sf_rise_set_t;

typedef struct {
    sf_limb_t limb;           /* 默认 SF_LIMB_UPPER */
    int    refraction;        /* 1 = 开（默认） */
    double horizon_deg;       /* 地平高度，默认 0 */
    double pressure_mbar;     /* 默认 1013.25 */
    double temperature_c;     /* 默认 15 —— 不是蒙气差函数的 10 */
    double height_m;          /* 观测者海拔，默认 0 */
    /* SF_RISE_FAST（默认）：9 点拟合缓慢漂移的日周三角曲线，每根再做两次
     * 位置校正；月球内部使用 L129/B64/R129，太阳仍用完整位置。切触/窗口
     * 边缘等病态情形自动退回全量 ACCURATE。
     * SF_RISE_ACCURATE：驻点分割 + 有界递归二分。
     * SF_RISE_REFERENCE_GRID：测试专用的 144 点上游复现路径。 */
    int    fast;
} sf_rise_opts_t;

/* 填全套默认值。opts 传 NULL 给下面两个函数时等价于调它。 */
void sf_rise_opts_default(sf_rise_opts_t *o);

/* JS 形态：在**显式** UT 区间 [day_start_ut, day_start_ut + 1) 上求所有穿越点。
 *
 * 判据（与 JS bodyRiseSetForDay 逐项对应）：
 *     站心几何高度(边缘) + 蒙气差(按该边缘的高度求) - 地平 = 0
 * 站心位置用 WGS84 椭球 + 海拔做**精确向量相减**，不是解析视差近似；
 * 蒙气差求在边缘那根光线上，不是先给中心加再取边缘。
 *
 * 默认 FAST 先在一天内取 9 个站心位置，把
 *
 *     sin(几何中心高度) − sin(固定几何地平 − 视半径)
 *
 * 拟合成「二次缓变系数 × 一次日周正余弦」；在廉价代理曲线上找全体交点，
 * 每根再做两次位置校正。蒙气差先反解成固定几何地平，不把地平附近很坏的
 * 蒙气差导数塞进牛顿法。另取两个真实窗口端点守日期归属，正常一升一落共
 * 15 次星历采样。月球 FAST 单独使用 L129/B64/R129；公开月球位置与
 * ACCURATE 保持全量。
 *
 * 拟合极值/窗口端点距地平 0.2° 内，或交点斜率过小时，FAST 自动退回下面的
 * ACCURATE；这些正是切触、极圈和日期归属对小误差敏感的窗口。
 *
 * ACCURATE 的两层结构：
 *
 *   1. **驻点分割法**（主路径）。判据对时间的导数近似正比于 sin(H)，
 *      驻点大致在中天；把窗口内的中天解出来连同两端排序，相邻两点之间
 *      判据接近单调 —— 数符号变化即数穿越次数。一天二十几次求值。
 *
 *   2. **有界递归二分**（兜底）。基于 cosinekitty / Astronomy Engine
 *      `FindAscent` 思路改写（许可见 NOTICE）。靠一个**与星历无关**的斜率上界
 *          |d(判据)/dt| ≤ [|(360.9856° − Δα̇)·cosφ| + |δ̇·sinφ|] × 1.5
 *      做 Nyquist 剪枝：两端同号且都离零够远 ⇒ 判据来不及跑到零再回来 ⇒
 *      整段否掉。其余一律细分，直到区间窄于 1 秒，再在那一秒里二分到底。
 *      **完全不依赖驻点**。
 *
 * 注意：第 1 层的「近似」有分量。导数里还有一个**赤纬漂移项**，太阳
 * 0.4°/天可忽略，月球最多 6.6°/天不可；在极区 ∂alt/∂H → 0 时该项反过来
 * 主宰，**连符号都能翻**。故第 1 层的适用前提是「驻点不贴近地平线」，
 * 一旦有驻点落进 1° 就整日交给第 2 层 —— 见 sf_rise.c 里 extrema_suspect
 * 那一段的实测记录。
 *
 * 两层都不碰 144 点网格；网格只服务于 SF_RISE_REFERENCE_GRID 的逐位对拍。
 *
 * 实测：0~90° 全纬度、月球与太阳各 5000 天/纬度（37 万例）与网格逐点一致，
 * 0 不符；88~90°（9 万例）同样 0 不符；跨 1968–2050 的 29 纬度大网格对拍
 * 最大时刻差 0.72 ms。
 *
 * ESP32-S3 240 MHz 真机（北京基准日，一天 = 升+落）：
 *     天体       FAST         ACCURATE       提速
 *     太阳     28.822 ms      274.692 ms      9.53x
 *     月亮     29.078 ms      501.435 ms     17.24x
 * FAST vs ACCURATE 在 −6000..10000 随机 20000 窗口上状态/根数 0 不符，
 * 月亮时刻误差 RMS 0.292 s、P99 1.187 s、最大 3.541 s；另一个卡边界年份、
 * 极区和多经度的 63756 窗口结构网格最坏 4.908 s，状态/根数仍 0 不符。
 * 病态窗口由守门自动退回全量 ACCURATE。
 *
 * 返回 0 成功，-1 参数非法。 */
int sf_sun_rise_set_for_day(double day_start_ut, double lat_deg, double lon_east_deg,
                            const sf_rise_opts_t *opts, sf_rise_set_t *out);
int sf_moon_rise_set_for_day(double day_start_ut, double lat_deg, double lon_east_deg,
                             const sf_rise_opts_t *opts, sf_rise_set_t *out);

/* --- 旧接口：签名不变，内部调用新实现 ---
 *
 * 求 jd_ut 所在**当地（按经度）那一天的**出没，写回 UT 儒略日。
 *
 * 窗口复刻 JS computeSolarRiseSetFast：以「离 jd_ut 最近的地方时正午」为心，
 * [localNoon-0.5, localNoon+0.5)。**不是 UT 日** —— 北京那天的日出落在 UT 日
 * 之前，用 UT 日会把日出漏掉、昼长算成负的。
 *
 * 日期归属采用显式 UT 区间求根，根的归属由区间定义；不使用角度方程
 * t = (alpha - H0 - lst0)/rate —— 其分子 α − H0 − lst0 带任意 2πk，会把根
 * 归到相邻日。
 *
 * 蒙气差采用 hybrid、站心视差采用精确算法后，与 taiyin-lite 的差从约 2 s rms
 * 降到噪声级。没有穿越时按极昼/极夜返回；只有一半穿越（高纬掠射）时，
 * 现返回码表达不了，也按极昼/极夜近似 —— 要精确区分请用 _for_day 那个。 */
int sf_sun_rise_set(double jd_ut, double lat_deg, double lon_east_deg,
                    double *out_rise_ut, double *out_set_ut);
int sf_moon_rise_set(double jd_ut, double lat_deg, double lon_east_deg,
                     double *out_rise_ut, double *out_set_ut);

/* 同一次求根同时给日出、日落与昼长，供一屏同时显示三者的调用方使用。
 * 返回码和 sf_sun_rise_set 相同；极昼/极夜时 out_day_length_hours 分别为
 * 24/0，rise/set 与旧接口一样未定义。不得先调 sf_sun_rise_set 再调
 * sf_day_length_hours，那会把同一天完整求两遍。 */
int sf_sun_rise_set_day_length(double jd_ut, double lat_deg, double lon_east_deg,
                               double *out_rise_ut, double *out_set_ut,
                               double *out_day_length_hours);

/* 同上，但自己指定几何高度角（度）—— 解**站心中心高度**穿过 h0_deg，
 * 不含蒙气差、不含视半径。想要"民用晨昏蒙影"传 h0_deg = -6。
 *
 * 注：解的是站心中心高度，非地心；太阳视差 8.8 角秒，约合 0.6 s。 */
int sf_sun_rise_set_at(double jd_ut, double lat_deg, double lon_east_deg,
                       double h0_deg, double *out_rise_ut, double *out_set_ut);

/* 昼长，小时。极昼 24、极夜 0。
 *
 * 本函数与 sf_sun_rise_set 都会调用 `sf_sun_rise_set_for_day` 完成整天求根。
 * 同时需要日出、日落和昼长时，可调用 `sf_sun_rise_set_for_day` 一次取得全部结果。
 *
 * 默认 FAST 的普通一升一落日需要 15 次星历采样；ESP32-S3 真机太阳
 * 28.8 ms。 */
double sf_day_length_hours(double jd_ut, double lat_deg, double lon_east_deg);

/* ------------------------------------------------------------------
 * 太阳高度 / 方位
 * ------------------------------------------------------------------ */

/* 太阳的**站心几何**高度角（弧度），**不含**蒙气差。
 * 要视高度就自己加上 sf_refraction(...) 的返回值。
 * out_azimuth 可为 NULL；方位角自北起、向东为正，弧度 [0, 2pi)。 */
double sf_sun_altitude(double jd_ut, double lat_deg, double lon_east_deg,
                       double *out_azimuth);

/* ------------------------------------------------------------------
 * 均时差 / 真太阳时
 * ------------------------------------------------------------------ */


/* 均时差，**分钟**。定义：真太阳时 − 平太阳时。
 * 一年里在 −14.2 … +16.4 分钟之间摆动。 */
double sf_equation_of_time_minutes(double jd_tt);

/* 真太阳时（小时，0..24）。 = 平太阳时 + 均时差/60 */
double sf_true_solar_time_hours(double jd_ut, double lon_east_deg);

/* 平太阳时（小时，0..24）。 = UT + 东经/15 */
double sf_mean_solar_time_hours(double jd_ut, double lon_east_deg);

/* ------------------------------------------------------------------
 * 太阳地面观测者全要素合算（Observer System）
 * ------------------------------------------------------------------ */

/**
 * 太阳地面观测要素完整结果（零堆内存、单次级数展开一步出）
 */
typedef struct {
    /* 1. 地心视位置 */
    double ra_rad;                  /* 地心视赤经 (rad, [0, 2pi)) */
    double dec_rad;                 /* 地心视赤纬 (rad, [-pi/2, pi/2]) */
    double distance_au;             /* 地心日地距离 (AU) */

    /* 2. 站心视位置与时角 (已修正站心周日视差与海拔) */
    double topo_ra_rad;             /* 站心赤经 (rad, [0, 2pi)) */
    double topo_dec_rad;            /* 站心赤纬 (rad, [-pi/2, pi/2]) */
    double topo_dist_au;            /* 站心距离 (AU) */
    double hour_angle_rad;          /* 站心地方时角 (rad, [-pi, pi]，南为0，西为正) */

    /* 3. 站心地平坐标 */
    double azimuth_deg;             /* 真方位角 (deg, 正北为0，顺时针 0..360) */
    double altitude_geometric_deg;  /* 站心几何高度角 (deg, 未加蒙气差) */
    double altitude_apparent_deg;   /* 站心视高度角 (deg, 已修正大气蒙气差) */
    double zenith_deg;              /* 视天顶角 (deg, 90° - altitude_apparent) */

    /* 4. 时间与均时差 */
    double gast_rad;                /* 格林尼治真恒星时 (rad, [0, 2pi)) */
    double last_rad;                /* 地方真恒星时 (rad, [0, 2pi)) */
    double equation_of_time_min;    /* 均时差 (分钟, 真太阳时 - 平太阳时) */
} sf_sun_observed_t;

/**
 * 太阳全要素观测位置合算（单次级数展开，全要素一步出）
 * 
 * @param jd_ut        世界时儒略日 (UT)
 * @param lat_deg      观测者纬度 (度, 北纬为正)
 * @param lon_east_deg 观测者经度 (度, 东经为正)
 * @param elevation_m  海拔高度 (米，默认给 0.0)
 * @param pressure_hpa 气压 (hPa / mbar，常用默认 1013.25；若 <= 0 则忽略蒙气差)
 * @param temp_c       气温 (摄氏度，常用默认 15.0)
 * @param out          输出结构体指针 (不可为 NULL)
 * @return 0 成功，-1 参数非法
 */
int sf_sun_observed(double jd_ut, double lat_deg, double lon_east_deg,
                    double elevation_m, double pressure_hpa, double temp_c,
                    sf_sun_observed_t *out);

/**
 * 倾斜采光面/太阳能板的太阳光入射角 (Solar Incidence Angle)
 * cos(theta) = cos(zenith)*cos(slope) + sin(zenith)*sin(slope)*cos(sun_azimuth - surface_azimuth)
 *
 * @param zenith_deg          太阳视天顶角 (度, 0..180)
 * @param sun_azimuth_deg     太阳方位角 (度, 正北=0, 顺时针 0..360)
 * @param slope_deg           采光面倾角 (度, 水平=0, 垂直=90)
 * @param surface_azimuth_deg 采光面朝向方位角 (度, 正北=0, 正东=90, 正南=180, 正西=270)
 * @return 入射角 (度, 0..180)
 */
double sf_solar_incidence_angle(double zenith_deg, double sun_azimuth_deg,
                                double slope_deg, double surface_azimuth_deg);

/**
 * 太阳中天时刻（正午太阳上中天 / Solar Transit）
 * 返回给定 UT 当日太阳上中天的儒略日 (JD UT)。
 *
 * @param jd_ut        所求日期附近的任意 UT 儒略日
 * @param lon_east_deg 观测者东经 (度)
 * @return 太阳中天时刻的 JD UT
 */
double sf_sun_transit(double jd_ut, double lon_east_deg);

#ifdef __cplusplus
}
#endif
#endif /* SF_RISE_H */
