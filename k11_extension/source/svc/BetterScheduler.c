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
#include <string.h>

#include "svc/BetterScheduler.h"
#include "synchronization.h"

// #define BETTER_SCHEDULER_ENABLE_DEBUG

//Lower number gives higher priority.
#define BETTER_SCHEDULER_MAX_CORES                  (u8)(4)
#define BETTER_SCHEDULER_MAX_THREADS                (u8)(32)
#define BETTER_SCHEDULER_QUEUE_CAPACITY             (u8)(4)
#define BETTER_SCHEDULER_MIN_PRIORITY               (u8)(63)
#define BETTER_SCHEDULER_MAX_PRIORITY               (u8)(0)
#define BETTER_SCHEDULER_MIN_USER_PRIORITY          (u8)(63)
#define BETTER_SCHEDULER_MAX_USER_PRIORITY          (u8)(24)

#define BETTER_SCHEDULER_NONE_MASK                  (u8)(0x00)
#define BETTER_SCHEDULER_RUNNING_MASK               (u8)(0x01)
#define BETTER_SCHEDULER_IN_SVC_MASK                (u8)(0x02)
#define BETTER_SCHEDULER_DISABLE_SELECTION_MASK     (u8)(0x04)
#define BETTER_SCHEDULER_SWITCHING_MASK             (u8)(0x08)

typedef struct
{
    u8 registeredThreads;
    KThread *thread[BETTER_SCHEDULER_MAX_THREADS];
} BetterSchedulerThreads;

typedef struct
{
    u8 targetCore;
    KThread *thread;
} BetterSchedulerQueue;

typedef struct
{
    bool isReady;
    KRecursiveLock lock;
    KEvent *events[BETTER_SCHEDULER_MAX_CORES];
    KThread *workerThreads[BETTER_SCHEDULER_MAX_CORES];
    KThread *currentThreads[BETTER_SCHEDULER_MAX_CORES];
    BetterSchedulerQueue targetQueue[BETTER_SCHEDULER_MAX_CORES][BETTER_SCHEDULER_QUEUE_CAPACITY];
    BetterSchedulerThreads threads;
} BetterSchedulerRWLockShared;

typedef struct
{
    u8 corePriority[BETTER_SCHEDULER_MAX_CORES];
    u8 numOfCores;
} BetterSchedulerNoLockShared;

static void BetterSchedulerSetUpNoLockShared(void);
static inline bool BetterSchedulerIsThreadRegistered(KThread *thread);
static inline u8 BetterSchedulerGetThreadIndex(KThread *thread);
static void BetterSchedulerGetPriorityList(u8 numOfCores, u8 *highestPriorityPerCore);
static void BetterSchedulerCleanUpInvalidThreads(void);
static bool BetterSchedulerCheckIsReady(u8 numOfCores);
static KThread * BetterSchedulerFindTarget(u8 currentCore, u8 currentMaxPriority);
extern bool BetterSchedulerContextSwitchHookCore1c(void);
extern void BetterSchedulerContextSwitchHookc(KThread *nextThread);

static volatile BetterSchedulerNoLockShared betterSchedulerNoLockShared = { 0, };//This is more like const (set by scheduler thread on boot and not changed after that) therefore no lock is needed.
static volatile BetterSchedulerRWLockShared betterSchedulerRWLockShared = { 0, };
#if defined(BETTER_SCHEDULER_ENABLE_DEBUG)
static volatile u32 debug[BETTER_SCHEDULER_MAX_CORES] = { 0, };
#endif //defined(BETTER_SCHEDULER_ENABLE_DEBUG)

static inline u8 BetterSchedulerReadPadding(volatile u8* address)
{
    u8 value;

    do
    {
        value = (u8)__ldrex8((s8*)address);
    }
    while (__strex8((s8*)address, (s8)value));

    __dmb();

    return value;
}

static inline void BetterSchedulerRemovePadding(volatile u8* address, u8 bitToRemove)
{
    u8 value;

    do
    {
        value = (u8)__ldrex8((s8*)address);
        value &= ~bitToRemove;
    }
    while (__strex8((s8*)address, (s8)value));

    __dmb();
}

static inline void BetterSchedulerAddPadding(volatile u8* address, u8 bitToAdd)
{
    u8 value;

    do
    {
        value = (u8)__ldrex8((s8*)address);
        value |= bitToAdd;
    }
    while (__strex8((s8*)address, (s8)value));

    __dmb();
}

static inline u8 BetterSchedulerReadAndUpdatePadding(volatile u8* address, u8 bitToAdd, u8 bitToRemove)
{
    u8 old_value;
    u8 new_value;

    do
    {
        old_value = (u8)__ldrex8((s8*)address);
        new_value = (old_value | bitToAdd);
        new_value &= ~bitToRemove;
    }
    while (__strex8((s8*)address, (s8)new_value));

    __dmb();

    return old_value;
}

static void BetterSchedulerSwitchCore(volatile KThread* target, KScheduler* currentScheduler, u8 currentCore, u8 targetCore)
{
    bool reschedule = false;
    u8 padding;

    KRecursiveLock__Lock(criticalSectionLock);
    {
        do
        {
            padding = (u8)__ldrex8((s8*)(&target->padding));

            if((padding & BETTER_SCHEDULER_IN_SVC_MASK) == 0
            && (target->schedulingMask == 0x00 || target->schedulingMask == 0x01))
            {
                //Remove from scheduler.
                if(target->schedulingMask == 0x01)
                {
                    target->schedulingMask = 0x00;
                    KScheduler__AdjustThread(currentScheduler, (KThread*)target, 0x01);
                }

                //Switch core.
                target->coreId = targetCore;

                padding |= BETTER_SCHEDULER_SWITCHING_MASK;
                reschedule = true;
            }
            else
            {
                //Interrupted by SVC, restore the original core if it's not scheduled.
                if(target->schedulingMask == 0x00)
                    target->coreId = currentCore;
            }

            padding &= ~BETTER_SCHEDULER_DISABLE_SELECTION_MASK;
        }
        while (__strex8((s8*)(&target->padding), (s8)padding));

        __dmb();

        if(reschedule)
        {
            //Add to scheduler.
            if(target->schedulingMask == 0x00)
            {
                target->schedulingMask = 0x01;
                KScheduler__AdjustThread(currentScheduler, (KThread*)target, 0x00);
            }
        }
    }
    KRecursiveLock__Unlock(criticalSectionLock);
}

bool BetterSchedulerContextSwitchHookCore1c(void)
{
    bool forbid_preemption = false;

    KRecursiveLock__Lock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);
    {
        //If there are no threads on better scheduler (i.e. in official apps), allow core #1 preemption to avoid regression.
        forbid_preemption = (betterSchedulerRWLockShared.threads.registeredThreads > 0);
    }
    KRecursiveLock__Unlock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);

    return forbid_preemption;
}

void BetterSchedulerContextSwitchHookc(KThread *nextThread)
{
    bool isNextThreadSwitching = false;
    u8 currentCore = nextThread->coreId;
    u8 old_padding = 0;
    KThread *currentThread = currentCoreContext->objectContext.currentThread;

    //We use padding in KThread struct as thread state storage.
    //Thread is running.
    old_padding = BetterSchedulerReadAndUpdatePadding(&nextThread->padding, BETTER_SCHEDULER_RUNNING_MASK, BETTER_SCHEDULER_SWITCHING_MASK);

    isNextThreadSwitching = ((old_padding & BETTER_SCHEDULER_SWITCHING_MASK) == BETTER_SCHEDULER_SWITCHING_MASK);

    //Thread is NOT running.
    BetterSchedulerRemovePadding(&currentThread->padding, BETTER_SCHEDULER_RUNNING_MASK);

    //This will enable FPU, save FPU registers for old thread
    //and restore FPU registers for next thread.
    //We need to do this for cross-core context switch.
    ContextSwitchFpu(nextThread);

    //Note: We should lock from here with KRecursiveLock__Lock(), however doing so
    //will cause system to KRecursiveLock__Lock() and KRecursiveLock__Unlock() everytime
    //context switch happens and results in noticeable slow down, so we don't lock it here.

    //Don't include our scheduler thread to avoid unnecessary cross-core context switch.
    if(nextThread != betterSchedulerRWLockShared.workerThreads[currentCore])
        betterSchedulerRWLockShared.currentThreads[currentCore] = nextThread;

    if(nextThread->dynamicPriority > BETTER_SCHEDULER_MAX_USER_PRIORITY && !isNextThreadSwitching
    && betterSchedulerRWLockShared.isReady && betterSchedulerRWLockShared.threads.registeredThreads > 0)
    {
        KRecursiveLock__Lock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);
        {
            //Double check RWLockShared after locking.
            if(betterSchedulerRWLockShared.isReady && betterSchedulerRWLockShared.threads.registeredThreads > 0)
            {
                u8 numOfCores = betterSchedulerNoLockShared.numOfCores;
                bool unavailableList[BETTER_SCHEDULER_MAX_CORES] = { 0, };
                u8 priorityList[BETTER_SCHEDULER_MAX_CORES] = { 0, };

                BetterSchedulerGetPriorityList(numOfCores, priorityList);

                for(u8 i = 0; i < numOfCores; i++)
                {
                    if(priorityList[i] <= BETTER_SCHEDULER_MAX_USER_PRIORITY)
                        unavailableList[i] = true;//No user threads have the priority greater than current priority, ignore this core.
                }

                for(u8 i = 0; i < numOfCores; i++)
                {
                    u8 lowestPriorityCore = UINT8_MAX;
                    u8 lowestPriority = BETTER_SCHEDULER_MAX_PRIORITY;
                    KThread *target = NULL;

                    //Search for lowest priority core.
                    for(u8 k = 0; k < numOfCores; k++)
                    {
                        u8 core = betterSchedulerNoLockShared.corePriority[k];

                        if(unavailableList[core])
                            continue;

                        //Lower number gives higher priority.
                        if(priorityList[core] > lowestPriority)
                        {
                            lowestPriority = priorityList[core];
                            lowestPriorityCore = core;
                        }
                    }

                    if(lowestPriorityCore == UINT8_MAX)
                        break;//Done.

                    //Search for the target thread.
                    target = BetterSchedulerFindTarget(lowestPriorityCore, lowestPriority);
                    if(target)
                    {
                        //We've found the target thread.
                        u8 targetCurrentCore = target->coreId;
                        KEvent *event = (KEvent *)betterSchedulerRWLockShared.events[targetCurrentCore];

                        for(u8 k = 0; k < BETTER_SCHEDULER_QUEUE_CAPACITY; k++)
                        {
                            if(!betterSchedulerRWLockShared.targetQueue[targetCurrentCore][k].thread)
                            {
                                //We can't change the core here (trying to do so result in crashing/freezing the kernel)
                                //so send the data to our worker thread.
                                betterSchedulerRWLockShared.targetQueue[targetCurrentCore][k].targetCore = lowestPriorityCore;
                                betterSchedulerRWLockShared.targetQueue[targetCurrentCore][k].thread = target;
                                BetterSchedulerAddPadding(&target->padding, BETTER_SCHEDULER_DISABLE_SELECTION_MASK);

#if defined(BETTER_SCHEDULER_ENABLE_DEBUG)
                                debug[lowestPriorityCore]++;
#endif //defined(BETTER_SCHEDULER_ENABLE_DEBUG)

                                //We don't need to signal it if it's already signaled, scheduler thread will process all of them.
                                if(!event->isSignaled)
                                {
                                    event->isSignaled = true;

                                    //Notify it to our scheduler thread.
                                    KSynchronizationObject__Signal(&event->syncObject, (event->resetType == RESET_PULSE));

                                    //Doesn't work.
                                    // KEvent__Signal(event);
                                }

                                break;
                            }
                        }
                    }

                    unavailableList[lowestPriorityCore] = true;
                }
            }
        }
        KRecursiveLock__Unlock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);
    }
}

void BetterSchedulerUpdateInSvcFlag(KThread *thread, bool inSvc)
{
    if(inSvc)
        BetterSchedulerAddPadding(&thread->padding, BETTER_SCHEDULER_IN_SVC_MASK);
    else
        BetterSchedulerRemovePadding(&thread->padding, BETTER_SCHEDULER_IN_SVC_MASK);
}

void BetterSchedulerRemoveThread(KThread *thread)
{
    KRecursiveLock__Lock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);
    {
        for(u8 i = 0; i < betterSchedulerRWLockShared.threads.registeredThreads; i++)
        {
            KThread *target = betterSchedulerRWLockShared.threads.thread[i];

            if(!target)
                continue;

            if(target == thread)
            {
                KAutoObject *obj = (KAutoObject *)target;
                obj->vtable->DecrementReferenceCount(obj);

                //Remove from the list.
                for(u8 k = (i + 1); k < betterSchedulerRWLockShared.threads.registeredThreads; k++)
                    betterSchedulerRWLockShared.threads.thread[k - 1] = betterSchedulerRWLockShared.threads.thread[k];

                betterSchedulerRWLockShared.threads.thread[betterSchedulerRWLockShared.threads.registeredThreads - 1] = NULL;
                betterSchedulerRWLockShared.threads.registeredThreads--;
                continue;
            }
        }

        BetterSchedulerCleanUpInvalidThreads();
    }
    KRecursiveLock__Unlock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);
}

Result BetterScheduler(u32 op, Handle threadHandle, u32 parameters)
{
    Result result = 0;

    if(op == BETTER_SCHEDULER_START_SCHEDULER)
    {
        u8 currentCore = getCurrentCoreID();
        KProcessHandleTable *table = handleTableOfProcess(currentCoreContext->objectContext.currentProcess);
        KEvent *event = (KEvent *)KProcessHandleTable__ToKAutoObject(table, parameters);

        BetterSchedulerSetUpNoLockShared();

        KRecursiveLock__Lock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);
        {
            if(!event)
                result = 0xD8E007F7;//Invalid handle.
            else if(betterSchedulerRWLockShared.workerThreads[currentCore])
            {
                //Remove reference.
                KAutoObject *obj = (KAutoObject *)event;
                obj->vtable->DecrementReferenceCount(obj);

                result = 0xF8C007F4;//Scheduler thread already exists for this core, not implemented.
            }
            else
            {
                betterSchedulerRWLockShared.events[currentCore] = event;
                betterSchedulerRWLockShared.workerThreads[currentCore] = currentCoreContext->objectContext.currentThread;
                betterSchedulerRWLockShared.isReady = BetterSchedulerCheckIsReady(betterSchedulerNoLockShared.numOfCores);
                result = 0;
            }
        }
        KRecursiveLock__Unlock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);

        if(result == 0)
        {
            bool wait = false;
            KThread *currentThread = currentCoreContext->objectContext.currentThread;
            KScheduler *currentScheduler = currentCoreContext->objectContext.currentScheduler;

            while(true)
            {
                bool must_stop = false;
                u8 targetCore = 0;
                KThread *target = NULL;

                if(wait)//What should I do for `betterSchedulerRWLockShared.events` here? Locking mutex would block everything...
                    WaitSynchronization1(NULL, currentThread, (KSynchronizationObject *)&betterSchedulerRWLockShared.events[currentCore]->syncObject, U64_MAX);

                KRecursiveLock__Lock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);
                {
                    //Check if we must exit the scheduler.
                    if(!betterSchedulerRWLockShared.workerThreads[currentCore])
                    {
                        //Remove reference.
                        KAutoObject *obj = (KAutoObject *)betterSchedulerRWLockShared.events[currentCore];
                        obj->vtable->DecrementReferenceCount(obj);

                        //Empty queue.
                        for(u8 i = 0; i < BETTER_SCHEDULER_QUEUE_CAPACITY; i++)
                        {
                            if(betterSchedulerRWLockShared.targetQueue[currentCore][i].thread)
                                BetterSchedulerRemovePadding(&betterSchedulerRWLockShared.targetQueue[currentCore][i].thread->padding, BETTER_SCHEDULER_DISABLE_SELECTION_MASK);

                            betterSchedulerRWLockShared.targetQueue[currentCore][i].targetCore = 0;
                            betterSchedulerRWLockShared.targetQueue[currentCore][i].thread = NULL;
                        }

                        betterSchedulerRWLockShared.events[currentCore] = NULL;
                        betterSchedulerRWLockShared.isReady = BetterSchedulerCheckIsReady(betterSchedulerNoLockShared.numOfCores);

                        must_stop = true;//We must stop now.
                        goto unlock_mutex;
                    }

                    //Get data from context-switch queue.
                    targetCore = betterSchedulerRWLockShared.targetQueue[currentCore][0].targetCore;
                    target = betterSchedulerRWLockShared.targetQueue[currentCore][0].thread;

                    if(target)
                    {
                        BetterSchedulerSwitchCore(target, currentScheduler, currentCore, targetCore);

                        //Update the queue.
                        for(u8 i = 1; i < BETTER_SCHEDULER_QUEUE_CAPACITY; i++)
                        {
                            betterSchedulerRWLockShared.targetQueue[currentCore][i - 1].targetCore = betterSchedulerRWLockShared.targetQueue[currentCore][i].targetCore;
                            betterSchedulerRWLockShared.targetQueue[currentCore][i - 1].thread = betterSchedulerRWLockShared.targetQueue[currentCore][i].thread;
                        }
                        betterSchedulerRWLockShared.targetQueue[currentCore][BETTER_SCHEDULER_QUEUE_CAPACITY - 1].targetCore = 0;
                        betterSchedulerRWLockShared.targetQueue[currentCore][BETTER_SCHEDULER_QUEUE_CAPACITY - 1].thread = NULL;

                        // SleepThreadInternal(currentThread, NULL, 10000000);//10ms
                    }

                    wait = (betterSchedulerRWLockShared.targetQueue[currentCore][0].thread ? false : true);
                }
                unlock_mutex:
                KRecursiveLock__Unlock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);

                if(must_stop)
                    break;
            }

            result = 0;//Success.
        }
    }
    else if(op == BETTER_SCHEDULER_STOP_SCHEDULER)
    {
        KRecursiveLock__Lock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);
        {
            //Stop all schedulers.
            for(u8 i = 0; i < BETTER_SCHEDULER_MAX_CORES; i++)
            {
                betterSchedulerRWLockShared.workerThreads[i] = NULL;
                if(betterSchedulerRWLockShared.events[i])
                {
                    KEvent *event = (KEvent *)betterSchedulerRWLockShared.events[i];

                    //Always signal no matter if it's already signaled just in case.
                    //Note: Double signaling is fine because it prioritizes exit `if(!betterSchedulerRWLockShared.workerThreads[currentCore])` check.
                    event->isSignaled = true;
                    KSynchronizationObject__Signal(&event->syncObject, (event->resetType == RESET_PULSE));
                }
            }
        }
        KRecursiveLock__Unlock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);

        result = 0;//Success.
    }
    else if(op == BETTER_SCHEDULER_FEATURES)
    {
        u32 *dst = (u32 *)threadHandle;

        if(dst)
        {
            *dst = BETTER_SCHEDULER_FEATURE_NONE;
            *dst |= BETTER_SCHEDULER_FEATURE_CROSS_CORE;

            result = 0;//Success.
        }
        else
            result = 0xD8E007F7;//Invalid handle (actually invalid buffer pointer).
    }
    else if(op == BETTER_SCHEDULER_REGISTER_THREAD
    || op == BETTER_SCHEDULER_SET_AFFINITY_MASK)
    {
        u8 coreMask = 0;

        for(u8 i = 0; i < betterSchedulerNoLockShared.numOfCores; i++)
            coreMask |= (1 << i);

        if((parameters & coreMask) != 0)
        {
            KProcessHandleTable *handleTable = handleTableOfProcess(currentCoreContext->objectContext.currentProcess);
            KThread *thread = KProcessHandleTable__ToKThread(handleTable, threadHandle);

            //We have valid mask.
            parameters = (parameters & coreMask);

            KRecursiveLock__Lock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);
            {
                BetterSchedulerCleanUpInvalidThreads();

                if(thread)
                {
                    if(op == BETTER_SCHEDULER_REGISTER_THREAD)
                    {
                        bool isFull = true;

                        //Register requested thread.
                        if(BetterSchedulerIsThreadRegistered(thread))
                        {
                            //Already registered, close handle and return.
                            KAutoObject *obj = (KAutoObject *)thread;
                            obj->vtable->DecrementReferenceCount(obj);
                            result = 0;//Already registered.
                        }
                        else
                        {
                            for(u8 i = betterSchedulerRWLockShared.threads.registeredThreads; i < BETTER_SCHEDULER_MAX_THREADS; i++)
                            {
                                if(!betterSchedulerRWLockShared.threads.thread[i])
                                {
                                    betterSchedulerRWLockShared.threads.thread[i] = thread;
                                    betterSchedulerRWLockShared.threads.thread[i]->affinityMask = parameters;
                                    betterSchedulerRWLockShared.threads.registeredThreads++;
                                    isFull = false;
                                    break;
                                }
                            }

                            if(!isFull)
                                result = 0;//Successfully registered.
                            else
                            {
                                //List is full, close handle and return.
                                KAutoObject *obj = (KAutoObject *)thread;
                                obj->vtable->DecrementReferenceCount(obj);
                                result = 0xC860180A;//Out of memory.
                            }
                        }
                    }
                    else if(op == BETTER_SCHEDULER_SET_AFFINITY_MASK)
                    {
                        u8 index = BetterSchedulerGetThreadIndex(thread);

                        if(index != BETTER_SCHEDULER_MAX_THREADS)
                        {
                            //Set affinity mask.
                            betterSchedulerRWLockShared.threads.thread[index]->affinityMask = parameters;
                            result = 0;//Success.
                        }
                        else
                            result = 0xD8E007F7;//Thread is NOT registered, invalid handle (for this operation).

                        //Close handle and return.
                        KAutoObject *obj = (KAutoObject *)thread;
                        obj->vtable->DecrementReferenceCount(obj);
                    }
                }
                else
                    result = 0xD8E007F7;//Invalid handle.
            }
            KRecursiveLock__Unlock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);
        }
        else
            result = 0xD8E007ED;//At least 1 processor must be selected, invalid enum (parameters) value.
    }
    else if(op == BETTER_SCHEDULER_UNREGISTER_THREAD)
    {
        KProcessHandleTable *handleTable = handleTableOfProcess(currentCoreContext->objectContext.currentProcess);
        KThread *thread = KProcessHandleTable__ToKThread(handleTable, threadHandle);

        KRecursiveLock__Lock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);
        {
            BetterSchedulerCleanUpInvalidThreads();

            if(thread)
            {
                u8 index = BetterSchedulerGetThreadIndex(thread);

                //Remove requested thread if exists.
                if(index != BETTER_SCHEDULER_MAX_THREADS)
                {
                    KAutoObject *obj = (KAutoObject *)betterSchedulerRWLockShared.threads.thread[index];

                    BetterSchedulerRemovePadding(&betterSchedulerRWLockShared.threads.thread[index]->padding, BETTER_SCHEDULER_DISABLE_SELECTION_MASK);
                    betterSchedulerRWLockShared.threads.thread[index]->affinityMask = (1 << betterSchedulerRWLockShared.threads.thread[index]->coreId);
                    obj->vtable->DecrementReferenceCount(obj);

                    for(u8 k = (index + 1); k < betterSchedulerRWLockShared.threads.registeredThreads; k++)
                        betterSchedulerRWLockShared.threads.thread[k - 1] = betterSchedulerRWLockShared.threads.thread[k];

                    betterSchedulerRWLockShared.threads.thread[betterSchedulerRWLockShared.threads.registeredThreads - 1] = NULL;
                    betterSchedulerRWLockShared.threads.registeredThreads--;
                }

                //Close handle and return.
                KAutoObject *obj = (KAutoObject *)thread;
                obj->vtable->DecrementReferenceCount(obj);

                result = 0;//Successfully unregisterd or not registered.
            }
            else
                result = 0;//Always return success for unregister request.
        }
        KRecursiveLock__Unlock((KRecursiveLock*)&betterSchedulerRWLockShared.lock);
    }
    else if(op == BETTER_SCHEDULER_DEBUG)
    {
#if defined(BETTER_SCHEDULER_ENABLE_DEBUG)
        u32 *dst = (u32 *)threadHandle;

        if(dst)
            memcpy(dst, (void *)debug, sizeof(debug));
#endif //defined(BETTER_SCHEDULER_ENABLE_DEBUG)

        result = 0;
    }
    else
        result = 0xF8C007F4;//Not implemented.

    return result;
}

static void BetterSchedulerSetUpNoLockShared(void)
{
    betterSchedulerNoLockShared.numOfCores = getNumberOfCores();
    betterSchedulerNoLockShared.numOfCores = ((betterSchedulerNoLockShared.numOfCores > BETTER_SCHEDULER_MAX_CORES) ? BETTER_SCHEDULER_MAX_CORES : betterSchedulerNoLockShared.numOfCores);

    //Assign core priority (prefered core for cross-core context switch).
    if(betterSchedulerNoLockShared.numOfCores == 2)
    {
        betterSchedulerNoLockShared.corePriority[0] = 0;//User core.
        betterSchedulerNoLockShared.corePriority[1] = 1;//System core.
    }
    else if(betterSchedulerNoLockShared.numOfCores == 4)
    {
        betterSchedulerNoLockShared.corePriority[0] = 2;//User core.
        betterSchedulerNoLockShared.corePriority[1] = 0;//User core.
        betterSchedulerNoLockShared.corePriority[2] = 3;//System core.
        betterSchedulerNoLockShared.corePriority[3] = 1;//System core.
    }
}

static inline bool BetterSchedulerIsThreadRegistered(KThread *thread)
{
    //Check if it's registered.
    for(u8 i = 0; i < betterSchedulerRWLockShared.threads.registeredThreads; i++)
    {
        if(betterSchedulerRWLockShared.threads.thread[i] == thread)
            return true;
    }

    return false;
}

static inline u8 BetterSchedulerGetThreadIndex(KThread *thread)
{
    //Return thread index if exists.
    for(u8 i = 0; i < betterSchedulerRWLockShared.threads.registeredThreads; i++)
    {
        if(betterSchedulerRWLockShared.threads.thread[i] == thread)
            return i;
    }

    return BETTER_SCHEDULER_MAX_THREADS;
}

static void BetterSchedulerGetPriorityList(u8 numOfCores, u8 *highestPriorityPerCore)
{
    for(u8 i = 0; i < numOfCores; i++)
    {
        //Get the priority for the thread that is currently running (or about to be executed).
        u8 currentMaxPriority = betterSchedulerRWLockShared.currentThreads[i]->dynamicPriority;

        //Get current maximum priority for the core including pending (switching) threads.
        for(u8 k = 0; k < numOfCores; k++)
        {
            for(u8 m = 0; m < BETTER_SCHEDULER_QUEUE_CAPACITY; m++)
            {
                KThread *pendingThread = betterSchedulerRWLockShared.targetQueue[k][m].thread;

                if(!pendingThread)
                    break;

                if(betterSchedulerRWLockShared.targetQueue[k][m].targetCore == k)
                {
                    //We also include pending threads.
                    //Lower number gives higher priority.
                    if(currentMaxPriority > pendingThread->dynamicPriority)
                        currentMaxPriority = pendingThread->dynamicPriority;
                }
            }
        }

        highestPriorityPerCore[i] = currentMaxPriority;
    }
}

static void BetterSchedulerCleanUpInvalidThreads(void)
{
    //Clean up invalid threads.
    for(u8 i = 0; i < betterSchedulerRWLockShared.threads.registeredThreads; i++)
    {
        KThread *target = betterSchedulerRWLockShared.threads.thread[i];

        if(!target)
            continue;

        if(target->shallTerminate || target->isEnded || !target->isAlive)
        {
            KAutoObject *obj = (KAutoObject *)target;
            obj->vtable->DecrementReferenceCount(obj);

            //Remove from the list.
            for(u8 k = (i + 1); k < betterSchedulerRWLockShared.threads.registeredThreads; k++)
                betterSchedulerRWLockShared.threads.thread[k - 1] = betterSchedulerRWLockShared.threads.thread[k];

            betterSchedulerRWLockShared.threads.thread[betterSchedulerRWLockShared.threads.registeredThreads - 1] = NULL;
            betterSchedulerRWLockShared.threads.registeredThreads--;
        }
    }
}

static bool BetterSchedulerCheckIsReady(u8 numOfCores)
{
    bool isSchedulerReady = true;

    for(u8 i = 0; i < numOfCores; i++)
    {
        if(!betterSchedulerRWLockShared.events[i])
        {
            isSchedulerReady = false;
            break;
        }
    }

    return isSchedulerReady;
}

static KThread * BetterSchedulerFindTarget(u8 currentCore, u8 currentMaxPriority)
{
    KThread *finalTarget = NULL;

    for(u8 i = 0; i < betterSchedulerRWLockShared.threads.registeredThreads; i++)
    {
        u8 padding = 0;
        KThread *target = betterSchedulerRWLockShared.threads.thread[i];

        if(betterSchedulerRWLockShared.currentThreads[target->coreId] == target)
            continue;

        //We seek for the thread that wants to run (scheduled) but couldn't (because of other threads).
        //So, skip if at least one of them is true :
        //1. Target thread is NOT scheduled (schedulingMask == 0x00).
        //2. Target thread is running.
        //3. Target thread is in SVC (trying to switch between cores here will cause crash).
        //4. Selection is disabled for target thread.
        padding = BetterSchedulerReadPadding(&target->padding);
        if((target->schedulingMask == 0x00) || ((padding & BETTER_SCHEDULER_RUNNING_MASK) == BETTER_SCHEDULER_RUNNING_MASK)
        || ((padding & BETTER_SCHEDULER_IN_SVC_MASK) == BETTER_SCHEDULER_IN_SVC_MASK)
        || ((padding & BETTER_SCHEDULER_DISABLE_SELECTION_MASK) == BETTER_SCHEDULER_DISABLE_SELECTION_MASK))
            continue;

        if((target->affinityMask & (1 << currentCore)) == 0)
            continue;//Target thread doesn't like current cocre, do nothing.

        if((target->affinityMask & (1 << target->coreId)) == 0)
        {
            //Target thread doesn't like the core currently running on, skip core and priority check and continue.
            finalTarget = target;
            currentMaxPriority = finalTarget->dynamicPriority;
            continue;
        }
        else
        {
            if(target->coreId == currentCore)
                continue;//We don't care the thread that runs on the same core since highest priority thread will automatically be executed.

            if(currentMaxPriority <= target->dynamicPriority)
                continue;//Higher or equal priority thread is present.

            finalTarget = target;
            currentMaxPriority = finalTarget->dynamicPriority;
        }
    }

    return finalTarget;
}
