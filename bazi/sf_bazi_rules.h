/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_bazi_rules.h —— 八字的**纯规则层**
 *
 * 移植自 taiyin-lite/packages/bazi/src/rules.ts 与 constants.ts
 * （npm 包 `bazi-lite`，基准版本与 sha256 钉在
 * experimental/bazi_oracle.mjs 的 REFERENCE 里；该探针离线、未随库发布）。
 *
 * 注意：这一层零天文、零 config、零 JD。十神、藏干、十二长生、空亡、
 * 四柱外四柱全是纯整数运算 —— 所以它**不链接求解器也能编、能穷举测**。
 * 上游 `primitives-cpp.json` 有 3848 行有限域真值，其中移植的 412 行
 * 零容差；把求解器拖进这条链只会让门槛变高。需要天文的那部分
 * （起运/大运）在 sf_bazi.h。
 *
 * ------------------------------------------------------------------
 * 两个**不是** JS 对齐面的东西（上游没有对应导出）
 *
 * 1. 本模块只要 `rules.ts` 的**前半**：关系（刑冲合害）那半张表没移植。
 *    所以 constants.ts 里 RELATION_KIND / *_RELATION_FLAG / PILLAR_MASK /
 *    RENYUAN_SILING_* 这些都没有对应的 C 常量。
 * 2. **五行本气 / 阴阳**：上游只导出 `WUXING_NAMES`，没有任何
 *    `stemElement` / `branchElement` / 阴阳 函数（整包 grep 零命中）。
 *    本库需要的这几项按下面的派生规则实现，**明确标注为非 JS 对齐面**：
 *
 *      sf_bazi_stem_element(s)    = STEM_ELEMENT[s >> 1]        （甲乙木…壬癸水）
 *      sf_bazi_branch_element(b)  = STEM_ELEMENT[藏干[b][0] >> 1]  ← 用**本气**推
 *      阴阳                       = 奇偶（干、支同理）
 *
 *    地支那条**刻意用本气藏干推**，而不是另写一张 12 项的表：全库就一份
 *    五行定义，不会出现「表和表打架」。这与 shen-sha.ts 里那份
 *    `STEM_ELEMENT` 的值一致（那个文件本库不移植）。
 *
 * 注意：纳音五行 ≠ 干支本气五行，两个不得互相替换：
 *   `sf_ganzhi_nayin_element()` 给的是**纳音**的五行（如「海中金」→ 金），
 *   `sf_bazi_stem_element()` 给的是**天干本身**的五行（庚 → 金）。
 * 前者按六十甲子两两一组，后者按字。屏上给「丙」「午」染色用的是后者。
 * ================================================================== */
#ifndef SF_BAZI_RULES_H
#define SF_BAZI_RULES_H

#include <stdint.h>

#include "sf_calendar.h"     /* sf_ganzhi / sf_ganzhi_pillars / SF_WUXING_* */

#ifdef __cplusplus
extern "C" {
#endif

/* 上游的 INVALID_ID。空亡只有两个地支，用不到它；这个常量是给
 * 「槽位没值」那种场合留的，与 sf_qishuo / sf_calendar 的 0xff 同源。 */
#define SF_BAZI_INVALID_ID 0xffu

/* 藏干容量 = rules.ts:200 的 hiddenStemCapacity。**恒为 3**，不足的补 0，
 * 实际个数由出参给 —— 不得用「有没有 0」判断结束，甲(0) 是合法天干。 */
#define SF_BAZI_HIDDEN_STEM_CAPACITY 3

/* 本命四柱的根数。上游的 analyzePillars 恒出 4 列，不是可变长。 */
#define SF_BAZI_PILLAR_COUNT 4

/* 四柱外四柱的个数，顺序 = 上游 calculateExtraPillars 的声明序 =
 * Object.values(extraPillars) 的实际顺序（已实测核对）。 */
#define SF_BAZI_EXTRA_PILLAR_COUNT 4

/* 槽位。值与上游 PILLAR_SLOT 相同，前四个与 sf_ganzhi_pillars 的字段序一致。 */
enum {
    SF_BAZI_SLOT_YEAR      = 0,
    SF_BAZI_SLOT_MONTH     = 1,
    SF_BAZI_SLOT_DAY       = 2,
    SF_BAZI_SLOT_HOUR      = 3,
    SF_BAZI_SLOT_MING_GONG = 4,
    SF_BAZI_SLOT_SHEN_GONG = 5,
    SF_BAZI_SLOT_TAI_YUAN  = 6,
    SF_BAZI_SLOT_TAI_XI    = 7,
    SF_BAZI_SLOT_COUNT     = 8
};

/* 十神。值与上游 TEN_GOD 逐字相同，**不得重排**（对拍会逐字比）。
 *   BI_JIAN 比肩  JIE_CAI 劫财  SHI_SHEN 食神  SHANG_GUAN 伤官  PIAN_CAI 偏财
 *   ZHENG_CAI 正财  QI_SHA 七杀  ZHENG_GUAN 正官  PIAN_YIN 偏印  ZHENG_YIN 正印 */
enum {
    SF_BAZI_TEN_GOD_BI_JIAN   = 0,
    SF_BAZI_TEN_GOD_JIE_CAI    = 1,
    SF_BAZI_TEN_GOD_SHI_SHEN   = 2,
    SF_BAZI_TEN_GOD_SHANG_GUAN = 3,
    SF_BAZI_TEN_GOD_PIAN_CAI   = 4,
    SF_BAZI_TEN_GOD_ZHENG_CAI  = 5,
    SF_BAZI_TEN_GOD_QI_SHA     = 6,
    SF_BAZI_TEN_GOD_ZHENG_GUAN = 7,
    SF_BAZI_TEN_GOD_PIAN_YIN   = 8,
    SF_BAZI_TEN_GOD_ZHENG_YIN  = 9,
    SF_BAZI_TEN_GOD_COUNT      = 10
};

/* 十二长生（屏上叫「星运」）。值与上游 LIFE_STAGE 相同。 */
enum {
    SF_BAZI_LIFE_STAGE_CHANG_SHENG = 0,   /* 长生 */
    SF_BAZI_LIFE_STAGE_MU_YU       = 1,   /* 沐浴 */
    SF_BAZI_LIFE_STAGE_GUAN_DAI    = 2,   /* 冠带 */
    SF_BAZI_LIFE_STAGE_LIN_GUAN    = 3,   /* 临官 */
    SF_BAZI_LIFE_STAGE_DI_WANG     = 4,   /* 帝旺 */
    SF_BAZI_LIFE_STAGE_SHUAI       = 5,   /* 衰 */
    SF_BAZI_LIFE_STAGE_BING        = 6,   /* 病 */
    SF_BAZI_LIFE_STAGE_SI          = 7,   /* 死 */
    SF_BAZI_LIFE_STAGE_MU          = 8,   /* 墓 */
    SF_BAZI_LIFE_STAGE_JUE         = 9,   /* 绝 */
    SF_BAZI_LIFE_STAGE_TAI         = 10,  /* 胎 */
    SF_BAZI_LIFE_STAGE_YANG        = 11,  /* 养 */
    SF_BAZI_LIFE_STAGE_COUNT       = 12
};

/* 地支寄宫（戊己两颗干跟谁同宫）。值与上游 EARTH_PALACE_MODE 相同。
 *
 * 注意：它**只改戊(stem 4)与己(stem 5)两颗干的十二长生起点**，不是换整张表：
 *   FIRE_EARTH  戊起寅(2)、己起酉(9)——与丙丁同宫，即「火土同宫」，默认
 *   WATER_EARTH 戊起申(8)、己起卯(3)——与壬癸同宫，即「水土同宫」 */
enum {
    SF_BAZI_EARTH_PALACE_FIRE_EARTH  = 0,
    SF_BAZI_EARTH_PALACE_WATER_EARTH = 1
};

/* 性别。**值是上游 GENDER 的（FEMALE=0 / MALE=1）**，但本库多一个 -1
 * 表示「没给」—— 起运必须知道性别（顺逆判据），上游在那里抛异常。
 *
 * 初始化函数将未指定的性别设为 -1；memset 得到的 0 表示女性，
 * 因此不能用全零初始化表示“未指定”。 */
enum {
    SF_BAZI_GENDER_NONE   = -1,
    SF_BAZI_GENDER_FEMALE = 0,
    SF_BAZI_GENDER_MALE   = 1
};

/* ------------------------------------------------------------------
 * 规则查询
 *
 * 全部返回 0 成功、-1 失败（入参越界）。上游在这些地方抛 RangeError，
 * 本库按惯例换负码 —— 判据一样，只是不抛。
 * ------------------------------------------------------------------ */

/* 十神：日干见目标干。day_stem / target_stem ∈ 0..9。
 *
 * 注意：日柱自己算出来是 0（比肩），不是「日主」。上游 grep「日主」零命中，
 * 它的测试钉的就是 0；「日柱显示日主」是**调用方的显示约定**。
 * 所以 UI 层要自己把第 2 列的那个 0 换成「日主」，库不管这件事。 */
int sf_bazi_ten_god(uint8_t day_stem, uint8_t target_stem, uint8_t *out);

/* 藏干：地支 → 1..3 个天干，**本气在首位**。out 恒写满 3 个槽（不足补 0），
 * out_count 给实际个数（1..3）。 */
int sf_bazi_hidden_stems(uint8_t branch,
                         uint8_t out[SF_BAZI_HIDDEN_STEM_CAPACITY],
                         int *out_count);

/* 十二长生：stem 见 branch，按 palace 那颗寄宫模式。 */
int sf_bazi_life_stage(uint8_t stem, uint8_t branch, int32_t earth_palace_mode,
                       uint8_t *out);

/* 空亡（旬空）：给一柱，出两个地支。
 *
 * 注意：顺序是有信息的，不是排序：`first` 恒为偶数支，于是阳干得
 * [first, second]（升序）、阴干得 [second, first]（**降序**）。同一个旬里
 * 甲子与乙丑的旬空集合相同而顺序相反 —— 上游测试覆盖这一行为
 * （getKongWang(0x00)===[10,11] 但 getKongWang(0x11)===[11,10]）。
 * 输出顺序属于接口语义，不应排序。 */
int sf_bazi_kong_wang(sf_ganzhi v, uint8_t out[2]);

/* 大运顺逆：阳年男 / 阴年女 顺行(+1)，否则逆行(-1)。
 *
 * 注意：吃的是**年柱**（锚立春的那个），不是「年号」—— 1900-01 出生的人年柱
 * 是上一年的，拿 sf_ganzhi_year_of(vt.year) 去判会**正好反掉**。 */
int sf_bazi_luck_direction(sf_ganzhi year_pillar, int32_t gender, int32_t *out);

/* 四柱外四柱。out 顺序 = 命宫 / 身宫 / 胎元 / 胎息。 */
int sf_bazi_extra_pillars(const sf_ganzhi_pillars *p,
                          sf_ganzhi out[SF_BAZI_EXTRA_PILLAR_COUNT]);

/* ------------------------------------------------------------------
 * 名字表（返回 rodata 里的静态串，调用方不用给缓冲）
 * ------------------------------------------------------------------ */
const char *sf_bazi_ten_god_name(uint8_t id);      /* 0..9，越界返回 NULL */
const char *sf_bazi_life_stage_name(uint8_t id);   /* 0..11 */
const char *sf_bazi_wuxing_name(uint8_t id);       /* 0..4，索引对齐 SF_WUXING_* */

/* ------------------------------------------------------------------
 * 五行 / 阴阳 —— 注意：非 JS 对齐面（派生），见文件头的说明
 *
 * 返回的是 SF_WUXING_*（WATER=0 WOOD=1 METAL=2 EARTH=3 FIRE=4），
 * 与 sf_bazi_wuxing_name() 的索引对齐，也就能直接喂给屏上的五行色表。
 * ------------------------------------------------------------------ */
int sf_bazi_stem_element(uint8_t stem, uint8_t *out_wuxing);
int sf_bazi_branch_element(uint8_t branch, uint8_t *out_wuxing);
int sf_bazi_stem_is_yang(uint8_t stem, int *out);
int sf_bazi_branch_is_yang(uint8_t branch, int *out);

#ifdef __cplusplus
}
#endif

#endif /* SF_BAZI_RULES_H */
