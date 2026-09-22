# solar-fast —— 纯 C99 构建，不依赖 ESP-IDF
#
#   make            构建测试程序
#   make check      跑全部核心与历法验证
#   make clean
#
# ------------------------------------------------------------------
# 可选的外部依赖（真值与历史基线测试使用）：
#   BASE            含 fl_qishuo.c 的目录（可选第 1 层基线）
#   SF_TRUTH_CSV    DE441 真值 CSV（可选第 2-3 层基准）
# ------------------------------------------------------------------

CC      ?= cc
CSTD    ?= -std=c99
OPT     ?= -O2
WARN    := -Wall -Wextra

CFLAGS  := $(CSTD) $(OPT) $(WARN) -D_GNU_SOURCE -I. -Iinclude -Icore -Irise -Iqishuo -Icalendar -Ibazi
LDFLAGS := -lm

SF_LUT_N ?= 4096
CFLAGS  += -DSF_LUT_N=$(SF_LUT_N)

BASE         ?=
SF_TRUTH_CSV ?= test/truth.csv
export TAIYIN_LITE ?= ../taiyin-lite/src/

TRUTH_DEF = -DSF_TRUTH_CSV='"$(SF_TRUTH_CSV)"'

SRC := core/sf_fixed.c core/sf_series.c core/sf_solve.c \
       calendar/sf_calendar.c calendar/sf_ganzhi.c \
       rise/sf_rise.c qishuo/sf_qishuo.c \
       bazi/sf_bazi_rules.c bazi/sf_bazi.c
OBJ := $(SRC:.c=.o)

HDRS := include/solar_fast.h core/solar_fast.h core/sf_fixed.h core/sf_data.h core/sf_lut.h \
        core/sf_internal.h calendar/sf_calendar.h calendar/sf_calendar_data.h \
        rise/sf_rise.h qishuo/sf_qishuo.h bazi/sf_bazi_rules.h bazi/sf_bazi.h

TESTBIN := test/sf_check
CALBIN  := test/sf_calendar_check
BAZIBIN := test/sf_bazi_check

all: $(TESTBIN) $(CALBIN) $(BAZIBIN)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

$(TESTBIN): test/sf_check.c $(OBJ)
	$(CC) $(CFLAGS) $(TRUTH_DEF) -o $@ test/sf_check.c $(OBJ) $(LDFLAGS)

$(CALBIN): test/sf_calendar_check.c $(OBJ)
	$(CC) $(CFLAGS) -o $@ test/sf_calendar_check.c $(OBJ) $(LDFLAGS)

$(BAZIBIN): test/sf_bazi_check.c $(OBJ)
	$(CC) $(CFLAGS) -o $@ test/sf_bazi_check.c $(OBJ) $(LDFLAGS)

ifneq ($(BASE),)
test/sf_check_vs_base: test/sf_check.c $(OBJ) $(BASE)/fl_qishuo.c
	$(CC) $(CFLAGS) $(TRUTH_DEF) -DSF_HAVE_BASE -I$(BASE) -o $@ \
	    test/sf_check.c $(OBJ) $(BASE)/fl_qishuo.c $(LDFLAGS)
endif

check: $(TESTBIN) $(CALBIN) $(BAZIBIN)
	@echo "=== 1) 单点逐位核对（J2000）==="
	./$(TESTBIN) raw 2451545.0
	@echo
	@echo "=== 1b) 低项数气朔：范围外逐位退回完整算法 ==="
	./$(TESTBIN) low
	@echo
	@echo "=== 2) 对 fl_qishuo.c 基线的增量（第 1 层）==="
	@$(MAKE) --no-print-directory test/sf_check_vs_base
	@test -x test/sf_check_vs_base && ./test/sf_check_vs_base pos 2415020 2488070 20001 || true
	@echo
	@echo "=== 3) 对 DE441 真值（第 2 层）==="
	@./$(TESTBIN) truth 3001 || echo "  （读不到 $(SF_TRUTH_CSV)，跳过）"
	@echo
	@echo "=== 4) 农历（对 taiyin-lite 的 JS 测试 fixture）==="
	@./$(CALBIN)
	@echo
	@echo "=== 5) 八字规则层（对 bazi-lite 的 JS 测试 fixture）==="
	@./$(BAZIBIN)

clean:
	rm -f $(OBJ) $(TESTBIN) $(CALBIN) $(BAZIBIN) test/sf_check_vs_base

.PHONY: all check clean test/sf_check_vs_base
