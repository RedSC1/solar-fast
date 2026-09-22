/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * 派生自 js-ephemeris-lite v1.2.0（MPL-2.0），详见 NOTICE。
 */
/* ==================================================================
 * sf_solve.c —— 气朔快速求解 + 牛顿斜率
 *
 * 与 fl_qishuo.c 的 fast 档对应，并增加括根回退条件。主要约束如下：
 *
 * 1) 主路径使用固定次数的牛顿迭代：太阳两段、朔三段。最后一步大于
 *    SF_EVENT_FALLBACK_STEP_DAYS 时，改用带括根的 sf_safeguarded。
 *
 *    最后一步的 step = wrap(f(jd) − target)/rate，可作为当前迭代的修正量
 *    使用，无需额外求值；未触发回退时，主路径结果不变。
 *
 *    此条件可检测周期选择错误、速率异常和日期窗口边缘问题；不能检测
 *    截断档自身的模型误差，因为修正量是相对同一截断模型计算的。
 *
 *    上游 JS 的 `mid` 档使用类似回退机制。本实现沿用其回退条件，
 *    但不使用上游的低阶估计器和漂移表。
 *
 * 2) 速率是拟合的近似斜率，不是物理角速度。
 *    尤其是 sf_elongation_rate：J2000 处约 0.1921 rad/day，而真实平均朔望
 *    速率是 0.2128 rad/day，约低 10%。这会影响固定迭代次数下的残差：
 *    朔的模型内残差可达约 0.5 s。
 *
 *    收紧 SF_EVENT_FALLBACK_STEP_DAYS 会增加回退比例，不能改善常规路径
 *    的收敛率。增加一次固定牛顿迭代可将实测最大残差从 0.499 s 降至
 *    0.005 s，求值次数增加约 33%。
 *
 * 3) **周期选择（2πk）只发生在 sf_solve_* 这一层**。下层函数吃的是未缠绕
 *    角度，2πk 直接映射到第 k 个回归/朔望月。
 * ================================================================== */
#include "solar_fast.h"
#include "sf_internal.h"

#include <math.h>
#include <stddef.h>

/* ------------------------------------------------------------------
 * 护栏参数
 *
 * SF_EVENT_FALLBACK_STEP_DAYS：主路径最后一步超过这么多天就转兜底。
 *
 * 在 ±8000 年范围内对 175 万 + 6.5 万次事件的测试结果：
 *   正常 |step| 的上界在 **0.005 ~ 0.01 天**之间（0.01 天时一次都不触发）。
 *   它远大于截断误差本身，因为 stage 1 用的是很粗的 L96/B3/R16 —— 这个
 *   最后一步包含 stage 1 粗模型与 stage 2 目标档之间的差。
 *   周期选择异常可达半个周期：朔 14.8 天、气 182 天（种子落入相邻周期，
 *   或速率反零反号）。
 *
 * 因此取 0.5 天作为阈值，约为正常上界的 50 倍、最小异常量级的 1/30。
 *
 * 此阈值用于识别异常迭代，不用于控制正常路径的模型精度。
 *
 * 注意：护栏抓不到截断档自身的定点偏差：残差相对截断模型是零，任何残差
 * 检查都看不见它；截断精度需单独评估。
 *
 * SF_EVENT_TOL_SECONDS：兜底路径的目标残差（秒）。
 *
 * 括根上限由缠绕不变量确定：残差用 sf_wrap 折到 (−π,π]，
 * 只有当括根跨度内角度推进小于 π 时，"左端 ≤ 0 且右端 ≥ 0"才等价于"中间
 * 有根"。太阳每天走 2π/365.25 ≈ 0.0172 rad、朔望每天 2π/29.53 ≈ 0.2128 rad，
 * 于是跨度上限分别是 182 天和 14.8 天。下面的 half_max 取的是保守值。
 * ------------------------------------------------------------------ */
#ifndef SF_EVENT_FALLBACK_STEP_DAYS
#define SF_EVENT_FALLBACK_STEP_DAYS 0.5
#endif
#ifndef SF_EVENT_TOL_SECONDS
#define SF_EVENT_TOL_SECONDS 1.0e-4
#endif
#define SF_EVENT_MAX_ITER 60

/* 回退路径以事件种子（平均运动线性反解）为中心，避免依赖异常的主路径结果。
 * 种子自身误差 = 中心差：
 * 太阳 0.0334 rad → ~1.9 天，月亮 ~0.11 rad → ~0.5 天，所以下面的初始半宽
 * 足以给出初始括根区间；必要时仍可扩宽。 */
#ifndef SF_SOLAR_HALF0
#define SF_SOLAR_HALF0   4.0
#endif
#ifndef SF_SOLAR_HALFMAX
#define SF_SOLAR_HALFMAX 48.0     /* 跨度 96 天 < 182 */
#endif
#ifndef SF_LUNAR_HALF0
#define SF_LUNAR_HALF0   1.5
#endif
#ifndef SF_LUNAR_HALFMAX
#define SF_LUNAR_HALFMAX 6.0      /* 跨度 12 天 < 14.8 */
#endif

/* 可选：在固定的 2/3 段之后再补一段同样的牛顿。
 *
 * 收的是**求解器残差**（模型内）：朔 0.499 s → 0.005 s（100 倍）、
 * 气 0.149 s → 0.070 s。代价 +1 次求值，不动任何数据表。
 *
 * 注意：对真值无用，两边都测了：
 *   节气（DE441，1000–3000 AD 共 48001 个）：rms 0.8040 → 0.8034 s，max 4.06 → 4.12 s
 *   朔  （DE441，1000–3000 AD 共 24734 个）：rms 0.3069 → 0.3081 s，max 1.30 → 1.25 s
 * 相比模型误差，这部分残差较小，因此默认关闭。 */
#ifndef SF_EXTRA_STAGE
#define SF_EXTRA_STAGE 0
#endif

/* 诊断：解出来的同时报告走了哪条路。0 = 定步长，1 = 括根兜底，-1 = 失败 */
#define SF_PATH_FAST 0
#define SF_PATH_GUARD 1
#define SF_PATH_FAIL (-1)

/* 值 + 导数一次 Horner 出来。多项式都在 x = (jd − J2000)/2922000 上，
 * 所以 rate 要除掉 SF_SCALE_DAYS 变成"每天"。 */
typedef struct { double value, rate; } sf_poly_t;

static sf_poly_t sf_poly_rate(const double *a, int n, double x)
{
    double value = 0.0, rate = 0.0;
    for (int i = n - 1; i >= 0; i--) {
        rate = rate * x + value;
        value = value * x + a[i];
    }
    sf_poly_t r;
    r.value = value;
    r.rate = rate / SF_SCALE_DAYS;
    return r;
}

/* ==================================================================
 * 速率
 * ================================================================== */

/* 4 项章动速率。系数只算一次（函数内静态缓存）。 */
static double sf_nutation_rate4(double jd)
{
    static int inited = 0;
    static double p[4], f[4], a[4], b[4], c[4];
    if (!inited) {
        static const double defA[5] = {
            485868.249036, 1287104.79305, 335779.526232, 1072260.70369, 450160.398036
        };
        static const double defB[5] = {
            1717915923.2178, 129596581.0481, 1739527262.8478, 1602961601.2090, -6962890.5431
        };
        for (int i = 0; i < 4; i++) {
            /* SF_IAU2000B 是 int32 表（章动主路径改成了全整数），
             * 这里只有 4 项、且是速率函数，留着 double 无所谓，
             * 但指针类型得跟着改。 */
            const int32_t *r = SF_IAU2000B[i];
            double phase = 0.0, speed = 0.0;
            for (int j = 0; j < 5; j++) {
                phase += r[j] * defA[j];
                speed += r[j] * defB[j];
            }
            p[i] = phase * SF_ARCSEC_TO_RAD;
            f[i] = speed * SF_ARCSEC_TO_RAD;
            a[i] = r[5] * 1e-7 * SF_ARCSEC_TO_RAD;
            b[i] = r[6] * 1e-7 * SF_ARCSEC_TO_RAD;
            c[i] = r[7] * 1e-7 * SF_ARCSEC_TO_RAD;
        }
        inited = 1;
    }
    const double t = (jd - SF_J2000) / SF_CENTURY;
    double rate = 0.0;
    for (int i = 0; i < 4; i++) {
        const double arg = p[i] + f[i] * t;
        rate += (b[i] * sin(arg) + (a[i] + b[i] * t) * f[i] * cos(arg)
                 - c[i] * f[i] * sin(arg)) / SF_CENTURY;
    }
    return rate;
}

static double sf_precession_rate(double jd)
{
    const double x = (jd - SF_J2000) / SF_SCALE_DAYS;
    return sf_poly_rate(SF_LOW_PRECESSION_RATE, SF_LOW_PRECESSION_RATE_N, x).value;
}

/* 地球本体系速率：长期项 + 两组谐波。
 * 谐波相位用 τ（儒略千年），幅度包络多项式用 x（2922000 天）。 */
static double sf_earth_native_rate(double jd, int harmonics)
{
    const double x = (jd - SF_J2000) / SF_SCALE_DAYS;
    const double tau = (jd - SF_J2000) / SF_MILLENNIUM;
    double rate = sf_poly_rate(SF_SOLAR_RATE_SECULAR, SF_SOLAR_RATE_SECULAR_N, x).rate;
    for (int i = 0; i < harmonics && i < SF_SOLAR_RATE_HARM_N; i++) {
        const sf_harm_t *h = &SF_SOLAR_RATE_HARM[i];
        const sf_poly_t cc = sf_poly_rate(h->cosPoly, h->cosN, x);
        const sf_poly_t ss = sf_poly_rate(h->sinPoly, h->sinN, x);
        const double ph = h->freq * tau;
        const double cp = cos(ph), sp = sin(ph);
        rate += cc.rate * cp + ss.rate * sp
              + h->freq / SF_MILLENNIUM * (ss.value * cp - cc.value * sp);
    }
    return rate;
}

double sf_solar_rate(double jd)
{
    return sf_earth_native_rate(jd, 2) + sf_precession_rate(jd) + sf_nutation_rate4(jd);
}

/* 月球速率：W1 多项式 + 前 40 项（按 |A| 降序）里的前 term_count 项。
 * 这里用 libm pow()，与 JS 的 x**n 对应。 */
static double sf_moon_native_rate(double jd, int term_count)
{
    const double x = (jd - SF_J2000) / SF_SCALE_DAYS;
    double rate = sf_poly_rate(SF_MOON_W1, SF_MOON_W1_N, x).rate;
    if (term_count > SF_MOON_RATE_TOP_N) term_count = SF_MOON_RATE_TOP_N;
    for (int i = 0; i < term_count; i++) {
        const int n = SF_MOON_RATE_TOP[i].power;
        const double s = SF_MOON_RATE_TOP[i].A;
        const double c = SF_MOON_RATE_TOP[i].B;
        const double *pp = SF_MOON_ARG[SF_MOON_RATE_TOP[i].arg_index];

        double a = pp[7], d = a;              /* d = a'(x) */
        for (int k = 6; k >= 0; k--) { a = a * x + pp[k]; d = d * x + a; }
        a *= x;
        const double sn = sin(a), cs = cos(a);
        rate += (s * cs - c * sn) * d / SF_SCALE_DAYS * pow(x, (double)n)
              + (n == 0 ? 0.0
                        : (double)n * pow(x, (double)(n - 1)) / SF_SCALE_DAYS
                              * (s * sn + c * cs));
    }
    return rate;
}

/* 距角速率：没有岁差、没有章动项 —— 它们在这个差值里约掉了。
 * 帧倾斜与小的周期速率项是刻意省略的近似。 */
double sf_elongation_rate(double jd)
{
    return sf_moon_native_rate(jd, 10) - sf_earth_native_rate(jd, 2);
}

/* 兜底路径专用的斜率：月亮取前 40 项（表本来就在，SF_MOON_RATE_TOP_N）。
 * 更靠近根时 10 项斜率的误差开始影响牛顿的二次收敛，40 项就没有这个问题。
 * 只用于兜底 —— 主路径仍走上面那个 10 项的 sf_elongation_rate。 */
static double sf_elongation_rate_refine(double jd)
{
    return sf_moon_native_rate(jd, SF_MOON_RATE_TOP_N) - sf_earth_native_rate(jd, 2);
}

/* ==================================================================
 * 护栏：带括根的兜底求解
 *
 * 对应 JS 的 solveSafeguarded（calendar-events.js）。先几何扩宽找到一个
 * 变号区间，再在区间内牛顿 + 中值兜底，直到残差降到 SF_EVENT_TOL_SECONDS。
 *
 * 注意：三个与 JS 不同的地方，都是为了正确性：
 *   1) 扩宽有**上限**（half_max），不是扩到 6 次为止 —— JS 从 2 天起 ×1.8
 *      扩 6 次能到 ±38 天，跨度 75 天已经跨了两三个朔望月，那个括根是假的。
 *   2) 括根端点求值失败（出窗口/非有限）直接放弃，不继续扩。
 *   3) 60 步不收敛返回失败，由调用方给 NAN —— 兜底再兜底没有意义，
 *      宁可明说"没解出来"也不给一个看起来像数字的东西。
 * ================================================================== */

typedef struct { double value, rate; } sf_event_state_t;

/* 返回 1 表示该点可用（有限 + 在窗口内） */
typedef int (*sf_event_eval_fn)(void *ctx, double jd, sf_event_state_t *out);

/* 只挡非有限值 —— 「NaN 进、NaN 出」是常识，不是范围栏杆。
 *
 * 注意：不设 `|jd − J2000| ≤ 2922000 天`（±8000 年）的硬限制，超出不返回 NAN。
 * 上游同样没有这道栏杆：JS 只有 `checkedEventDate`，且仅在 fast/accurate 档
 * 调用；默认 mid 档不检查，一路外推。若在此设限，同年份（如 10000 年）上游
 * 给 12 条事件、本库只给 2 条，属接口行为不一致。
 *
 * 有效范围仍为 ±8000 年（级数排序窗口、速率多项式、帧投影表均按它定义），
 * 但它不作为取值判据：出界后仍返回外推结果。 */
static int sf_event_date_ok(double jd)
{
    return isfinite(jd);
}

static int sf_safeguarded(sf_event_eval_fn eval, void *ctx, double target,
                          double start, double half0, double half_max,
                          double *out)
{
    sf_event_state_t s;
    double left = 0.0, right = 0.0, f_left = 0.0, f_right = 0.0;
    int bracketed = 0;

    for (double half = half0; ; half *= 2.0) {
        if (half > half_max) half = half_max;
        left  = start - half;
        right = start + half;
        if (!eval(ctx, left, &s)) break;
        f_left = sf_wrap(s.value - target);
        if (!eval(ctx, right, &s)) break;
        f_right = sf_wrap(s.value - target);
        /* 角度单调增 → 残差从负穿到正 */
        if (f_left <= 0.0 && f_right >= 0.0) { bracketed = 1; break; }
        if (half >= half_max) break;
    }
    if (!bracketed) return 0;

    double jd = start;
    if (jd < left)  jd = left;
    if (jd > right) jd = right;

    for (int iter = 0; iter < SF_EVENT_MAX_ITER; iter++) {
        if (!eval(ctx, jd, &s) || s.rate <= 0.0) return 0;
        const double residual = sf_wrap(s.value - target);
        const double step = residual / s.rate;
        if (!isfinite(step)) return 0;
        if (fabs(step) * 86400.0 <= SF_EVENT_TOL_SECONDS) { *out = jd; return 1; }

        if (residual < 0.0) left = jd; else right = jd;
        double next = jd - step;
        if (!(next > left && next < right)) next = 0.5 * (left + right);
        if (fabs(next - jd) * 86400.0 <= SF_EVENT_TOL_SECONDS) { *out = next; return 1; }
        jd = next;
    }
    return 0;
}

/* ---- 兜底的两个求值器（用的是调用方给的档，不是另开一套模型）---- */

typedef struct { int l_bud, b_bud, r_bud; } sf_solar_ctx_t;

static int sf_eval_solar(void *ctxp, double jd, sf_event_state_t *out)
{
    const sf_solar_ctx_t *c = (const sf_solar_ctx_t *)ctxp;
    if (!sf_event_date_ok(jd)) return 0;
    out->value = sf_sun_apparent_longitude_budget(jd, c->l_bud, SF_DEF_NUT,
                                                  c->b_bud, c->r_bud);
    out->rate = sf_solar_rate(jd);
    return isfinite(out->value) && isfinite(out->rate);
}

/* 朔的兜底上下文：档跟着调用方走（同太阳那边）。
 * 注意：默认档下与原硬写版逐位相同 —— 兜底只在 |step| > 0.5 天时触发，
 * 正常路径一次都不多算。 */
typedef struct {
    int moon_l, earth_l, moon_b, earth_b, earth_r;
} sf_lunar_ctx_t;

static int sf_eval_lunar(void *ctxp, double jd, sf_event_state_t *out)
{
    const sf_lunar_ctx_t *c = (const sf_lunar_ctx_t *)ctxp;
    if (!sf_event_date_ok(jd)) return 0;
    out->value = sf_elongation_budget(jd, c->moon_l, c->earth_l, c->moon_b,
                                      c->earth_b, c->earth_r);
    out->rate = sf_elongation_rate_refine(jd);
    return isfinite(out->value) && isfinite(out->rate);
}

/* ==================================================================
 * 求解器
 * ================================================================== */

/* ==================================================================
 * 第一段的粗定位档 —— 跟着调用方的档走，不写死
 *
 * 第一段的档跟着调用方走，不写死。若硬编码 L96/B3/R16，会有两处问题：
 *   1) 倒挂：调用方要 L16，stage 1 却跑 L96 —— 粗定位比精修还贵。
 *   2) 白花：调用方给 SPA 档（L129/B7/R59），stage 1 照样跑 L96，多出 59%
 *      的开销却无收益。
 *
 * 规则分两半（`sf_stage1_bud`）：
 *
 *   (a) **调用方档本身就不比 cap 细** → 第一段直接用调用方档。
 *       这样第二段就是**同档的第二次牛顿迭代**，而不是"粗种子 + 一次精修"。
 *       差别很大：`sf_solar_rate` 是全模型的速率，粗档模型的导数与它差得远，
 *       一次迭代收不干净。实测 max|step2| 从 1.4e-3 天涨到 3.2e-2 天（23 倍），
 *       折合残差 0.14 s → 2.70 s。代价是这类档要多跑一次求值 —— 但它们本来就便宜。
 *
 *   (b) **调用方档比 cap 细** → 挑一个约 1/SF_COARSE_DIV 的便宜种子。
 *       cap = 原来写死的那一档（L96/B3/R16）。默认档下 (b) 挑出来的
 *       恰好就是原值，所以**默认路径逐位不变**（已验证 42291 个气朔时刻）。
 *
 *   floor = 不能再粗的下限。L 取 L16：L7 的模型误差折合 1.35 天，已经越过
 *           护栏阈值（0.5 天）；L16 是 0.0145 天，安全两个数量级。
 *           （L3 → L7 之间的模型误差相差约 94 倍。）
 * ================================================================== */
/* 实测选 2（残差 vs 开销的权衡，见下表）。越大越省但 stage 2 的步长越大：
 *   DIV   ACCURATE残差  L192残差  SPA残差   开销(ACC/L192/SPA)
 *    2     0.149 s      0.148 s   0.198 s   1.18× / 1.26× / 1.43×   ← 选这个
 *    3     0.149 s      0.217 s   0.481 s   1.18× / 1.19× / 1.26×
 *    4     0.149 s      0.229 s   0.485 s   1.18× / 1.15× / 1.22×
 *    6     0.217 s      0.481 s   2.708 s   1.13× / 1.12× / 1.13×
 * DIV=2 下 ≥L192 的调用方挑到的还是 L96，与原来写死的档**逐位相同**。 */
#ifndef SF_COARSE_DIV
#define SF_COARSE_DIV 2
#endif

/* caller 是 *_PC 的行索引；负数（文档里的"全量"写法）按最后一行算 */
static int sf_coarse_pick(const int *base, int stride, int nlev,
                          int caller, int cap, int floor_lev)
{
    int c = caller;
    if (c < 0 || c >= nlev) c = nlev - 1;
    int total = 0;
    for (int g = 0; g < stride; g++) total += base[c * stride + g];
    const int target = total / SF_COARSE_DIV;

    const int hi = (cap < nlev - 1) ? cap : nlev - 1;
    int pick = floor_lev;
    for (int i = floor_lev; i <= hi; i++) {
        int n = 0;
        for (int g = 0; g < stride; g++) n += base[i * stride + g];
        if (n <= target) pick = i;
    }
    return pick;
}

/* 第一段该用哪个档：见上面 (a)/(b) 两条规则 */
static int sf_stage1_bud(const int *base, int stride, int nlev,
                         int caller, int cap, int floor_lev)
{
    if (caller >= 0 && caller <= cap) return caller;   /* (a) 同档两次迭代 */
    return sf_coarse_pick(base, stride, nlev, caller, cap, floor_lev);  /* (b) */
}

/* 用平均运动线性反解一个初值。相位常数与 JS 同。 */
static double sf_event_angle_seed(double angle, int lunar)
{
    return SF_J2000 + (lunar ? (angle + 1.08472) / 7771.37714500204
                             : (angle - 1.75347 - M_PI) / 628.3319653318) * SF_CENTURY;
}

/* ==================================================================
 * 低项数气朔：只给“离历法日界够不够远”的归日预判用
 *
 * 寿星万年历的 qi_low / so_low 是少项公式 + 近午夜回退高精度。本库不能直接
 * 搬那两个公式：去掉它们内嵌的 ΔT 与北京时间后，在 -6000..10000 年远端仍
 * 会漂到约 15 小时。这里保留调度思路，值模型换成本库按 ±8000 年排过名次的
 * 事件方程短路径：
 *
 *   气：Earth L32 / B3 / nutation 9，固定两次平均速率修正
 *   朔：同一原生黄道系 Moon L16 - Earth L16，固定三次平均速率修正
 *
 * 旧方案每轮还调用通用拟合速度，故板上仅快 1.7～3.6 倍。此处用寿星式固定
 * 平均速率；朔多做一轮短方程，把常速两轮留下的约一小时尾差收掉。
 *
 * 范围是历年 -6000..10000，对应 J2000 左右各 8000 儒略年。范围外直接走
 * 当前完整算法，避免低档默默外推。没有缓存，也没有可变状态。
 * ================================================================== */
#define SF_LOW_HALF_RANGE_DAYS (8000.0 * 365.25)

static int sf_low_event_range(double jd)
{
    return isfinite(jd)
        && jd >= SF_J2000 - SF_LOW_HALF_RANGE_DAYS
        && jd <= SF_J2000 + SF_LOW_HALF_RANGE_DAYS;
}

double sf_solar_term_time_low(double longitude_rad)
{
    const double seed = sf_event_angle_seed(longitude_rad, 0);
    double jd = seed;
    if (!sf_low_event_range(jd)) return sf_solar_term_time(longitude_rad);

    for (int i = 0; i < 2; i++) {
        const double value = sf_sun_event_longitude_low(jd);
        const double rate = 628.3319653318 / SF_CENTURY;
        if (!isfinite(value))
            return sf_solar_term_time(longitude_rad);
        jd -= sf_wrap(value - longitude_rad) / rate;
        if (!sf_low_event_range(jd)) return sf_solar_term_time(longitude_rad);
    }
    return jd;
}

double sf_lunar_phase_time_low(double elongation_rad)
{
    const double seed = sf_event_angle_seed(elongation_rad, 1);
    double jd = seed;
    if (!sf_low_event_range(jd)) return sf_lunar_phase_time(elongation_rad);

    for (int i = 0; i < 3; i++) {
        const double value = sf_lunar_event_elongation_low(jd);
        const double rate = 7771.37714500204 / SF_CENTURY;
        if (!isfinite(value))
            return sf_lunar_phase_time(elongation_rad);
        jd -= sf_wrap(value - elongation_rad) / rate;
        if (!sf_low_event_range(jd)) return sf_lunar_phase_time(elongation_rad);
    }
    return jd;
}

double sf_solar_term_time_budget_path(double longitude_rad, int l_bud, int b_bud,
                                      int r_bud, int *path)
{
    const double longitude = longitude_rad;
    const double seed = sf_event_angle_seed(longitude, 0);
    double jd = seed;
    if (path) *path = SF_PATH_FAIL;
    if (!sf_event_date_ok(jd)) return NAN;

    /* 阶段 1：便宜的粗定位。档跟着调用方走（默认下就是原来的 L96/B3/R16）。 */
    const int l1 = sf_stage1_bud(&SF_EARTH_L_PC[0][0], SF_EARTH_L_NG, SF_EARTH_L_NBUD,
                                 l_bud, SF_EARTH_L_BUD_96, SF_EARTH_L_BUD_16);
    const int b1 = sf_stage1_bud(&SF_EARTH_B_PC[0][0], SF_EARTH_B_NG, SF_EARTH_B_NBUD,
                                 b_bud, SF_EARTH_B_BUD_3, SF_EARTH_B_BUD_3);
    const int r1 = sf_stage1_bud(&SF_EARTH_R_PC[0][0], SF_EARTH_R_NG, SF_EARTH_R_NBUD,
                                 r_bud, SF_EARTH_R_BUD_16, SF_EARTH_R_BUD_3);
    jd = jd - sf_wrap(sf_sun_apparent_longitude_budget(
                                jd, l1, SF_DEF_NUT, b1, r1)
                            - longitude) / sf_solar_rate(jd);
    if (!sf_event_date_ok(jd)) return NAN;

    /* 阶段 2：调用方指定的档 */
    const double value = sf_sun_apparent_longitude_budget(jd, l_bud, SF_DEF_NUT,
                                                          b_bud, r_bud);
    const double rate = sf_solar_rate(jd);
    if (!isfinite(value) || !isfinite(rate) || rate <= 0.0) return NAN;
    double step = sf_wrap(value - longitude) / rate;
    jd -= step;
    if (!sf_event_date_ok(jd)) return NAN;
#if SF_EXTRA_STAGE
    step = sf_wrap(sf_sun_apparent_longitude_budget(jd, l_bud, SF_DEF_NUT,
                                                    b_bud, r_bud) - longitude) / rate;
    jd -= step;
    if (!sf_event_date_ok(jd)) return NAN;
#endif

    /* 护栏：最后一步就是"修正前还差多少"。正常时它是 ~1e-10 天。 */
    if (fabs(step) > SF_EVENT_FALLBACK_STEP_DAYS) {
        sf_solar_ctx_t ctx;
        double fixed;
        ctx.l_bud = l_bud; ctx.b_bud = b_bud; ctx.r_bud = r_bud;
        if (!sf_safeguarded(sf_eval_solar, &ctx, longitude, seed,
                            SF_SOLAR_HALF0, SF_SOLAR_HALFMAX, &fixed))
            return NAN;
        if (!sf_event_date_ok(fixed)) return NAN;
        if (path) *path = SF_PATH_GUARD;
        return fixed;
    }

    if (path) *path = SF_PATH_FAST;
    return jd;
}

double sf_solar_term_time_budget(double longitude_rad, int l_bud, int b_bud, int r_bud)
{
    return sf_solar_term_time_budget_path(longitude_rad, l_bud, b_bud, r_bud, NULL);
}

double sf_solar_term_time(double longitude_rad)
{
    return sf_solar_term_time_budget(longitude_rad, SF_DEF_L_BUD,
                                     SF_DEF_B_BUD, SF_DEF_R_BUD);
}

/* 朔的解算核心，**档可传**。第一、二段（粗定位 + 中档）固定不动 —— 它们是
 * 定位用的，代价小；**只有第三段（决定残差的那一段）和兜底吃调用方的档**，
 * 这也是「往下调精度就是截断」的唯一旋钮。
 * 传 SF_DEF_* 就是原来的行为，逐位不变。 */
static double sf_lunar_phase_time_budget_path(double elongation_rad,
                                              const sf_lunar_ctx_t *bud, int *path)
{
    const double elongation = elongation_rad;
    const double daily_motion = 7771.37714500204 / SF_CENTURY;
    const double seed = sf_event_angle_seed(elongation, 1);
    double jd = seed;
    if (path) *path = SF_PATH_FAIL;
    if (!sf_event_date_ok(jd)) return NAN;

    /* 阶段 1：月 8 项 / 地 44 项 / 月纬 0 / 地纬 0 / 半径 3 */
    jd = jd - sf_wrap(sf_elongation_budget(jd, SF_MOON_L_BUD_7, SF_EARTH_L_BUD_48,
                                                 SF_MOON_B_BUD_3, SF_EARTH_B_BUD_3,
                                                 SF_EARTH_R_BUD_16)
                            - elongation) / daily_motion;
    if (!sf_event_date_ok(jd)) return NAN;

    /* 速度只算一次，后两段复用 */
    const double velocity = sf_elongation_rate(jd);

    /* 阶段 2：月 33 / 地 120 / 月纬 10 / 地纬 0 / 半径 3 */
    jd = jd - sf_wrap(sf_elongation_budget(jd, SF_MOON_L_BUD_32, SF_EARTH_L_BUD_48,
                                                 SF_MOON_B_BUD_FULL, SF_EARTH_B_BUD_3,
                                                 SF_EARTH_R_BUD_16)
                            - elongation) / velocity;
    if (!sf_event_date_ok(jd)) return NAN;

    /* 阶段 3：调用方指定的档。这一段的值就是护栏要看的残差 —— 最后一步正是
     * "修正前还差多少"，所以护栏不需要额外求值。
     * 注意：除数仍然是被复用的 velocity（10 项斜率），不是兜底用的 40 项版本，
     * 否则主路径的结果就不再与 fl_qishuo.c 逐位一致了。 */
    const double value = sf_elongation_budget(jd, bud->moon_l, bud->earth_l,
                                              bud->moon_b, bud->earth_b,
                                              bud->earth_r);
    if (!isfinite(value) || !isfinite(velocity) || velocity <= 0.0) return NAN;
    double step = sf_wrap(value - elongation) / velocity;
    jd -= step;
    if (!sf_event_date_ok(jd)) return NAN;
#if SF_EXTRA_STAGE
    step = sf_wrap(sf_elongation_budget(jd, bud->moon_l, bud->earth_l,
                                        bud->moon_b, bud->earth_b,
                                        bud->earth_r) - elongation) / velocity;
    jd -= step;
    if (!sf_event_date_ok(jd)) return NAN;
#endif

    /* 护栏：同太阳。正常时 step 是 ~1e-10 天，0.05 天离它有六个数量级。 */
    if (fabs(step) > SF_EVENT_FALLBACK_STEP_DAYS) {
        double fixed;
        if (!sf_safeguarded(sf_eval_lunar, (void *)bud, elongation, seed,
                            SF_LUNAR_HALF0, SF_LUNAR_HALFMAX, &fixed))
            return NAN;
        if (!sf_event_date_ok(fixed)) return NAN;
        if (path) *path = SF_PATH_GUARD;
        return fixed;
    }

    if (path) *path = SF_PATH_FAST;
    return jd;
}

/* 默认档（= 上游 fast 档的预算）。 */
static const sf_lunar_ctx_t SF_LUNAR_DEF = {
    SF_MOON_L_BUD_FULL, SF_DEF_L_BUD, SF_MOON_B_BUD_FULL, SF_DEF_B_BUD, SF_DEF_R_BUD
};

double sf_lunar_phase_time_path(double elongation_rad, int *path)
{
    return sf_lunar_phase_time_budget_path(elongation_rad, &SF_LUNAR_DEF, path);
}

double sf_lunar_phase_time_budget(double elongation_rad, int moon_l, int earth_l,
                                  int moon_b, int earth_b, int earth_r)
{
    const sf_lunar_ctx_t bud = { moon_l, earth_l, moon_b, earth_b, earth_r };
    return sf_lunar_phase_time_budget_path(elongation_rad, &bud, NULL);
}

double sf_lunar_phase_time(double elongation_rad)
{
    return sf_lunar_phase_time_budget_path(elongation_rad, &SF_LUNAR_DEF, NULL);
}

/* 周期选择：把目标角展开到离 near_jd_tt 最近的那一圈。
 * `lbud` 只有 lunar 分支用（朔的档），NULL = 默认档。 */
static double sf_solve_tier(double target_angle, double near_jd_tt, int lunar,
                           int l_bud, int b_bud, int r_bud,
                           const sf_lunar_ctx_t *lbud, int low)
{
    if (!isfinite(target_angle) || !isfinite(near_jd_tt)) return NAN;
    if (!sf_event_date_ok(near_jd_tt)) return NAN;
    if (!lbud) lbud = &SF_LUNAR_DEF;

    const double target = sf_wrap_radians(target_angle);
    const double t = (near_jd_tt - SF_J2000) / SF_CENTURY;
    const double mean = lunar ? 7771.37714500204 * t - 1.08472
                              : 1.75347 + M_PI + 628.3319653318 * t;

    const double angle = target + SF_TWO_PI *
        sf_js_round((mean - target) / SF_TWO_PI);

    double result = lunar
        ? (low ? sf_lunar_phase_time_low(angle)
               : sf_lunar_phase_time_budget_path(angle, lbud, NULL))
        : (low ? sf_solar_term_time_low(angle)
               : sf_solar_term_time_budget(angle, l_bud, b_bud, r_bud));
    if (!isfinite(result)) return NAN;

    /* 半个周期边界上均值圈数可能选错邻居；往请求日期那一侧探一次。
     * 阈值 12 天（朔）/ 170 天（气）都远小于各自周期的一半。
     *
     * 注意：这只是"错一圈"的补救，不是"种子离真解半个周期"的补救：
     * 那种情形下当前这一圈和隔壁一圈一样远，下面的 < 由中心差（~0.3 天）
     * 决定倒向哪边，同一批调用里会时而取前一个、时而取后一个。
     * 所以调用方必须把 near_jd_tt 喂在真解附近（远小于半周期），否则结果会在
     * 两个邻居之间静默跳。 */
    const double distance = near_jd_tt - result;
    const double guard = lunar ? 12.0 : 170.0;
    if (fabs(distance) > guard) {
        const double step = (distance > 0.0) ? SF_TWO_PI : -SF_TWO_PI;
        const double adjacent = lunar
            ? (low ? sf_lunar_phase_time_low(angle + step)
                   : sf_lunar_phase_time_budget_path(angle + step, lbud, NULL))
            : (low ? sf_solar_term_time_low(angle + step)
                   : sf_solar_term_time_budget(angle + step, l_bud, b_bud, r_bud));
        if (isfinite(adjacent) && fabs(adjacent - near_jd_tt) < fabs(distance))
            result = adjacent;
    }
    return result;
}

double sf_solve_solar_term_budget(double target_longitude_rad, double near_jd_tt,
                                  int l_bud, int b_bud, int r_bud)
{
    return sf_solve_tier(target_longitude_rad, near_jd_tt, 0,
                         l_bud, b_bud, r_bud, NULL, 0);
}

double sf_solve_solar_term(double target_longitude_rad, double near_jd_tt)
{
    return sf_solve_tier(target_longitude_rad, near_jd_tt, 0,
                         SF_DEF_L_BUD, SF_DEF_B_BUD, SF_DEF_R_BUD, NULL, 0);
}

double sf_solve_lunar_phase(double target_elongation_rad, double near_jd_tt)
{
    return sf_solve_tier(target_elongation_rad, near_jd_tt, 1, 0, 0, 0,
                         &SF_LUNAR_DEF, 0);
}

double sf_solve_solar_term_low(double target_longitude_rad, double near_jd_tt)
{
    return sf_solve_tier(target_longitude_rad, near_jd_tt, 0,
                         SF_DEF_L_BUD, SF_DEF_B_BUD, SF_DEF_R_BUD, NULL, 1);
}

double sf_solve_lunar_phase_low(double target_elongation_rad, double near_jd_tt)
{
    return sf_solve_tier(target_elongation_rad, near_jd_tt, 1, 0, 0, 0,
                         &SF_LUNAR_DEF, 1);
}

double sf_solve_lunar_phase_budget(double target_elongation_rad, double near_jd_tt,
                                   int moon_l, int earth_l, int moon_b,
                                   int earth_b, int earth_r)
{
    const sf_lunar_ctx_t bud = { moon_l, earth_l, moon_b, earth_b, earth_r };
    return sf_solve_tier(target_elongation_rad, near_jd_tt, 1, 0, 0, 0, &bud, 0);
}

/* ==================================================================
 * 按公历年汇总事件的入口在 sf_qishuo.c（sf_qishuo_year_run）。
 *
 * sf_qishuo 照 JS 的 getQiShuoYear 逐条复刻，窗口即公历年，并有 C↔JS 的
 * 逐事件对拍（离线探针 qishuo_diff.py，未随库发布）。
 *
 * 不得以「线性种子的历元年编号」代替公历年：那种编号既非 1 月 1 日–12 月
 * 31 日，也可能因目标角错误而使解出的时刻相位错位。
 * ================================================================== */
