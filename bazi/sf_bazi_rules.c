/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* ==================================================================
 * sf_bazi_rules.c —— 八字的纯规则层
 *
 * 移植自 taiyin-lite/packages/bazi/src/rules.ts（201 行）与 constants.ts。
 * 逐函数对应，注释里引了上游行号。**这一层不链接任何天文代码。**
 * ================================================================== */
#include <string.h>

#include "sf_bazi_rules.h"

/* ------------------------------------------------------------------
 * 小工具
 * ------------------------------------------------------------------ */
static int valid_stem(uint8_t v)   { return v < 10u; }
static int valid_branch(uint8_t v) { return v < 12u; }
static uint8_t stem_of(sf_ganzhi v)   { return (uint8_t)(v >> 4); }
static uint8_t branch_of(sf_ganzhi v) { return (uint8_t)(v & 0x0fu); }

/* 一个「合法干支」= 干 <10、支 <12、且奇偶相同。上游 ganzhiStem() 的判据
 * 与 sf_ganzhi.c 的 valid_ganzhi() 完全一致（那边是 static，这里不跨模块取）。 */
static int valid_ganzhi(sf_ganzhi v)
{
    return valid_stem(stem_of(v)) && valid_branch(branch_of(v))
        && ((stem_of(v) & 1u) == (branch_of(v) & 1u));
}

/* ------------------------------------------------------------------
 * 数据表
 * ------------------------------------------------------------------ */

/* 藏干（rules.ts:14）。每支 1..3 个天干，**本气在首位**。
 * 容量恒 3（rules.ts:200 的 hiddenStemCapacity）。 */
static const uint8_t kHiddenStems[12][SF_BAZI_HIDDEN_STEM_CAPACITY] = {
    { 9, 0, 0 },   /* 子：癸 */
    { 5, 9, 7 },   /* 丑：己 癸 辛 */
    { 0, 2, 4 },   /* 寅：甲 丙 戊 */
    { 1, 0, 0 },   /* 卯：乙 */
    { 4, 1, 9 },   /* 辰：戊 乙 癸 */
    { 2, 6, 4 },   /* 巳：丙 庚 戊 */
    { 3, 5, 0 },   /* 午：丁 己 */
    { 5, 3, 1 },   /* 未：己 丁 乙 */
    { 6, 8, 4 },   /* 申：庚 壬 戊 */
    { 7, 0, 0 },   /* 酉：辛 */
    { 4, 7, 3 },   /* 戌：戊 辛 丁 */
    { 8, 0, 0 },   /* 亥：壬 甲 */
};
/* 亥是 {8,0}（壬、甲）—— 上面那行的第三个 0 是补位，个数由下表给。
 * 单独一张计数表而不是「数到 0 为止」：**甲(0) 是合法天干**，
 * 拿 0 当终止符会把亥的甲当成没有。 */
static const uint8_t kHiddenCount[12] = { 1, 3, 3, 1, 3, 3, 2, 3, 3, 1, 3, 2 };

/* 十二长生起点（rules.ts:38）。注意：甲起**亥(11)**不是寅 —— 这是三命通会
 * 那张阴干逆行的表，不得「顺手改成寅」。 */
static const uint8_t kLifeStageStart[10] = { 11, 6, 2, 9, 2, 9, 5, 0, 8, 3 };

/* 地支六合（rules.ts:21）。四柱外四柱的胎息要用。 */
static const uint8_t kBranchCombinationPartner[12] =
    { 1, 0, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2 };

/* 天干本气五行，按 `stem >> 1` 索引（甲乙→木、丙丁→火、戊己→土、
 * 庚辛→金、壬癸→水）。值与 shen-sha.ts:102 的 STEM_ELEMENT 相同 ——
 * 那个文件本库不移植，所以这里是独立的表，不是从它推的。 */
static const uint8_t kStemElement[5] = {
    SF_WUXING_WOOD,   /* 甲 乙 */
    SF_WUXING_FIRE,   /* 丙 丁 */
    SF_WUXING_EARTH,  /* 戊 己 */
    SF_WUXING_METAL,  /* 庚 辛 */
    SF_WUXING_WATER,  /* 壬 癸 */
};

/* ------------------------------------------------------------------
 * 名字表
 *
 * 用**定宽 char[N][W] 而不是 const char *const[N]**：定宽可以直接
 * `return kXxx[id];` 给出指针，不需要调用方缓冲；在 32 位 MCU 上还省掉
 * 一张指针数组。列宽按**字节**算（一个汉字 3 字节 + NUL）——
 * 十神/长生都是 2 字 = 7，五行 1 字 = 4。 */
static const char kTenGodNames[SF_BAZI_TEN_GOD_COUNT][7] = {
    "比肩", "劫财", "食神", "伤官", "偏财",
    "正财", "七杀", "正官", "偏印", "正印",
};
static const char kLifeStageNames[SF_BAZI_LIFE_STAGE_COUNT][7] = {
    "长生", "沐浴", "冠带", "临官", "帝旺", "衰",
    "病", "死", "墓", "绝", "胎", "养",
};
static const char kWuxingNames[5][4] = { "水", "木", "金", "土", "火" };

/* ------------------------------------------------------------------
 * 规则
 * ------------------------------------------------------------------ */

/* rules.ts:70 —— 位运算版，照抄。
 *
 * 注意：这里是**裸 `%` 不是 positive_mod**：靠那个 `+5` 保证非负
 * （两边 `>>1` 都在 0..4，+5 后落在 1..9）。挪动 +5 就会出负数，
 * 而 C 的 `%` 向零截断，会给出与 JS 不同的值。 */
int sf_bazi_ten_god(uint8_t day_stem, uint8_t target_stem, uint8_t *out)
{
    if (!out || !valid_stem(day_stem) || !valid_stem(target_stem)) return -1;
    const int delta = ((target_stem >> 1) + 5 - (day_stem >> 1)) % 5;
    *out = (uint8_t)((delta << 1) | ((day_stem ^ target_stem) & 1));
    return 0;
}

int sf_bazi_hidden_stems(uint8_t branch,
                         uint8_t out[SF_BAZI_HIDDEN_STEM_CAPACITY],
                         int *out_count)
{
    if (!out || !out_count || !valid_branch(branch)) return -1;
    memcpy(out, kHiddenStems[branch], SF_BAZI_HIDDEN_STEM_CAPACITY);
    *out_count = (int)kHiddenCount[branch];
    return 0;
}

/* rules.ts:149。
 *
 * 注意：两处容易被「修」坏的地方：
 *   1. 寄宫开关**只改戊(4)与己(5)两颗干的起点**，不是换整张表；
 *   2. 阴干那一支是**符号翻转**（`start + 12 − branch`），不是起点偏移。
 * 表里所有取值都能保证两个式子里的和为正，所以 `% 12` 安全。 */
int sf_bazi_life_stage(uint8_t stem, uint8_t branch, int32_t earth_palace_mode,
                       uint8_t *out)
{
    if (!out || !valid_stem(stem) || !valid_branch(branch)) return -1;
    if (earth_palace_mode != SF_BAZI_EARTH_PALACE_FIRE_EARTH
        && earth_palace_mode != SF_BAZI_EARTH_PALACE_WATER_EARTH) return -1;

    int start = kLifeStageStart[stem];
    if (earth_palace_mode == SF_BAZI_EARTH_PALACE_WATER_EARTH) {
        if (stem == 4u) start = 8;
        if (stem == 5u) start = 3;
    }
    *out = (uint8_t)(((stem & 1u) == 0u)
        ? (branch + 12 - start) % 12
        : (start + 12 - branch) % 12);
    return 0;
}

/* rules.ts:61。
 *
 * 注意：返回的两个地支是有序的，而且阴干是降序：first 恒为偶数支，
 * 所以阳干得 [first, second]（升序）、阴干得 [second, first]。
 * 上游测试钉死了：getKongWang(0x00)===[10,11] 而 getKongWang(0x11)===[11,10]。
 * 同一个旬里甲子与乙丑的旬空集合相同、顺序相反 —— **不得改成排序**。 */
int sf_bazi_kong_wang(sf_ganzhi v, uint8_t out[2])
{
    if (!out) return -1;
    /* 序号走库里的公开入口（它自带合法性检查），不在这里重抄 6·干−5·支 */
    int32_t index = 0;
    if (sf_ganzhi_index(v, &index) != 0) return -1;
    const int first  = (10 - (int)(index / 10) * 2 + 12) % 12;
    const int second = (first + 1) % 12;
    if ((int)(stem_of(v) & 1u) == (first & 1)) {
        out[0] = (uint8_t)first;  out[1] = (uint8_t)second;
    } else {
        out[0] = (uint8_t)second; out[1] = (uint8_t)first;
    }
    return 0;
}

/* fortune.ts:60 —— `((stem & 1) === 0) === (gender === MALE) ? 1 : -1`
 *
 * 也就是「阳年男 / 阴年女 顺行」。写成 `||` 就只剩一半。
 * 注意：吃的是**年柱**不是年号 —— 见头注。 */
int sf_bazi_luck_direction(sf_ganzhi year_pillar, int32_t gender, int32_t *out)
{
    if (!out) return -1;
    if (gender != SF_BAZI_GENDER_FEMALE && gender != SF_BAZI_GENDER_MALE) return -1;
    if (!valid_ganzhi(year_pillar)) return -1;
    const int yang = ((stem_of(year_pillar) & 1u) == 0u);
    *out = (yang == (gender == SF_BAZI_GENDER_MALE)) ? 1 : -1;
    return 0;
}

/* rules.ts:170。
 *
 *   月支 → 月序（寅月=1）→ monthPosition → 命宫支
 *   命宫干/身宫干 = 五虎遁的起点干 + 偏移 —— 那个起点公式
 *   `((yearStem % 5) * 2 + 2) % 10` 与 sf_ganzhi_get_month() 里的**逐字相同**，
 *   所以这里直接用 sf_ganzhi_get_month()，不另抄一遍。
 *
 * 注意：胎元是 `advanceGanzhi(month, -9)`。在 mod 10/12 下它等价于
 * 「干 +1、支 +3」，但**不得改写成 +3** —— 对拍比的是这一步的结果，
 * 而且 -9 是上游的字面值，改了以后读代码的人对不上。 */
int sf_bazi_extra_pillars(const sf_ganzhi_pillars *p,
                          sf_ganzhi out[SF_BAZI_EXTRA_PILLAR_COUNT])
{
    if (!p || !out) return -1;
    if (!valid_ganzhi(p->year) || !valid_ganzhi(p->month)
        || !valid_ganzhi(p->day) || !valid_ganzhi(p->hour)) return -1;

    const uint8_t year_stem   = stem_of(p->year);
    const uint8_t month_branch = branch_of(p->month);
    const uint8_t day_stem    = stem_of(p->day);
    const uint8_t day_branch  = branch_of(p->day);
    const uint8_t hour_branch = branch_of(p->hour);

    const int month_number   = ((month_branch + 10) % 12) + 1;      /* 寅月 = 1 */
    const int month_position = (12 - (month_number - 1)) % 12;
    const int ming_branch = (month_position + ((3 + 12 - hour_branch) % 12)) % 12;
    const int shen_branch = (month_branch + hour_branch + 1) % 12;

    /* 命宫/身宫的天干走五虎遁：起点干由年干定，月序用 (支+10)%12 折算 */
    sf_ganzhi ming = 0, shen = 0;
    if (sf_ganzhi_get_month(year_stem, (uint8_t)((ming_branch + 10) % 12), &ming) != 0
        || sf_ganzhi_get_month(year_stem, (uint8_t)((shen_branch + 10) % 12), &shen) != 0)
        return -1;

    sf_ganzhi tai_yuan = 0, tai_xi = 0;
    if (sf_ganzhi_advance(p->month, -9, &tai_yuan) != 0) return -1;
    if (sf_ganzhi_make((uint8_t)((day_stem + 5) % 10),
                       kBranchCombinationPartner[day_branch], &tai_xi) != 0) return -1;

    out[0] = ming;      /* 命宫 */
    out[1] = shen;      /* 身宫 */
    out[2] = tai_yuan;  /* 胎元 */
    out[3] = tai_xi;    /* 胎息 */
    return 0;
}

/* ------------------------------------------------------------------
 * 名字
 * ------------------------------------------------------------------ */
const char *sf_bazi_ten_god_name(uint8_t id)
{
    return id < SF_BAZI_TEN_GOD_COUNT ? kTenGodNames[id] : NULL;
}

const char *sf_bazi_life_stage_name(uint8_t id)
{
    return id < SF_BAZI_LIFE_STAGE_COUNT ? kLifeStageNames[id] : NULL;
}

const char *sf_bazi_wuxing_name(uint8_t id)
{
    return id <= SF_WUXING_FIRE ? kWuxingNames[id] : NULL;
}

/* ------------------------------------------------------------------
 * 五行 / 阴阳 —— 注意：非 JS 对齐面（派生），见头注
 * ------------------------------------------------------------------ */
int sf_bazi_stem_element(uint8_t stem, uint8_t *out_wuxing)
{
    if (!out_wuxing || !valid_stem(stem)) return -1;
    *out_wuxing = kStemElement[stem >> 1];
    return 0;
}

/* 地支的五行走**本气藏干**推，不另写一张 12 项的表：全库一份五行定义 */
int sf_bazi_branch_element(uint8_t branch, uint8_t *out_wuxing)
{
    if (!out_wuxing || !valid_branch(branch)) return -1;
    *out_wuxing = kStemElement[kHiddenStems[branch][0] >> 1];
    return 0;
}

int sf_bazi_stem_is_yang(uint8_t stem, int *out)
{
    if (!out || !valid_stem(stem)) return -1;
    *out = ((stem & 1u) == 0u);
    return 0;
}

int sf_bazi_branch_is_yang(uint8_t branch, int *out)
{
    if (!out || !valid_branch(branch)) return -1;
    *out = ((branch & 1u) == 0u);
    return 0;
}
