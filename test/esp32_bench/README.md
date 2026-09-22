# ESP32-S3 benchmark / ESP32-S3 真机基准

This is a core-library benchmark for an ESP32-S3 at 240 MHz with ESP-IDF
v5.5.5 and performance optimization. It runs without the handheld UI. The
serial header reports the actual chip, clock, IDF version, and instruction/data
cache sizes; keep those settings with any quoted measurement.

本工程在 ESP32-S3 上测量核心库，不需要界面仓库。默认配置为 240 MHz、
ESP-IDF v5.5.5 和性能优化。引用耗时时请同时注明串口表头中的 CPU 主频
与指令/数据 cache 容量。

## Run / 运行

```sh
cd test/esp32_bench
. /path/to/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbserial-140 flash monitor
```

Replace the port and ESP-IDF path for your machine. The optional 32 KB
instruction / 64 KB data-cache build uses a separate build directory and
configuration file, so it does not modify the default build:

请替换成本机 ESP-IDF 路径和串口。以下大 cache 变体使用独立构建目录，
不会覆盖默认构建配置：

```sh
idf.py -B build-bigcache -DSDKCONFIG=build-bigcache/sdkconfig \
  -DSDKCONFIG_DEFAULTS='sdkconfig.defaults;sdkconfig.bigcache' build
idf.py -B build-bigcache -p /dev/cu.usbserial-140 flash monitor
```

The NREL SPA implementation is **not distributed** with this repository: its
source notice restricts redistribution. If you have the right to use a local
copy, place `spa.c` and `spa.h` in `main/spa/` before configuring the build.
The benchmark automatically enables its SPA section when both files are
present; otherwise the core-only benchmark still builds. Do not commit or
redistribute those files without permission.

本仓库**不分发** NREL SPA 源码，其原文件声明限制再分发。若你有权在本机
使用参考源码，可将 `spa.c` 和 `spa.h` 放入 `main/spa/` 后重新配置构建；
两文件存在时自动启用 SPA 对比，否则仍可正常编译核心库测试。

## Method / 方法

The harness uses `esp_timer_get_time()`, warms each operation, then runs five
rounds with an automatically chosen number of calls per round. It prints the
minimum, median, and maximum per-call wall time. Timer-loop overhead is
**included** (the empty-operation control is about 0.06 µs); no host-side
timing or screen/SPI transfer is involved.

程序用板上的 `esp_timer_get_time()` 计时；每项先预热，再测五轮，
每轮次数自动选择。表格列出单次最小值、中位数和最大值。数字**包含**
约 0.06 µs 的计时循环开销，不包含电脑端或屏幕 SPI 传输耗时。

For the SPA comparison, both sides use 2023-02-25 00:00 UT and the same
Beijing observer inputs. The geocentric pair computes apparent solar RA/Dec
and distance; the SPA routine also computes sidereal time internally. The
topocentric pair includes parallax, elevation, refraction, zenith, and azimuth;
`solar-fast` additionally computes equation of time. Thus these are closely
matched *paths*, not identical instruction sets or a proof of equal accuracy.

SPA 双方使用同一时刻（2023-02-25 00:00 UT）和同一北京观测参数。
地心路径都输出太阳视赤经、视赤纬和距离，但 SPA 内部还算恒星时；
站心路径都含视差、海拔、蒙气差、天顶角和方位角，而本库另算均时差。
因此这是相近功能路径的耗时对比，不是逐条指令相同或精度证明。

## Results / 结果

Corrected-input runs on 2026-09-22 (ESP32-S3 rev 0.2, 240 MHz; minimum per
call). Each SPA pair was measured in the same build:

2026-09-22 修正输入时刻后，在默认与大 cache 两档上板复测。每组 SPA
双方都来自同一次构建（单次最小值）：

| Cache I/D | Operation / 操作 | NREL SPA | solar-fast | Ratio / 倍率 |
|---|---|---:|---:|---:|
| 16/32 KB | Geocentric apparent Sun / 地心太阳视位置 | 4.145 ms | 1.390 ms | 2.98× |
| 16/32 KB | Topocentric Sun / 站心太阳位置 | 4.633 ms | 2.497 ms | 1.86× |
| 32/64 KB | Geocentric apparent Sun / 地心太阳视位置 | 4.138 ms | 0.762 ms | 5.43× |
| 32/64 KB | Topocentric Sun / 站心太阳位置 | 4.421 ms | 1.357 ms | 3.26× |

| Operation / 操作 | 16/32 KB | 32/64 KB |
|---|---:|---:|
| Sunrise/sunset FAST / 日出日落 FAST | 46.707 ms | 28.786 ms |
| Annual qi/shuo HIGH / 整年气朔表 HIGH | 313.251 ms | 213.678 ms |

An earlier same-source cache A/B capture measured sunrise/sunset FAST at
47.595 ms with 16/32 KB cache and 28.738 ms with 32/64 KB cache. The old
28–29 ms result must not be labelled as the default-cache result. The
`results/` captures are historical runs from earlier revisions and may include
UI measurements; the current core-only build omits the UI section.

早期同版本 A/B 记录表明，FAST 日出日落在默认 16/32 KB cache 下为
47.595 ms，在大 32/64 KB cache 下为 28.738 ms。旧文档把后者写成
默认构建结果是不对的。`results/` 中的原始串口记录来自较早版本，
部分带 UI 测量；当前核心库构建不包含 UI 项目。
