/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#ifndef SF_SOLAR_FAST_ALL_H
#define SF_SOLAR_FAST_ALL_H

/* ------------------------------------------------------------------
 * solar-fast 顶层公共入口
 *
 * 模块划分：
 *   - core/     : 天文核心引擎（日月几何/视位置、级数求值、Q31定点三角、求根）
 *   - rise/     : 视运动与出没系统（日出日落、月升月落、昼长、高度角、恒星时）
 *   - qishuo/   : 气朔系统（24 节气时刻、合朔望月、整年气朔编历）
 *   - calendar/ : 历法系统（农历、阴阳历转换、干支纪年法）
 *   - bazi/     : 八字命理（四柱排盘、起运、大运、流年流月流日流时、格局神煞）
 * ------------------------------------------------------------------ */

#include "core/solar_fast.h"
#include "rise/sf_rise.h"
#include "qishuo/sf_qishuo.h"
#include "calendar/sf_calendar.h"
#include "bazi/sf_bazi.h"
#include "bazi/sf_bazi_rules.h"

#endif /* SF_SOLAR_FAST_ALL_H */
