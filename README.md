# solar-fast

[English](#english) · [中文](#中文)

## English

`solar-fast` is a portable C99 library designed for embedded devices such as
the ESP32. It calculates solar and lunar positions, rise/set times, solar terms,
lunar phases, the Chinese calendar, and Ganzhi and related traditional
astrological calculations. It uses no dynamic allocation. Its series evaluator
combines Q31 phase arithmetic with a sine lookup table to reduce software
`double` work on microcontrollers such as the ESP32-S3.

The model's tested long-range window is approximately 6000 BCE–10000 CE
(J2000 ±8000 years). Calls outside that window are extrapolations; the range
is not a hard API cutoff or a precision guarantee.

Planned: add position calculations for the other planets and Pluto. These are
not part of the current public API.

### Build and use

```sh
make check
```

This runs the host-side unit and calendar tests. The optional DE441 truth CSV
is not distributed; without it, `make check` reports that portion as skipped.
For the independent fixed-J2000 comparison, install `jplephem` and `numpy`,
provide your own DE441 kernel, and run:

```sh
python3 test/compare_j2000_de441.py --bsp /path/to/de441.bsp \
  --start-year -1000 --end-year 1000 --step-days 32
```

Include `solar_fast.h` and compile the sources listed in [Makefile](Makefile)
with a C99 compiler and `libm`. For ESP-IDF, see the source list in
[the benchmark component](test/esp32_bench/main/CMakeLists.txt).

```c
#include "solar_fast.h"
#include <stdio.h>

int main(void) {
    double jd_ut = sf_julian_day_ut(2024, 2, 10, 12, 30, 0);
    sf_sun_observed_t sun;
    if (sf_sun_observed(jd_ut, 39.9042, 116.4074,
                        50.0, 1013.25, 15.0, &sun) != 0)
        return 1;
    printf("Zenith: %.4f deg, azimuth: %.4f deg\n",
           sun.zenith_deg, sun.azimuth_deg);
    return 0;
}
```

`sf_sun_observed` accepts a UT Julian day, latitude north-positive and
longitude east-positive. Other APIs may require TT; check each declaration.
The calendar's structural day boundary is determined by its configured
calendar meridian, not by the display time zone.

### ESP32-S3 measurements

Measured on an ESP32-S3 rev 0.2 at 240 MHz, ESP-IDF v5.5.5, GCC performance
optimization, on 2026-09-22. The two builds differ only in cache size:
default 16 KB instruction / 32 KB data, or larger 32/64 KB. Values are the
minimum per-call time after warm-up, reported by the on-device `esp_timer`
harness. The SPA pairs use the same instant, observer, build and cache setting
on both sides. Their computation scopes are close but not identical; see the
[benchmark method](test/esp32_bench/README.md). NREL reference source is a
local-only optional input and is not distributed with this library.

The timings below do not all use one accuracy mode. The annual qi/shuo table
uses the default **HIGH** event preset with 24 solar terms plus new/full moons
(phase angles 0° and 180°). Rise/set uses the default **FAST** path; `qi_low`
and `shuo_low` are separate coarse event solvers, not HIGH timings. HIGH is the
highest event preset, but not every coefficient axis is fully evaluated: its
Earth-distance budget is 192 of 475 terms. The PMO comparison below also uses
HIGH, but includes all four principal lunar phases, so it is a different
workload from the annual-table benchmark.

| Cache I/D | Operation | NREL SPA | solar-fast | Ratio |
|---|---|---:|---:|---:|
| 16/32 KB | Geocentric apparent Sun RA/Dec/distance | 4.145 ms | 1.390 ms | 2.98× |
| 16/32 KB | Topocentric solar position with refraction | 4.633 ms | 2.497 ms | 1.86× |
| 32/64 KB | Geocentric apparent Sun RA/Dec/distance | 4.138 ms | 0.762 ms | 5.43× |
| 32/64 KB | Topocentric solar position with refraction | 4.421 ms | 1.357 ms | 3.26× |

| Operation | Default 16/32 KB | Larger 32/64 KB |
|---|---:|---:|
| `sf_sun_rise_set` FAST | 46.707 ms | 28.786 ms |
| `sf_moon_rise_set` FAST | 46.796 ms | 29.367 ms |
| `sf_qishuo_year_run` HIGH | 313.251 ms | 213.678 ms |
| `qi_low` | 0.496 ms | 0.469 ms |
| `shuo_low` | 0.240 ms | 0.240 ms |

The earlier 28–29 ms sunrise/sunset number belongs to the larger-cache build,
not the default build. For the full method, see the
[benchmark README](test/esp32_bench/README.md).

### DE441 precision check

The standalone checker compares geometric Earth and Moon L/B/R in a **fixed
J2000 ecliptic** frame. It applies the engine's constant planetary native-axis
matrix and the ELP theory's own lunar P/Q conversion; it does not add of-date
precession, nutation, aberration, light time, or a fitted epoch offset. Both
sides use the same numeric Julian day. Angular errors are in arcseconds and
distance errors in kilometres.

| Sampling | Earth L RMS / max | Earth B RMS / max | Earth R full RMS / max | Moon L RMS / max | Moon B RMS / max | Moon R RMS / max |
|---|---:|---:|---:|---:|---:|---:|
| J2000 ±1000 yr; 32 d; 22,830 points | 0.0147″ / 0.0829″ | 0.0271″ / 0.1107″ | 2.76 / 11.54 km | 0.1547″ / 0.9059″ | 0.0835″ / 0.5257″ | 0.145 / 0.804 km |
| J2000 ±8000 yr; 25 yr; 641 points | 0.1186″ / 0.5604″ | 0.0559″ / 0.2571″ | 12.96 / 64.60 km | 1.1947″ / 7.4441″ | 0.3383″ / 1.5850″ | 0.627 / 3.341 km |

The 641-point long-range sample is sparse; it does not establish a whole-range
maximum. `R full` uses all Earth distance terms. The normal 192-term Earth R
budget has a 15.16 km RMS / 76.42 km maximum in the ±1000-year sample.

For a published-calendar check, see the [2026 PMO solar-term and lunar-phase
comparison](#pmo-2026) below. All 24 solar terms and 50 principal lunar phases
match the observatory's published Beijing-time minute after rounding.

### License and provenance

The library implementation is under [MPL-2.0](LICENSE). [NOTICE](NOTICE)
identifies the upstream models and historical data; the MPL does not
automatically relicense third-party material. DE441 and the NREL SPA reference
source are not included. The historical-calendar and ELP coefficient sources
identified in `NOTICE` do not carry an explicit conventional software license;
review their rights before redistribution or commercial use.

## 中文

`solar-fast` 是专为 ESP32 等嵌入式设备设计的可移植 C99 库，无动态内存分配，
提供日月位置、日月出没、节气月相、中国农历和八字计算。级数求值采用
Q31 相位运算和正弦查表，减少 ESP32-S3 等无硬件双精度浮点 MCU 上的
`double` 运算。

已测试的长期窗口约为公元前 6000 年至公元 10000 年（J2000 ±8000 年）。
窗口外仍可外推，但不保证精度；这不是 API 的硬截止范围。

后续计划接入其他行星及冥王星的位置计算；当前公开 API 尚不提供这些功能。

### 构建与调用

运行 `make check` 可执行宿主机单元与历法测试。仓库不附带 DE441 真值 CSV；
缺少它时，对应检查会明确跳过。独立的固定 J2000 精度检查需要自备
DE441 内核并安装 `jplephem`、`numpy`，命令见英文部分。

公共头文件是 `solar_fast.h`；C99 源文件和 `libm` 链接方式见
[Makefile](Makefile)，ESP-IDF 源文件列表见[基准工程配置](test/esp32_bench/main/CMakeLists.txt)。
上面的示例展示了以 UT 儒略日计算北京的太阳站心位置。经度东正西负；
其他接口可能接收 TT，请以各函数声明为准。农历的结构日界按所配置的
**历法经度**确定，不跟随显示时区。

### ESP32-S3 真机性能

2026-09-22 在 ESP32-S3 rev 0.2、240 MHz、ESP-IDF v5.5.5、GCC 性能优化下，
分别用默认 I/D cache 16/32 KB 和大 cache 32/64 KB 上板重测。上表是板上
`esp_timer` 预热后的单次最小耗时。SPA 双方统一了时刻、观测地点、构建
与 cache 配置，但两套接口的计算范围仍有小差别，不能称为“100% 同口径”。
NREL SPA 源码仅作本机可选基准输入，不随本库分发。

下表并非所有项目都使用同一种精度路径：整年气朔表采用默认 **HIGH** 档，
计算 24 节气及朔、望（月相角 0°、180°）；日月出没采用默认 **FAST** 路径；
`qi_low`、`shuo_low` 是单列的粗定位算法，不是 HIGH 耗时。HIGH 是事件计算的
最高预设档，但并非每条系数轴都全量求值，例如地球距离轴使用 475 项中的
192 项。文末紫金山对照也使用 HIGH，但包含朔、上弦、望、下弦四种月相，
因此与这里的整年表不是同一工作量。

默认 cache 下，地心太阳视位置为 4.145 对 1.390 ms（2.98 倍），
站心太阳位置为 4.633 对 2.497 ms（1.86 倍）；大 cache 下分别为
5.43 倍和 3.26 倍。默认 cache 的太阳 FAST 出没为 46.707 ms，
大 cache 为 28.786 ms；旧文档把大 cache 数字写成默认结果，现已纠正。
其余结果和测试方法见上表及
[基准工程说明](test/esp32_bench/README.md)。

### DE441 精度

上表比较双方在**固定 J2000 黄道**下的地球和月球几何 L/B/R。
地球仅应用常量坐标轴矩阵，月球应用 ELP 理论自带的 P/Q 转换；
不叠加日期岁差、章动、光行差、光行时或历元常数拟合。
角度误差单位为角秒，距离误差单位为公里。±8000 年的 641 点样本较稀疏，
不能据此宣称全区间最坏误差。

另见文末的[2026 年紫金山天文台节气与月相逐项对照](#pmo-2026)：
本库结果四舍五入到北京时间的分钟后，24 个节气和 50 个主要月相全部一致。

### 许可与来源

本库实现采用 [MPL-2.0](LICENSE)。[NOTICE](NOTICE) 说明上游理论和历史
数据的来源；MPL 不会自动为第三方材料重新授权。仓库不附带 DE441 或
NREL SPA 参考源码。`NOTICE` 所列历史历法数据与 ELP 系数源文件缺少
明确的常规软件许可，公开再分发或商用前应单独核对相关权利。

<a id="pmo-2026"></a>

### 2026 PMO calendar check / 2026 年紫金山天文台历书对照

The [Purple Mountain Observatory 2026 calendar](https://pmo.cas.cn/xwdt2019/kpdt2019/202203/P020251230620718707826.pdf) publishes Beijing-time event times to the minute. With `sf_qishuo_year_run` at the default HIGH accuracy, UTC+8 display time, and phase angles 0°/90°/180°/270°, `solar-fast` matches the published minute after rounding for **24/24 solar terms** and **50/50 lunar phases**. These printed minutes are not second-level truth data.

紫金山天文台[《二○二六年日历资料》](https://pmo.cas.cn/xwdt2019/kpdt2019/202203/P020251230620718707826.pdf)公布到分钟。本库使用 `sf_qishuo_year_run` 默认 HIGH 档、UTC+8 显示时区，月相角设为 0°、90°、180°、270°；计算时刻四舍五入到分钟后，**24/24 节气、50/50 月相**与公布值一致。公布的分钟不能当成秒级真值。下表秒值为本库计算结果。

#### Solar terms / 二十四节气

| 节气 | 紫金山天文台（UTC+8） | solar-fast（UTC+8） |
|---|---|---|
| 小寒 | 2026-01-05 16:23 | 2026-01-05 16:23:09.704 |
| 大寒 | 2026-01-20 09:45 | 2026-01-20 09:44:55.798 |
| 立春 | 2026-02-04 04:02 | 2026-02-04 04:02:08.139 |
| 雨水 | 2026-02-18 23:52 | 2026-02-18 23:51:55.329 |
| 惊蛰 | 2026-03-05 21:59 | 2026-03-05 21:58:59.458 |
| 春分 | 2026-03-20 22:46 | 2026-03-20 22:45:57.391 |
| 清明 | 2026-04-05 02:40 | 2026-04-05 02:39:59.646 |
| 谷雨 | 2026-04-20 09:39 | 2026-04-20 09:39:06.350 |
| 立夏 | 2026-05-05 19:49 | 2026-05-05 19:48:43.854 |
| 小满 | 2026-05-21 08:37 | 2026-05-21 08:36:44.191 |
| 芒种 | 2026-06-05 23:48 | 2026-06-05 23:48:22.123 |
| 夏至 | 2026-06-21 16:25 | 2026-06-21 16:24:30.548 |
| 小暑 | 2026-07-07 09:57 | 2026-07-07 09:56:57.444 |
| 大暑 | 2026-07-23 03:13 | 2026-07-23 03:13:05.117 |
| 立秋 | 2026-08-07 19:43 | 2026-08-07 19:42:44.396 |
| 处暑 | 2026-08-23 10:19 | 2026-08-23 10:18:48.454 |
| 白露 | 2026-09-07 22:41 | 2026-09-07 22:41:17.035 |
| 秋分 | 2026-09-23 08:05 | 2026-09-23 08:05:13.217 |
| 寒露 | 2026-10-08 14:29 | 2026-10-08 14:29:17.144 |
| 霜降 | 2026-10-23 17:38 | 2026-10-23 17:37:56.166 |
| 立冬 | 2026-11-07 17:52 | 2026-11-07 17:52:03.896 |
| 小雪 | 2026-11-22 15:23 | 2026-11-22 15:23:20.617 |
| 大雪 | 2026-12-07 10:53 | 2026-12-07 10:52:31.280 |
| 冬至 | 2026-12-22 04:50 | 2026-12-22 04:50:14.321 |

#### Lunar phases / 朔望两弦

| 月相 | 紫金山天文台（UTC+8） | solar-fast（UTC+8） |
|---|---|---|
| 望 | 2026-01-03 18:03 | 2026-01-03 18:02:54.844 |
| 下弦 | 2026-01-10 23:48 | 2026-01-10 23:48:23.504 |
| 朔 | 2026-01-19 03:52 | 2026-01-19 03:51:58.913 |
| 上弦 | 2026-01-26 12:47 | 2026-01-26 12:47:23.592 |
| 望 | 2026-02-02 06:09 | 2026-02-02 06:09:14.942 |
| 下弦 | 2026-02-09 20:43 | 2026-02-09 20:43:06.284 |
| 朔 | 2026-02-17 20:01 | 2026-02-17 20:01:09.049 |
| 上弦 | 2026-02-24 20:28 | 2026-02-24 20:27:36.803 |
| 望 | 2026-03-03 19:38 | 2026-03-03 19:37:53.752 |
| 下弦 | 2026-03-11 17:39 | 2026-03-11 17:38:30.712 |
| 朔 | 2026-03-19 09:23 | 2026-03-19 09:23:28.829 |
| 上弦 | 2026-03-26 03:18 | 2026-03-26 03:17:42.809 |
| 望 | 2026-04-02 10:12 | 2026-04-02 10:11:57.567 |
| 下弦 | 2026-04-10 12:52 | 2026-04-10 12:51:38.810 |
| 朔 | 2026-04-17 19:52 | 2026-04-17 19:51:48.443 |
| 上弦 | 2026-04-24 10:32 | 2026-04-24 10:31:45.378 |
| 望 | 2026-05-02 01:23 | 2026-05-02 01:23:10.480 |
| 下弦 | 2026-05-10 05:10 | 2026-05-10 05:10:27.832 |
| 朔 | 2026-05-17 04:01 | 2026-05-17 04:01:02.996 |
| 上弦 | 2026-05-23 19:11 | 2026-05-23 19:10:57.328 |
| 望 | 2026-05-31 16:45 | 2026-05-31 16:45:12.305 |
| 下弦 | 2026-06-08 18:01 | 2026-06-08 18:00:31.435 |
| 朔 | 2026-06-15 10:54 | 2026-06-15 10:54:09.958 |
| 上弦 | 2026-06-22 05:55 | 2026-06-22 05:55:24.503 |
| 望 | 2026-06-30 07:57 | 2026-06-30 07:56:40.981 |
| 下弦 | 2026-07-08 03:29 | 2026-07-08 03:28:59.505 |
| 朔 | 2026-07-14 17:44 | 2026-07-14 17:43:36.659 |
| 上弦 | 2026-07-21 19:06 | 2026-07-21 19:05:36.207 |
| 望 | 2026-07-29 22:36 | 2026-07-29 22:35:43.415 |
| 下弦 | 2026-08-06 10:21 | 2026-08-06 10:21:29.505 |
| 朔 | 2026-08-13 01:37 | 2026-08-13 01:36:44.930 |
| 上弦 | 2026-08-20 10:46 | 2026-08-20 10:46:20.756 |
| 望 | 2026-08-28 12:19 | 2026-08-28 12:18:32.052 |
| 下弦 | 2026-09-04 15:51 | 2026-09-04 15:51:13.783 |
| 朔 | 2026-09-11 11:27 | 2026-09-11 11:26:59.951 |
| 上弦 | 2026-09-19 04:44 | 2026-09-19 04:43:46.788 |
| 望 | 2026-09-27 00:49 | 2026-09-27 00:49:02.592 |
| 下弦 | 2026-10-03 21:25 | 2026-10-03 21:25:03.698 |
| 朔 | 2026-10-10 23:50 | 2026-10-10 23:50:05.028 |
| 上弦 | 2026-10-19 00:13 | 2026-10-19 00:12:41.088 |
| 望 | 2026-10-26 12:12 | 2026-10-26 12:11:48.581 |
| 下弦 | 2026-11-02 04:28 | 2026-11-02 04:28:26.769 |
| 朔 | 2026-11-09 15:02 | 2026-11-09 15:02:06.923 |
| 上弦 | 2026-11-17 19:48 | 2026-11-17 19:47:49.421 |
| 望 | 2026-11-24 22:54 | 2026-11-24 22:53:33.688 |
| 下弦 | 2026-12-01 14:09 | 2026-12-01 14:08:39.897 |
| 朔 | 2026-12-09 08:52 | 2026-12-09 08:51:51.100 |
| 上弦 | 2026-12-17 13:43 | 2026-12-17 13:42:39.954 |
| 望 | 2026-12-24 09:28 | 2026-12-24 09:28:14.335 |
| 下弦 | 2026-12-31 02:59 | 2026-12-31 02:59:29.867 |
