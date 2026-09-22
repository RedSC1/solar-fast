# ESP-IDF 工程：在**真芯片**上量这个库的绝对耗时。
#
#   . ~/esp/esp-idf-5.5.5/export.sh
#   idf.py set-target esp32s3
#   idf.py -p /dev/cu.usbserial-140 flash monitor
#
# 结果的读法见同目录 README.md。**注意 sdkconfig.defaults 里那两行**：
# 主频和编译优化级别都会让数字说谎（S3 的 IDF 默认是 160 MHz + -Og）。
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sf_esp32_bench)
