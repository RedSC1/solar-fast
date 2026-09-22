/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_bazi_check.c —— 八字的验收程序
 *
 * 断言全部来自 taiyin-lite/packages/bazi/test/bazi-core.test.mjs。
 * 那份测试对照的是 taiyin-ephemeris 的 C++ 实现（其 charts fixture 名为
 * `charts-cpp.json`），与本库同口径移植 —— 这些期望值是三方共同的口径。
 *
 *   ./test/sf_bazi_check              全部
 *   ./test/sf_bazi_check primitives   只跑穷举那几节
 *   ./test/sf_bazi_check pins         只跑 fixture 钉住的那几条
 *   ./test/sf_bazi_check chart        只跑装配层 / 流运
 *   ./test/sf_bazi_check qiyun        只跑起运 / 大运
 *
 * 注意：这里**不是**移植正确性的主力判据，两件事分清楚：
 *   · 这个程序管的是**定义域本身** —— 十神 100 / 长生 240 / 空亡 60 全扫，
 *     以及上游 fixture 钉住的那几个点。价值是**不依赖 node**，随时能跑。
 *   · 与 JS 的**逐字节对拍**见离线探针（未随库发布）experimental/bazi_diff.py：
 *       --stage rules   32046 次调用零容差
 *       --stage fixture 上游 charts-cpp.json 1024 行 × 8 项逐字一致
 *       --stage qi      出生时刻网格，结构逐字比 + 时刻按实测定容差
 * 起运那一节的**时分秒不设死**（受 C 自己节气精度影响，被 ×120 放大），
 * 只钉民历日 —— 容差怎么来的见离线探针 bazi_diff.py（未随库发布）里 TOL_* 的注释。
 * ================================================================== */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sf_bazi.h"
#include "sf_bazi_rules.h"

static int g_pass, g_fail;
static int g_only = 0;      /* 0 全部，1 primitives，2 pins，3 chart，4 qiyun */

#define OK(cond, ...) do {                                    \
    if (cond) { g_pass++; }                                   \
    else { g_fail++; printf("  ✗ %s:%d  ", __FILE__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); }               \
} while (0)

/* ------------------------------------------------------------------
 * 一、定义域穷举
 *
 * 这几节的项数与上游 primitives-cpp.json 里本库移植的那 412 行**同构**
 * （tenGod 100 / hiddenStems 12 / lifeStage 240 / kongWang 60）。
 * 上游那份是拿 C++ 生成的；这里手写同构的穷举，对拍脚本另外做逐字节比。
 * ------------------------------------------------------------------ */

/* 十神的期望值：日干见目标干。这张表是**手工列的**，不是从 C 实现抄的 ——
 * 它按「同我/我生/我克/克我/生我」五组、每组再分阴阳排，可以照着数出来。
 * 行 = 日干（甲…癸），列 = 目标干（甲…癸）。 */
static const uint8_t kTenGodExpected[10][10] = {
    /* 甲日 */ { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 },
    /* 乙日 */ { 1, 0, 3, 2, 5, 4, 7, 6, 9, 8 },
    /* 丙日 */ { 8, 9, 0, 1, 2, 3, 4, 5, 6, 7 },
    /* 丁日 */ { 9, 8, 1, 0, 3, 2, 5, 4, 7, 6 },
    /* 戊日 */ { 6, 7, 8, 9, 0, 1, 2, 3, 4, 5 },
    /* 己日 */ { 7, 6, 9, 8, 1, 0, 3, 2, 5, 4 },
    /* 庚日 */ { 4, 5, 6, 7, 8, 9, 0, 1, 2, 3 },
    /* 辛日 */ { 5, 4, 7, 6, 9, 8, 1, 0, 3, 2 },
    /* 壬日 */ { 2, 3, 4, 5, 6, 7, 8, 9, 0, 1 },
    /* 癸日 */ { 3, 2, 5, 4, 7, 6, 9, 8, 1, 0 },
};

/* 藏干期望：每支 1..3 个干，本气首位。个数单独一张表 —— **不能用 0 当
 * 终止符，甲(0) 是合法天干**。 */
static const uint8_t kHiddenExpected[12][3] = {
    { 9, 0, 0 }, { 5, 9, 7 }, { 0, 2, 4 }, { 1, 0, 0 }, { 4, 1, 9 }, { 2, 6, 4 },
    { 3, 5, 0 }, { 5, 3, 1 }, { 6, 8, 4 }, { 7, 0, 0 }, { 4, 7, 3 }, { 8, 0, 0 },
};
static const uint8_t kHiddenCountExpected[12] = { 1, 3, 3, 1, 3, 3, 2, 3, 3, 1, 3, 2 };

static void run_primitives(void)
{
    printf("== 定义域穷举 ==\n");

    /* --- 十神 100 项 --- */
    {
        int bad = 0;
        for (int d = 0; d < 10; d++) {
            for (int t = 0; t < 10; t++) {
                uint8_t r = 0xff;
                if (sf_bazi_ten_god((uint8_t)d, (uint8_t)t, &r) != 0
                    || r != kTenGodExpected[d][t]) {
                    if (bad++ < 3)
                        printf("  ✗ 十神 日干%d 见 %d = %d，期望 %d\n",
                               d, t, r, kTenGodExpected[d][t]);
                }
            }
        }
        OK(bad == 0, "十神 100 项有 %d 项不符", bad);
    }

    /* --- 藏干 12 项 --- */
    {
        int bad = 0;
        for (int b = 0; b < 12; b++) {
            uint8_t r[SF_BAZI_HIDDEN_STEM_CAPACITY];
            int cnt = -1;
            if (sf_bazi_hidden_stems((uint8_t)b, r, &cnt) != 0
                || cnt != kHiddenCountExpected[b]
                || memcmp(r, kHiddenExpected[b], 3) != 0) {
                if (bad++ < 3)
                    printf("  ✗ 藏干 %d = [%d,%d,%d]×%d，期望 [%d,%d,%d]×%d\n",
                           b, r[0], r[1], r[2], cnt,
                           kHiddenExpected[b][0], kHiddenExpected[b][1],
                           kHiddenExpected[b][2], kHiddenCountExpected[b]);
            }
        }
        OK(bad == 0, "藏干 12 项有 %d 项不符", bad);
    }

    /* --- 十二长生 240 项（两种寄宫全扫）---
     * 期望值按 rules.ts:165 的式子现算 —— 但**式子与表都是这里独立写的**：
     * 起点表照抄三命通会那张（甲起亥），极性分支是符号翻转。
     * 这不像十神那张手写表那么独立，所以它的主力判据仍是对拍。 */
    {
        static const int start_fire[10]  = { 11, 6, 2, 9, 2, 9, 5, 0, 8, 3 };
        int bad = 0;
        for (int pal = 0; pal < 2; pal++) {
            for (int s = 0; s < 10; s++) {
                for (int b = 0; b < 12; b++) {
                    int start = start_fire[s];
                    if (pal == SF_BAZI_EARTH_PALACE_WATER_EARTH) {
                        if (s == 4) start = 8;
                        if (s == 5) start = 3;
                    }
                    const int want = ((s & 1) == 0)
                        ? (b + 12 - start) % 12 : (start + 12 - b) % 12;
                    uint8_t r = 0xff;
                    if (sf_bazi_life_stage((uint8_t)s, (uint8_t)b, pal, &r) != 0
                        || r != want) {
                        if (bad++ < 3)
                            printf("  ✗ 长生 干%d 支%d 寄宫%d = %d，期望 %d\n",
                                   s, b, pal, r, want);
                    }
                }
            }
        }
        OK(bad == 0, "十二长生 240 项有 %d 项不符", bad);
    }

    /* --- 空亡 60 项：只查**结构不变量** ---
     * 具体值由 fixture 钉住（见下节），这里扫的是两条必须处处成立的：
     *   · 两个支在十二支环上**相邻** —— 注意是「相邻」不是「升序相邻」：
     *     阴干那一支是降序输出的（first 恒偶数支，阴干排在前面），
     *     所以差值在环上是 +1 **或** +11 都算对。
     *   · 同旬的十柱集合相同 —— 一旬 = **连续的 10 个甲子序号**
     *     （0-9 是甲子旬、10-19 是甲戌旬……），所以拿 i 跟**旬首**
     *     比，不是跟 i−10 比（那是相邻的上一旬，旬空本来就不同）。 */
    {
        int bad = 0;
        for (int i = 0; i < 60; i++) {
            const sf_ganzhi v = (sf_ganzhi)(((i % 10) << 4) | (i % 12));
            uint8_t k[2];
            if (sf_bazi_kong_wang(v, k) != 0) { bad++; continue; }
            const int step = ((int)k[1] - (int)k[0] + 12) % 12;
            if (step != 1 && step != 11) {
                if (bad++ < 3) printf("  ✗ 空亡 %02x 两支不相邻：%d,%d\n", v, k[0], k[1]);
            }
            uint8_t head[2];
            const int h = (i / 10) * 10;
            sf_bazi_kong_wang((sf_ganzhi)(((h % 10) << 4) | (h % 12)), head);
            if (!((head[0] == k[0] && head[1] == k[1])
                  || (head[0] == k[1] && head[1] == k[0]))) {
                if (bad++ < 3)
                    printf("  ✗ 空亡 同旬不同集合：旬首%02x=[%d,%d] %02x=[%d,%d]\n",
                           (unsigned)(((h % 10) << 4) | (h % 12)), head[0], head[1],
                           v, k[0], k[1]);
            }
        }
        OK(bad == 0, "空亡 60 项有 %d 项不符", bad);
    }
}

/* ------------------------------------------------------------------
 * 二、上游 fixture 钉住的点
 * ------------------------------------------------------------------ */
static void run_pins(void)
{
    printf("\n== 上游 fixture 钉住的点 ==\n");

    /* 空亡：**顺序是有信息的**。同一个旬里甲子与乙丑的旬空集合相同而
     * 顺序相反（阳干升序、阴干降序）—— 不得改为排序。 */
    {
        static const struct { sf_ganzhi v; uint8_t k0, k1; } fx[] = {
            { 0x00, 10, 11 },   /* 甲子 */
            { 0x0a,  8,  9 },   /* 甲戌 */
            { 0x08,  6,  7 },   /* 甲申 */
            { 0x11, 11, 10 },   /* 乙丑 —— 集合同甲子、顺序相反 */
        };
        for (unsigned i = 0; i < sizeof fx / sizeof fx[0]; i++) {
            uint8_t k[2] = { 0xff, 0xff };
            OK(sf_bazi_kong_wang(fx[i].v, k) == 0
               && k[0] == fx[i].k0 && k[1] == fx[i].k1,
               "空亡 %02x = [%d,%d]，期望 [%d,%d]", fx[i].v, k[0], k[1],
               fx[i].k0, fx[i].k1);
        }
    }

    /* 四柱外四柱。bazi-core.test.mjs:165 钉的那一组 */
    {
        const sf_ganzhi_pillars p = { 0x26, 0x62, 0x42, 0x35 };
        sf_ganzhi e[SF_BAZI_EXTRA_PILLAR_COUNT] = { 0 };
        OK(sf_bazi_extra_pillars(&p, e) == 0, "四外柱求解失败");
        OK(e[0] == 0x4a, "命宫 = %02x，期望 4a", e[0]);
        OK(e[1] == 0x28, "身宫 = %02x，期望 28", e[1]);
        OK(e[2] == 0x75, "胎元 = %02x，期望 75", e[2]);
        OK(e[3] == 0x9b, "胎息 = %02x，期望 9b", e[3]);
    }

    /* 大运顺逆：阳年男 / 阴年女 顺行 */
    {
        int32_t d = 0;
        OK(sf_bazi_luck_direction(0x04, SF_BAZI_GENDER_MALE, &d) == 0 && d == 1,
           "甲辰年男应顺行(+1)，得到 %d", d);
        OK(sf_bazi_luck_direction(0x04, SF_BAZI_GENDER_FEMALE, &d) == 0 && d == -1,
           "甲辰年女应逆行(-1)，得到 %d", d);
        OK(sf_bazi_luck_direction(0x15, SF_BAZI_GENDER_MALE, &d) == 0 && d == -1,
           "乙巳年男应逆行(-1)，得到 %d", d);
        OK(sf_bazi_luck_direction(0x15, SF_BAZI_GENDER_FEMALE, &d) == 0 && d == 1,
           "乙巳年女应顺行(+1)，得到 %d", d);
    }

    /* 名字表 */
    {
        OK(!strcmp(sf_bazi_ten_god_name(SF_BAZI_TEN_GOD_BI_JIAN), "比肩"), "十神名[0]");
        OK(!strcmp(sf_bazi_ten_god_name(SF_BAZI_TEN_GOD_ZHENG_YIN), "正印"), "十神名[9]");
        OK(!strcmp(sf_bazi_life_stage_name(SF_BAZI_LIFE_STAGE_DI_WANG), "帝旺"), "长生名[4]");
        OK(!strcmp(sf_bazi_life_stage_name(SF_BAZI_LIFE_STAGE_YANG), "养"), "长生名[11]");
        OK(sf_bazi_ten_god_name(10) == NULL, "十神名越界应返回 NULL");
        OK(sf_bazi_life_stage_name(12) == NULL, "长生名越界应返回 NULL");
    }

    /* 五行 / 阴阳（非 JS 对齐面的派生量，见 sf_bazi_rules.h 头注）。
     * 判据是**自洽**：名字表的顺序必须与元素 id 对得上。 */
    {
        static const char *const want[5] = { "水", "木", "金", "土", "火" };
        for (int i = 0; i < 5; i++)
            OK(!strcmp(sf_bazi_wuxing_name((uint8_t)i), want[i]),
               "五行名[%d] = %s，期望 %s", i, sf_bazi_wuxing_name((uint8_t)i), want[i]);

        /* 天干本气：甲乙木 丙丁火 戊己土 庚辛金 壬癸水 */
        static const uint8_t want_stem[10] = {
            SF_WUXING_WOOD, SF_WUXING_WOOD, SF_WUXING_FIRE, SF_WUXING_FIRE,
            SF_WUXING_EARTH, SF_WUXING_EARTH, SF_WUXING_METAL, SF_WUXING_METAL,
            SF_WUXING_WATER, SF_WUXING_WATER,
        };
        int bad = 0;
        for (int s = 0; s < 10; s++) {
            uint8_t e = 0xff;
            if (sf_bazi_stem_element((uint8_t)s, &e) != 0 || e != want_stem[s]) bad++;
        }
        OK(bad == 0, "天干本气五行有 %d 项不符", bad);

        /* 地支本气：寅卯木 巳午火 辰戌丑未土 申酉金 亥子水 */
        static const uint8_t want_branch[12] = {
            SF_WUXING_WATER, SF_WUXING_EARTH, SF_WUXING_WOOD, SF_WUXING_WOOD,
            SF_WUXING_EARTH, SF_WUXING_FIRE, SF_WUXING_FIRE, SF_WUXING_EARTH,
            SF_WUXING_METAL, SF_WUXING_METAL, SF_WUXING_EARTH, SF_WUXING_WATER,
        };
        bad = 0;
        for (int b = 0; b < 12; b++) {
            uint8_t e = 0xff;
            if (sf_bazi_branch_element((uint8_t)b, &e) != 0 || e != want_branch[b]) {
                if (bad++ < 3)
                    printf("  ✗ 地支%d 五行 = %d，期望 %d\n", b, e, want_branch[b]);
            }
        }
        OK(bad == 0, "地支本气五行有 %d 项不符", bad);

        int y = 0;
        OK(sf_bazi_stem_is_yang(0, &y) == 0 && y == 1, "甲应为阳");
        OK(sf_bazi_stem_is_yang(1, &y) == 0 && y == 0, "乙应为阴");
        OK(sf_bazi_branch_is_yang(0, &y) == 0 && y == 1, "子应为阳");
        OK(sf_bazi_branch_is_yang(1, &y) == 0 && y == 0, "丑应为阴");
    }

    /* 纳音名 ↔ 纳音五行。
     *
     * 一条断言同时守住**两张表**：名字表的顺序（30 项有没有错位）与五行表
     * （kNayin 那张 60 项的表有没有读错）。名字末字必须是它的五行字 ——
     * 上游那串五行是 `金火木土金火水土金木水土火木水` 重复两遍，实测核对过。
     * 注意：名字本身是**非 JS 对齐面**（上游零命中），所以这条查的是**自洽**，
     * 不是「与上游一致」—— 但那正是它能守住错位的原因。 */
    {
        static const char kElemChar[5][4] = {
            "水", "木", "金", "土", "火"     /* 索引对齐 SF_WUXING_* */
        };
        int bad = 0;
        for (uint8_t id = 0; id < 30; id++) {
            const char *nm = sf_ganzhi_nayin_name(id);
            if (!nm || strlen(nm) != 9) {
                printf("  ✗ 纳音名[%d] 不是三个汉字：%s\n", id, nm ? nm : "(NULL)");
                bad++; continue;
            }
            /* 该编号对应的甲子：六十甲子两两一组 */
            const sf_ganzhi v = (sf_ganzhi)((((id * 2) % 10) << 4) | ((id * 2) % 12));
            uint8_t el = 0xff;
            if (sf_ganzhi_nayin_element(v, &el) != 0) { bad++; continue; }
            if (strcmp(nm + 6, kElemChar[el]) != 0) {   /* 末字 = 最后 3 字节 */
                if (bad++ < 5)
                    printf("  ✗ 纳音名[%d]「%s」末字 %s，但五行表说是 %s\n",
                           id, nm, nm + 6, kElemChar[el]);
            }
        }
        OK(bad == 0, "纳音名与五行表有 %d 项对不上", bad);
        OK(sf_ganzhi_nayin_name(30) == NULL, "纳音名越界应返回 NULL");
    }

    /* 非法输入：越界一律回负码，不返回半个结果 */
    {
        uint8_t r = 0; int cnt = 0; int32_t d = 0; int y = 0;
        uint8_t k[2]; sf_ganzhi e[4];
        uint8_t hs[SF_BAZI_HIDDEN_STEM_CAPACITY];
        OK(sf_bazi_ten_god(10, 0, &r) != 0, "日干 10 应被拒");
        OK(sf_bazi_ten_god(0, 10, &r) != 0, "目标干 10 应被拒");
        OK(sf_bazi_hidden_stems(12, hs, &cnt) != 0, "地支 12 应被拒");
        OK(sf_bazi_life_stage(0, 12, 0, &r) != 0, "长生：支 12 应被拒");
        OK(sf_bazi_life_stage(0, 0, 2, &r) != 0, "长生：寄宫 2 应被拒");
        OK(sf_bazi_kong_wang(255, k) != 0, "空亡：0xff 应被拒");
        OK(sf_bazi_kong_wang(0x10, k) != 0, "空亡：奇偶不符应被拒");
        OK(sf_bazi_luck_direction(0x26, 7, &d) != 0, "顺逆：性别 7 应被拒");
        OK(sf_bazi_luck_direction(0x26, SF_BAZI_GENDER_NONE, &d) != 0,
           "顺逆：性别未给(-1)应被拒");
        OK(sf_bazi_stem_element(10, &r) != 0, "天干五行：10 应被拒");
        OK(sf_bazi_branch_is_yang(12, &y) != 0, "地支阴阳：12 应被拒");
        const sf_ganzhi_pillars bad_p = { 0x10, 0x62, 0x42, 0x35 };
        OK(sf_bazi_extra_pillars(&bad_p, e) != 0, "四外柱：非法年柱应被拒");
        OK(sf_bazi_extra_pillars(NULL, e) != 0, "四外柱：空指针应被拒");
        OK(sf_bazi_ten_god(0, 0, NULL) != 0, "十神：空指针应被拒");
    }
}

/* ------------------------------------------------------------------
 * 三、装配层与流运（bazi-core.test.mjs:163 起）
 * ------------------------------------------------------------------ */
static void run_chart(void)
{
    printf("\n== 装配层 / 流运（上游钉住的点）==\n");

    /* 一组固定的四柱，下面几条都用它 */
    const sf_ganzhi_pillars p = { 0x26, 0x62, 0x42, 0x35 };

    {
        sf_bazi_chart ch;
        OK(sf_bazi_analyze(&p, SF_BAZI_EARTH_PALACE_FIRE_EARTH, &ch) == SF_BAZI_OK,
           "analyzePillars 失败");
        OK(ch.day_master == 4, "日主 = %d，期望 4（日柱 0x42 的天干）", ch.day_master);

        /* 注意：日柱的 visibleTenGod **恒为 0（比肩）**，上游测试钉的就是这个。
         * 「日柱显示日主」是调用方的显示约定，不是算法 —— 库不管这件事。 */
        OK(ch.columns[2].visible_ten_god == 0,
           "日柱 visibleTenGod = %d，期望 0（比肩）", ch.columns[2].visible_ten_god);

        const uint8_t hs[3] = { 0, 2, 4 };
        OK(ch.columns[2].n_hidden == 3
           && !memcmp(ch.columns[2].hidden_stems, hs, 3),
           "日柱藏干（寅）= [%d,%d,%d]×%d，期望 [0,2,4]×3",
           ch.columns[2].hidden_stems[0], ch.columns[2].hidden_stems[1],
           ch.columns[2].hidden_stems[2], ch.columns[2].n_hidden);

        static const uint8_t want_ls[4] = { 4, 0, 0, 3 };
        static const uint8_t want_ny[4] = { 21, 13, 7, 26 };
        int bad = 0;
        for (int i = 0; i < 4; i++) {
            if (ch.columns[i].life_stage != want_ls[i]) {
                printf("  ✗ 第%d列长生 = %d，期望 %d\n", i, ch.columns[i].life_stage, want_ls[i]);
                bad++;
            }
            if (ch.columns[i].nayin_id != want_ny[i]) {
                printf("  ✗ 第%d列纳音 = %d，期望 %d\n", i, ch.columns[i].nayin_id, want_ny[i]);
                bad++;
            }
        }
        OK(bad == 0, "四列的长生/纳音有 %d 项不符", bad);

        /* 年柱 = 0x26，干的支的都验一下解码没串位 */
        OK(ch.columns[0].stem == 2 && ch.columns[0].branch == 6 && ch.columns[0].index == 42,
           "年柱解码 = 干%d 支%d 序%d，期望 2/6/42",
           ch.columns[0].stem, ch.columns[0].branch, ch.columns[0].index);

        /* 藏干十神：寅的本气甲 丙 戊，对日主乙（1）依次是 劫财 伤官 正财 = 1/3/5…
         * 上游给的是 [6,8,0]。**补位那几格不是数据**（见 n_hidden）。 */
        static const uint8_t want_htg[3] = { 6, 8, 0 };
        OK(ch.columns[2].n_hidden == 3
           && !memcmp(ch.columns[2].hidden_ten_gods, want_htg, 3),
           "日柱藏干十神 = [%d,%d,%d]，期望 [6,8,0]",
           ch.columns[2].hidden_ten_gods[0], ch.columns[2].hidden_ten_gods[1],
           ch.columns[2].hidden_ten_gods[2]);
    }

    /* 流年：对齐甲子 = 1984（与 sf_ganzhi_year_of 恒等） */
    {
        sf_ganzhi g = 0xff;
        OK(sf_bazi_flow_year(1984, &g) == SF_BAZI_OK && g == 0x00,
           "流年 1984 = %02x，期望 00（甲子）", g);
        OK(sf_bazi_flow_year(2024, &g) == SF_BAZI_OK && g == 0x04,
           "流年 2024 = %02x，期望 04（甲辰）", g);
        OK(sf_bazi_flow_year(4, &g) == SF_BAZI_OK && g == 0x00,
           "流年 4 = %02x，期望 00", g);
    }

    /* 小运：基数是**时柱**；大运柱：基数是**月柱** */
    {
        sf_ganzhi g = 0;
        OK(sf_bazi_xiao_yun(p.hour, 1, 1, &g) == SF_BAZI_OK,
           "小运求解失败");
        OK(sf_bazi_xiao_yun(p.hour, 1, 0, &g) != 0, "小运 age=0 应被拒");
        OK(sf_bazi_xiao_yun(p.hour, 0, 5, &g) != 0, "小运 direction=0 应被拒");

        sf_bazi_dayun_pillar dy[8];
        OK(sf_bazi_da_yun_pillars(p.month, 1, 3, 1, dy, 8) == SF_BAZI_OK,
           "大运柱求解失败");
        OK(dy[0].pillar == 0x73 && dy[1].pillar == 0x84 && dy[2].pillar == 0x95,
           "前三步大运柱 = %02x/%02x/%02x，期望 73/84/95",
           dy[0].pillar, dy[1].pillar, dy[2].pillar);
        OK(dy[0].index == 1 && dy[0].start_virtual_age == 1 && dy[0].end_virtual_age == 10,
           "第 1 步 index=%d 虚岁 %d..%d，期望 1 / 1..10",
           dy[0].index, dy[0].start_virtual_age, dy[0].end_virtual_age);
        OK(dy[2].start_virtual_age == 21 && dy[2].end_virtual_age == 30,
           "第 3 步虚岁 %d..%d，期望 21..30",
           dy[2].start_virtual_age, dy[2].end_virtual_age);

        /* capacity 不足：**一条都不写**（与 sf_qishuo 的「写满 + 报总数」相反） */
        dy[0].pillar = 0xabu;
        OK(sf_bazi_da_yun_pillars(p.month, 1, 8, 1, dy, 3) == SF_BAZI_ERR_CAPACITY,
           "capacity 不足应返回 ERR_CAPACITY");
        OK(dy[0].pillar == 0xabu, "capacity 不足时不该写出任何一条");
        /* count = 0 是合法的空表 */
        OK(sf_bazi_da_yun_pillars(p.month, 1, 0, 1, dy, 8) == SF_BAZI_OK,
           "count=0 应成功（空表）");
    }

    /* 流月 / 流日 / 流时（跟流年同一层，纯整数）
     *
     * 注意：流月这一段的**关键**是那个 `(branch+10)%12` 转换：底下的
     * sf_ganzhi_get_month 收的是**月序**（0=寅），而上游 calculateFlowMonth
     * 先拿月支转一次。所以下面特意钉了**非寅月**的月支（子月 / 巳月）——
     * 只钉寅月的话，漏掉那个转换一样全绿（寅月恰好 branch==2 → monthIndex==0）。 */
    {
        sf_ganzhi g = 0xff;
        /* 甲子年 + 寅月支 → 五虎遁「甲己之年丙作首」→ 丙寅 */
        OK(sf_bazi_flow_month(0x00, 2, &g) == SF_BAZI_OK && g == 0x22,
           "流月 甲子年 寅月 = %02x，期望 22（丙寅）", g);
        /* 甲子年 + 子月支 → monthIndex = 10 → 丙子 */
        OK(sf_bazi_flow_month(0x00, 0, &g) == SF_BAZI_OK && g == 0x20,
           "流月 甲子年 子月 = %02x，期望 20（丙子）—— 漏掉 (branch+10)%%12 这一处就错",
           g);
        /* 甲子年 + 亥月支 → monthIndex = 9 → 乙亥 */
        OK(sf_bazi_flow_month(0x00, 11, &g) == SF_BAZI_OK && g == 0x1b,
           "流月 甲子年 亥月 = %02x，期望 1b（乙亥）", g);
        /* 丙寅年（0x22）+ 巳月支 → monthIndex = 3 → 起干 ((2%5)*2+2)%10 = 6
         * → 干 (6+3)%10 = 9 = 癸，支 (3+2)%12 = 5 = 巳 → 癸巳 */
        OK(sf_bazi_flow_month(0x22, 5, &g) == SF_BAZI_OK && g == 0x95,
           "流月 丙寅年 巳月 = %02x，期望 95（癸巳）", g);
        OK(sf_bazi_flow_month(0x00, 12, &g) != 0, "月支 12 应被拒");
        OK(sf_bazi_flow_month(0x00, 2, NULL) != 0, "流月空指针应被拒");
    }
    {
        /* 流日：就是 sf_ganzhi_day_pillar 的壳，钉两个已知日期 */
        sf_cal_datetime cd;
        sf_ganzhi g = 0xff;
        cd.year = 2000; cd.month = 1; cd.day = 1; cd.hour = 0;
        cd.minute = 0; cd.second = 0.0;
        OK(sf_bazi_flow_day(&cd, &g) == SF_BAZI_OK && g == 0x46,
           "流日 2000-01-01 = %02x，期望 46（戊午）", g);
        cd.year = 2024; cd.month = 2; cd.day = 29;
        OK(sf_bazi_flow_day(&cd, &g) == SF_BAZI_OK && g == 0x9b,
           "流日 2024-02-29 = %02x，期望 9b（癸亥）", g);
        OK(sf_bazi_flow_day(NULL, &g) != 0, "流日空指针应被拒");
    }
    {
        /* 流时：五鼠遁「甲己还加甲」—— 基数 = **流日柱天干** */
        sf_ganzhi g = 0xff;
        OK(sf_bazi_flow_hour(0x00, 0, &g) == SF_BAZI_OK && g == 0x00,
           "流时 甲子日 子时 = %02x，期望 00（甲子）", g);
        OK(sf_bazi_flow_hour(0x11, 0, &g) == SF_BAZI_OK && g == 0x20,
           "流时 乙丑日 子时 = %02x，期望 20（丙子）", g);
        OK(sf_bazi_flow_hour(0x22, 0, &g) == SF_BAZI_OK && g == 0x40,
           "流时 丙寅日 子时 = %02x，期望 40（戊子）", g);
        OK(sf_bazi_flow_hour(0x00, 12, &g) != 0, "时支序 12 应被拒");
        OK(sf_bazi_flow_hour(0x00, 0, NULL) != 0, "流时空指针应被拒");
    }

    /* 非法输入 */
    {
        sf_bazi_chart ch;
        const sf_ganzhi_pillars bad_p = { 0x10, 0x62, 0x42, 0x35 };
        OK(sf_bazi_analyze(&bad_p, 0, &ch) != 0, "奇偶不符的年柱应被拒");
        OK(sf_bazi_analyze(&p, 2, &ch) != 0, "寄宫 2 应被拒");
        OK(sf_bazi_analyze(NULL, 0, &ch) != 0, "空指针应被拒");

        OK(sf_bazi_flow_year(2024, NULL) != 0, "流年空指针应被拒");
        sf_bazi_dayun_pillar dy[8];
        OK(sf_bazi_da_yun_pillars(p.month, 1, -1, 1, dy, 8) != 0, "count<0 应被拒");
        OK(sf_bazi_da_yun_pillars(p.month, 2, 8, 1, dy, 8) != 0, "direction=2 应被拒");
    }

    /* 选项默认值：gender 必须是 -1（未给），不能是 0（女） */
    {
        sf_bazi_options o;
        sf_bazi_options_init(&o);
        OK(o.gender == SF_BAZI_GENDER_NONE,
           "sf_bazi_options_init 的 gender = %d，期望 -1（未给）—— "
           "写成 0 就是「默认女命」", o.gender);
        OK(o.earth_palace_mode == SF_BAZI_EARTH_PALACE_FIRE_EARTH, "默认火土同宫");
        OK(o.qiyun_time_model == SF_BAZI_QIYUN_TRADITIONAL_CALENDAR, "默认传统折算");
        OK(o.dayun_boundary_model == SF_BAZI_DAYUN_CIVIL_YEARS, "默认民用整年");
        OK(o.dayun_count == SF_BAZI_DEFAULT_DAYUN_COUNT, "默认 8 步");
        OK(o.rat_hour_mode == SF_GANZHI_RAT_HOUR_NO_SPLIT, "默认 23:00 换日");
    }
}

/* ------------------------------------------------------------------
 * 四、起运与大运（bazi-core.test.mjs:226 起）
 *
 * 上游那一组：出生 2026-02-19 23:28 +480、四柱 {0x26,0x62,0x11,0x20}、男命、
 * 默认模型（传统折算 + 民用整年）。**注意那组四柱是直接喂进去的**，
 * 不是从出生时刻解出来的 —— 所以这里也不解，用同一组。
 * ------------------------------------------------------------------ */
static void run_qiyun(void)
{
    printf("\n== 起运 / 大运（上游钉住的点）==\n");

    const sf_ganzhi_pillars p = { 0x26, 0x62, 0x11, 0x20 };
    const sf_cal_datetime bt = { 2026, 2, 19, 23, 28, 0.0 };

    sf_bazi_options o;
    sf_bazi_options_init(&o);
    o.gender = SF_BAZI_GENDER_MALE;

    const double jd_utc = sf_julian_day_ut(bt.year, bt.month, bt.day,
                                           bt.hour, bt.minute, bt.second)
                        - 480.0 / 1440.0;
    const double jd_ut1 = sf_utc_to_ut1(jd_utc);

    sf_bazi_qiyun q;
    OK(sf_bazi_qi_yun(&o, jd_ut1, &bt, p.year, &q) == SF_BAZI_OK, "起运求解失败");
    OK(q.direction == 1, "顺逆 = %d，期望 1（阳年男顺行）", q.direction);
    OK(q.jie_interval_days > 13.0 && q.jie_interval_days < 15.0,
       "节间隔 = %.6f 天，期望落在 (13,15)", q.jie_interval_days);

    /* 注意：交运的**民历日**是硬断言；时分秒不设死 —— 它受 C 自己节气精度
     * （对 DE441 rms 0.218 s）的影响，被 ×120 放大后能差几十秒。 */
    OK(q.start_civil_time.year == 2030 && q.start_civil_time.month == 10
       && q.start_civil_time.day == 12,
       "交运民历日 = %d-%d-%d，期望 2030-10-12",
       q.start_civil_time.year, q.start_civil_time.month, q.start_civil_time.day);
    OK(q.trad_years == 4 && q.trad_months == 7 && q.trad_days == 22,
       "三日折一年 = %d年%d月%d日，期望 4/7/22",
       q.trad_years, q.trad_months, q.trad_days);

    {
        static sf_bazi_dayun_entry dy[8];
        OK(sf_bazi_da_yun(&o, &bt, p.month, &q, dy, 8) == SF_BAZI_OK,
           "大运求解失败");
        static const uint8_t want_p[8] = { 0x73, 0x84, 0x95, 0x06,
                                           0x17, 0x28, 0x39, 0x4a };
        static const int32_t want_age[8] = { 5, 15, 25, 35, 45, 55, 65, 75 };
        int bad = 0;
        for (int i = 0; i < 8; i++) {
            if (dy[i].pillar != want_p[i]) {
                printf("  ✗ 第%d步大运柱 = %02x，期望 %02x\n", i + 1, dy[i].pillar, want_p[i]);
                bad++;
            }
            if (dy[i].start_virtual_age != want_age[i]) {
                printf("  ✗ 第%d步起始虚岁 = %d，期望 %d\n",
                       i + 1, dy[i].start_virtual_age, want_age[i]);
                bad++;
            }
        }
        OK(bad == 0, "大运柱/虚岁有 %d 项不符", bad);

        /* 注意：虚岁跨度恒 9，而 end_civil_time 是下一个十年边界 —— 两者
         * **故意不同步**。这条就是守它的。 */
        bad = 0;
        for (int i = 0; i < 8; i++)
            if (dy[i].end_virtual_age - dy[i].start_virtual_age != 9) bad++;
        OK(bad == 0, "虚岁跨度有 %d 步不是 9", bad);
        OK(dy[0].end_civil_time.year == 2040 && dy[0].start_civil_time.year == 2030,
           "第 1 步民历 %d..%d，期望 2030..2040",
           dy[0].start_civil_time.year, dy[0].end_civil_time.year);
    }

    /* 性别未给必须**明确失败**，不能默认成女命（那会静默排反） */
    {
        sf_bazi_options o2;
        sf_bazi_options_init(&o2);
        sf_bazi_qiyun q2;
        OK(sf_bazi_qi_yun(&o2, jd_ut1, &bt, p.year, &q2) == SF_BAZI_ERR_GENDER,
           "性别未给应返回 ERR_GENDER，得到 %d", sf_bazi_qi_yun(&o2, jd_ut1, &bt, p.year, &q2));
        o2.gender = 7;
        OK(sf_bazi_qi_yun(&o2, jd_ut1, &bt, p.year, &q2) == SF_BAZI_ERR_GENDER,
           "性别 7 应被拒");
    }

    /* 非法出生时刻：1582-10-05..14 那十天不存在，回环校验要挡住 */
    {
        const sf_cal_datetime ghost = { 1582, 10, 10, 12, 0, 0.0 };
        sf_bazi_qiyun q3;
        OK(sf_bazi_qi_yun(&o, jd_ut1, &ghost, p.year, &q3) == SF_BAZI_ERR_CHART_TIME,
           "1582-10-10 是不存在的日期，应返回 ERR_CHART_TIME");
        const sf_cal_datetime bad = { 2026, 2, 30, 12, 0, 0.0 };
        OK(sf_bazi_qi_yun(&o, jd_ut1, &bad, p.year, &q3) == SF_BAZI_ERR_CHART_TIME,
           "2 月 30 日应被拒");
        OK(sf_bazi_qi_yun(&o, jd_ut1, &bt, p.year, NULL) == SF_BAZI_ERR_ARG,
           "空指针应被拒");
        OK(sf_bazi_qi_yun(&o, NAN, &bt, p.year, &q3) == SF_BAZI_ERR_BIRTH_JD,
           "非有限的 birth_jd 应被拒");
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        if (!strcmp(argv[1], "primitives")) g_only = 1;
        else if (!strcmp(argv[1], "pins")) g_only = 2;
        else if (!strcmp(argv[1], "chart")) g_only = 3;
        else if (!strcmp(argv[1], "qiyun")) g_only = 4;
    }

    if (g_only == 0 || g_only == 1) run_primitives();
    if (g_only == 0 || g_only == 2) run_pins();
    if (g_only == 0 || g_only == 3) run_chart();
    if (g_only == 0 || g_only == 4) run_qiyun();

    printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
