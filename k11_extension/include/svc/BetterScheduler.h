/*
*   This file is part of Luma3DS
*   Copyright (C) 2016-2020 Aurora Wright, TuxSH
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*   Additional Terms 7.b and 7.c of GPLv3 apply to this file:
*       * Requiring preservation of specified reasonable legal notices or
*         author attributions in that material or in the Appropriate Legal
*         Notices displayed by works containing it.
*       * Prohibiting misrepresentation of the origin of that material,
*         or requiring that modified versions of such material be marked in
*         reasonable ways as different from the original version.
*/

#pragma once

#include "utils.h"
#include "kernel.h"
#include "svc.h"

#define BETTER_SCHEDULER_START_SCHEDULER    (u32)(0x00040000)   //Start a custom scheduler (it should only be called from luma3ds (rosalina)).
#define BETTER_SCHEDULER_STOP_SCHEDULER     (u32)(0x00040001)   //Stop a custom scheduler (it should only be called from luma3ds (rosalina)).
#define BETTER_SCHEDULER_FEATURES           (u32)(0x00040002)   //Read features.
#define BETTER_SCHEDULER_REGISTER_THREAD    (u32)(0x00040005)   //Register a thread.
#define BETTER_SCHEDULER_UNREGISTER_THREAD  (u32)(0x00040006)   //Unregister a thread.
#define BETTER_SCHEDULER_SET_AFFINITY_MASK  (u32)(0x00040010)   //Set thread affinity mask.
#define BETTER_SCHEDULER_DEBUG              (u32)(0x000400FF)   //Debug.

#define BETTER_SCHEDULER_FEATURE_NONE               (u32)(0x00000000)   //No features.
#define BETTER_SCHEDULER_FEATURE_CROSS_CORE         (u32)(0x00000001)   //Cross-core context switch is supported.
#define BETTER_SCHEDULER_FEATURE_CORE_1_UNLIMITED   (u32)(0x00000002)   //Use of core #1 is unlimited.

void BetterSchedulerUpdateInSvcFlag(KThread *thread, bool inSvc);
void BetterSchedulerRemoveThread(KThread* thread);
Result BetterScheduler(u32 op, Handle threadHandle, u32 parameters);
