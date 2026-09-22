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

### 许可与来源

本库实现采用 [MPL-2.0](LICENSE)。[NOTICE](NOTICE) 说明上游理论和历史
数据的来源；MPL 不会自动为第三方材料重新授权。仓库不附带 DE441 或
NREL SPA 参考源码。`NOTICE` 所列历史历法数据与 ELP 系数源文件缺少
明确的常规软件许可，公开再分发或商用前应单独核对相关权利。
