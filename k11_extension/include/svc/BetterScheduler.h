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

#define BETTER_SCHEDULER_START_SCHEDULER    (u32)(0x00040000)
#define BETTER_SCHEDULER_STOP_SCHEDULER     (u32)(0x00040001)
#define BETTER_SCHEDULER_DEBUG              (u32)(0x00040002)
#define BETTER_SCHEDULER_REGISTER_THREAD    (u32)(0x00040005)
#define BETTER_SCHEDULER_UNREGISTER_THREAD  (u32)(0x00040006)
#define BETTER_SCHEDULER_SET_AFFINITY_MASK  (u32)(0x00040010)

void BetterSchedulerUpdateInSvcFlag(KThread *thread, bool inSvc);
void BetterSchedulerRemoveThread(KThread* thread);
Result BetterScheduler(u32 op, Handle threadHandle, u32 parameters);
