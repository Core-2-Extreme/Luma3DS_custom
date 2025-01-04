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

// #define BETTER_SCHEDULER_DEBUG

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

static void BetterSchedulerGetPriorityList(u8 numOfCores, u8 *highestPriorityPerCore);
static void BetterSchedulerCleanUpInvalidThreads(void);
static bool BetterSchedulerCheckIsReady(u8 numOfCores);
static KThread * BetterSchedulerFindTarget(u8 currentCore, u8 currentMaxPriority);
extern void BetterSchedulerContextSwitchHookc(KThread *nextThread);

static volatile bool betterSchedulerIsReady = false;
static volatile u8 betterSchedulerCorePriority[BETTER_SCHEDULER_MAX_CORES] = { 0, };
static volatile u8 betterSchedulerNumOfCores = BETTER_SCHEDULER_MAX_CORES;
static volatile KEvent *betterSchedulerEvents[BETTER_SCHEDULER_MAX_CORES] = { 0, };
static volatile KThread *betterSchedulerWorkerThreads[BETTER_SCHEDULER_MAX_CORES] = { 0, };
static volatile KThread *betterSchedulerCurrentThreads[BETTER_SCHEDULER_MAX_CORES] = { 0, };
static volatile BetterSchedulerQueue betterSchedulerTargetQueue[BETTER_SCHEDULER_MAX_CORES][BETTER_SCHEDULER_QUEUE_CAPACITY] = { 0, };
static volatile BetterSchedulerThreads betterSchedulerThreads = { 0, };
#if defined(BETTER_SCHEDULER_DEBUG)
static volatile u32 debug[BETTER_SCHEDULER_MAX_CORES] = { 0, };
#endif //defined(BETTER_SCHEDULER_DEBUG)

void BetterSchedulerContextSwitchHookc(KThread *nextThread)
{
    bool isNextThreadSwitching = false;
    u8 currentCore = nextThread->coreId;
    KThread *currentThread = currentCoreContext->objectContext.currentThread;

    //Don't include our scheduler thread to avoid unnecessary cross core context switch.
    if(nextThread != betterSchedulerWorkerThreads[currentCore])
        betterSchedulerCurrentThreads[currentCore] = nextThread;

    isNextThreadSwitching = ((nextThread->padding & BETTER_SCHEDULER_SWITCHING_MASK) == BETTER_SCHEDULER_SWITCHING_MASK);

    //We use padding in KThread struct as thread state storage.
    //Thread is running.
    nextThread->padding |= BETTER_SCHEDULER_RUNNING_MASK;
    nextThread->padding &= ~BETTER_SCHEDULER_SWITCHING_MASK;
    //Thread is NOT running.
    currentThread->padding &= ~BETTER_SCHEDULER_RUNNING_MASK;

    //This will enable FPU, save FPU registers for old thread
    //and restore FPU registers for next thread.
    //We need to do this for cross core context switch.
    ContextSwitchFpu(nextThread);

    if(nextThread->dynamicPriority > BETTER_SCHEDULER_MAX_USER_PRIORITY
    && !isNextThreadSwitching && betterSchedulerIsReady && betterSchedulerThreads.registeredThreads > 0)
    {
        bool unavailableList[BETTER_SCHEDULER_MAX_CORES] = { 0, };
        u8 priorityList[BETTER_SCHEDULER_MAX_CORES] = { 0, };

        KRecursiveLock__Lock(criticalSectionLock);

        BetterSchedulerGetPriorityList(betterSchedulerNumOfCores, priorityList);

        for(u8 i = 0; i < betterSchedulerNumOfCores; i++)
        {
            if(priorityList[i] <= BETTER_SCHEDULER_MAX_USER_PRIORITY)
                unavailableList[i] = true;//No user threads have the priority greater than current priority, ignore this core.
        }

        for(u8 i = 0; i < betterSchedulerNumOfCores; i++)
        {
            u8 lowestPriorityCore = UINT8_MAX;
            u8 lowestPriority = BETTER_SCHEDULER_MAX_PRIORITY;
            KThread *target = NULL;

            //Search for lowest priority core.
            for(u8 k = 0; k < betterSchedulerNumOfCores; k++)
            {
                u8 core = betterSchedulerCorePriority[k];

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
                KEvent *event = (KEvent *)betterSchedulerEvents[targetCurrentCore];

                for(u8 k = 0; k < BETTER_SCHEDULER_QUEUE_CAPACITY; k++)
                {
                    if(!betterSchedulerTargetQueue[targetCurrentCore][k].thread)
                    {
                        //We can't change the core here (trying to do so result in crashing/freezing the kernel)
                        //so send the data to our worker thread.
                        betterSchedulerTargetQueue[targetCurrentCore][k].targetCore = lowestPriorityCore;
                        betterSchedulerTargetQueue[targetCurrentCore][k].thread = target;
                        target->padding |= BETTER_SCHEDULER_DISABLE_SELECTION_MASK;

#if defined(BETTER_SCHEDULER_DEBUG)
                        debug[lowestPriorityCore]++;
#endif //defined(BETTER_SCHEDULER_DEBUG)

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
        KRecursiveLock__Unlock(criticalSectionLock);
    }
}

void BetterSchedulerUpdateInSvcFlag(KThread *thread, bool inSvc)
{
    KRecursiveLock__Lock(criticalSectionLock);

    if(inSvc)
        thread->padding |= BETTER_SCHEDULER_IN_SVC_MASK;
    else
        thread->padding &= ~BETTER_SCHEDULER_IN_SVC_MASK;

    KRecursiveLock__Unlock(criticalSectionLock);
}

void BetterSchedulerRemoveThread(KThread *thread)
{
    KRecursiveLock__Lock(criticalSectionLock);

    for(u8 i = 0; i < betterSchedulerThreads.registeredThreads; i++)
    {
        KThread *target = betterSchedulerThreads.thread[i];

        if(!target)
            continue;

        if(target == thread)
        {
            KAutoObject *obj = (KAutoObject *)target;
            obj->vtable->DecrementReferenceCount(obj);

            //Remove from the list.
            for(u8 k = (i + 1); k < betterSchedulerThreads.registeredThreads; k++)
                betterSchedulerThreads.thread[k - 1] = betterSchedulerThreads.thread[k];

            betterSchedulerThreads.thread[betterSchedulerThreads.registeredThreads - 1] = NULL;
            betterSchedulerThreads.registeredThreads--;
            continue;
        }
    }

    BetterSchedulerCleanUpInvalidThreads();
    KRecursiveLock__Unlock(criticalSectionLock);
}

Result BetterScheduler(u32 op, Handle threadHandle, u32 parameters)
{
    Result result = 0;

    betterSchedulerNumOfCores = getNumberOfCores();

    if(op == BETTER_SCHEDULER_START_SCHEDULER)
    {
        u8 currentCore = getCurrentCoreID();
        KProcessHandleTable *table = handleTableOfProcess(currentCoreContext->objectContext.currentProcess);
        KEvent *event = (KEvent *)KProcessHandleTable__ToKAutoObject(table, parameters);

        //Assign core priority (prefered core for cross core context switch).
        if(betterSchedulerNumOfCores == 2)
        {
            betterSchedulerCorePriority[0] = 0;//User core.
            betterSchedulerCorePriority[1] = 1;//System core.
        }
        else if(betterSchedulerNumOfCores == 4)
        {
            betterSchedulerCorePriority[0] = 2;//User core.
            betterSchedulerCorePriority[1] = 0;//User core.
            betterSchedulerCorePriority[2] = 3;//System core.
            betterSchedulerCorePriority[3] = 1;//System core.
        }

        KRecursiveLock__Lock(criticalSectionLock);

        if(!event)
            result = 0xD8E007F7;//Invalid handle.
        else if(betterSchedulerWorkerThreads[currentCore])
        {
            //Remove reference.
            KAutoObject *obj = (KAutoObject *)event;
            obj->vtable->DecrementReferenceCount(obj);

            result = 0xF8C007F4;//Scheduler thread already exists for this core, not implemented.
        }
        else
        {
            betterSchedulerEvents[currentCore] = event;
            betterSchedulerWorkerThreads[currentCore] = currentCoreContext->objectContext.currentThread;
            betterSchedulerIsReady = BetterSchedulerCheckIsReady(betterSchedulerNumOfCores);
            result = 0;
        }

        KRecursiveLock__Unlock(criticalSectionLock);

        if(result == 0)
        {
            bool wait = false;
            KThread *currentThread = currentCoreContext->objectContext.currentThread;
            KScheduler *currentScheduler = currentCoreContext->objectContext.currentScheduler;

            while(true)
            {
                u8 targetCore = 0;
                KThread *target = NULL;

                if(wait)
                    WaitSynchronization1(NULL, currentThread, (KSynchronizationObject *)&betterSchedulerEvents[currentCore]->syncObject, U64_MAX);

                KRecursiveLock__Lock(criticalSectionLock);

                if(!betterSchedulerWorkerThreads[currentCore])
                {
                    //Remove reference.
                    KAutoObject *obj = (KAutoObject *)betterSchedulerEvents[currentCore];
                    obj->vtable->DecrementReferenceCount(obj);

                    for(u8 i = 0; i < BETTER_SCHEDULER_QUEUE_CAPACITY; i++)
                    {
                        if(betterSchedulerTargetQueue[currentCore][i].thread)
                            betterSchedulerTargetQueue[currentCore][i].thread->padding &= ~BETTER_SCHEDULER_DISABLE_SELECTION_MASK;

                        betterSchedulerTargetQueue[currentCore][i].targetCore = 0;
                        betterSchedulerTargetQueue[currentCore][i].thread = NULL;
                    }

                    betterSchedulerEvents[currentCore] = NULL;
                    betterSchedulerIsReady = BetterSchedulerCheckIsReady(betterSchedulerNumOfCores);

                    KRecursiveLock__Unlock(criticalSectionLock);
                    break;//We must stop now.
                }

                targetCore = betterSchedulerTargetQueue[currentCore][0].targetCore;
                target = betterSchedulerTargetQueue[currentCore][0].thread;

                if(target)
                {
                    bool isValid = false;

                    //Check if thread is still valid.
                    BetterSchedulerCleanUpInvalidThreads();
                    for(u8 i = 0; i < betterSchedulerThreads.registeredThreads; i++)
                    {
                        if(betterSchedulerThreads.thread[i] == target)
                        {
                            isValid = true;
                            break;
                        }
                    }

                    if(isValid)
                    {
                        if(((target->padding & BETTER_SCHEDULER_IN_SVC_MASK) == 0)
                        && target->coreId == currentCore)
                        {
                            //Remove from scheduler, switch core, add to scheduler.
                            target->schedulingMask = 0x00;
                            KScheduler__AdjustThread(currentScheduler, target, 0x01);

                            target->coreId = targetCore;
                            target->padding |= BETTER_SCHEDULER_SWITCHING_MASK;
                            target->padding &= ~BETTER_SCHEDULER_DISABLE_SELECTION_MASK;

                            target->schedulingMask = 0x01;
                            KScheduler__AdjustThread(currentScheduler, target, 0x00);
                        }
                        else
                            target->padding &= ~BETTER_SCHEDULER_DISABLE_SELECTION_MASK;
                    }

                    //Update the queue.
                    for(u8 i = 1; i < BETTER_SCHEDULER_QUEUE_CAPACITY; i++)
                    {
                        betterSchedulerTargetQueue[currentCore][i - 1].targetCore = betterSchedulerTargetQueue[currentCore][i].targetCore;
                        betterSchedulerTargetQueue[currentCore][i - 1].thread = betterSchedulerTargetQueue[currentCore][i].thread;
                    }
                    betterSchedulerTargetQueue[currentCore][BETTER_SCHEDULER_QUEUE_CAPACITY - 1].targetCore = 0;
                    betterSchedulerTargetQueue[currentCore][BETTER_SCHEDULER_QUEUE_CAPACITY - 1].thread = NULL;

                    // SleepThreadInternal(currentThread, NULL, 10000000);//10ms
                }

                wait = (betterSchedulerTargetQueue[currentCore][0].thread ? false : true);

                KRecursiveLock__Unlock(criticalSectionLock);
            }

            result = 0;//Success.
        }
    }
    else if(op == BETTER_SCHEDULER_STOP_SCHEDULER)
    {
        KRecursiveLock__Lock(criticalSectionLock);

        //Stop all schedulers.
        for(u8 i = 0; i < BETTER_SCHEDULER_MAX_CORES; i++)
        {
            betterSchedulerWorkerThreads[i] = NULL;
            if(betterSchedulerEvents[i])
            {
                KEvent *event = (KEvent *)betterSchedulerEvents[i];
                KSynchronizationObject__Signal(&event->syncObject, (event->resetType == RESET_PULSE));
            }
        }

        KRecursiveLock__Unlock(criticalSectionLock);

        result = 0;//Success.
    }
    else if(op == BETTER_SCHEDULER_REGISTER_THREAD
    || op == BETTER_SCHEDULER_SET_AFFINITY_MASK)
    {
        u8 coreMask = 0;
        betterSchedulerNumOfCores = getNumberOfCores();

        if(betterSchedulerNumOfCores > BETTER_SCHEDULER_MAX_CORES)
            betterSchedulerNumOfCores = BETTER_SCHEDULER_MAX_CORES;

        for(u8 i = 0; i < betterSchedulerNumOfCores; i++)
            coreMask |= (1 << i);

        if((parameters & coreMask) != 0)
        {
            KProcessHandleTable *handleTable = handleTableOfProcess(currentCoreContext->objectContext.currentProcess);
            KThread *thread = KProcessHandleTable__ToKThread(handleTable, threadHandle);

            //We have valid mask.
            parameters = (parameters & coreMask);

            KRecursiveLock__Lock(criticalSectionLock);
            BetterSchedulerCleanUpInvalidThreads();

            if(thread)
            {
                if(op == BETTER_SCHEDULER_REGISTER_THREAD)
                {
                    bool isFull = true;
                    bool isRegistered = false;

                    //Check if it's registered.
                    for(u8 i = 0; i < betterSchedulerThreads.registeredThreads; i++)
                    {
                        if(betterSchedulerThreads.thread[i] == thread)
                        {
                            isRegistered = true;
                            break;
                        }
                    }

                    //Register requested thread.
                    if(isRegistered)
                    {
                        //Already registered, close handle and return.
                        KAutoObject *obj = (KAutoObject *)thread;
                        obj->vtable->DecrementReferenceCount(obj);
                        result = 0;//Already registered.
                    }
                    else
                    {
                        for(u8 i = betterSchedulerThreads.registeredThreads; i < BETTER_SCHEDULER_MAX_THREADS; i++)
                        {
                            if(!betterSchedulerThreads.thread[i])
                            {
                                betterSchedulerThreads.thread[i] = thread;
                                betterSchedulerThreads.thread[i]->affinityMask = parameters;
                                betterSchedulerThreads.registeredThreads++;
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
                    bool found = false;

                    for(u8 i = 0; i < betterSchedulerThreads.registeredThreads; i++)
                    {
                        if(betterSchedulerThreads.thread[i] == thread)
                        {
                            found = true;
                            //Set affinity mask.
                            betterSchedulerThreads.thread[i]->affinityMask = parameters;
                            break;
                        }
                    }

                    //Close handle and return.
                    KAutoObject *obj = (KAutoObject *)thread;
                    obj->vtable->DecrementReferenceCount(obj);

                    if(found)
                        result = 0;//Success.
                    else
                        result = 0xD8E007F7;//Thread is NOT registered, invalid handle (for this operation).
                }
            }
            else
                result = 0xD8E007F7;//Invalid handle.

            KRecursiveLock__Unlock(criticalSectionLock);
        }
        else
            result = 0xD8E007ED;//At least 1 processor must be selected, invalid enum (parameters) value.
    }
    else if(op == BETTER_SCHEDULER_UNREGISTER_THREAD)
    {
        KProcessHandleTable *handleTable = handleTableOfProcess(currentCoreContext->objectContext.currentProcess);
        KThread *thread = KProcessHandleTable__ToKThread(handleTable, threadHandle);

        KRecursiveLock__Lock(criticalSectionLock);
        BetterSchedulerCleanUpInvalidThreads();

        if(thread)
        {
            //Remove requested thread if exists.
            for(u8 i = 0; i < betterSchedulerThreads.registeredThreads; i++)
            {
                if(betterSchedulerThreads.thread[i] == thread)
                {
                    KAutoObject *obj = (KAutoObject *)betterSchedulerThreads.thread[i];

                    betterSchedulerThreads.thread[i]->padding &= ~BETTER_SCHEDULER_DISABLE_SELECTION_MASK;
                    betterSchedulerThreads.thread[i]->affinityMask = (1 << betterSchedulerThreads.thread[i]->coreId);
                    obj->vtable->DecrementReferenceCount(obj);

                    for(u8 k = (i + 1); k < betterSchedulerThreads.registeredThreads; k++)
                        betterSchedulerThreads.thread[k - 1] = betterSchedulerThreads.thread[i];

                    betterSchedulerThreads.thread[betterSchedulerThreads.registeredThreads - 1] = NULL;
                    betterSchedulerThreads.registeredThreads--;
                    break;
                }
            }

            //Close handle and return.
            KAutoObject *obj = (KAutoObject *)thread;
            obj->vtable->DecrementReferenceCount(obj);

            result = 0;//Successfully unregisterd or not registered.
        }
        else
            result = 0;//Always return success for unregister request.

        KRecursiveLock__Unlock(criticalSectionLock);
    }
    else if(op == BETTER_SCHEDULER_DEBUG)
    {
#if defined(BETTER_SCHEDULER_DEBUG)
        u32 *dst = (u32 *)threadHandle;
        memcpy(dst, (void *)debug, sizeof(debug));
#endif //defined(BETTER_SCHEDULER_DEBUG)

        result = 0;
    }
    else
        result = 0xF8C007F4;//Not implemented.

    return result;
}

static void BetterSchedulerGetPriorityList(u8 numOfCores, u8 *highestPriorityPerCore)
{
    for(u8 i = 0; i < numOfCores; i++)
    {
        //Get the priority for the thread that is currently running (or about to be executed).
        u8 currentMaxPriority = betterSchedulerCurrentThreads[i]->dynamicPriority;

        //Get current maximum priority for the core including pending (switching) threads.
        for(u8 k = 0; k < numOfCores; k++)
        {
            for(u8 m = 0; m < BETTER_SCHEDULER_QUEUE_CAPACITY; m++)
            {
                KThread *pendingThread = betterSchedulerTargetQueue[k][m].thread;

                if(!pendingThread)
                    break;

                if(betterSchedulerTargetQueue[k][m].targetCore == k)
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
    for(u8 i = 0; i < betterSchedulerThreads.registeredThreads; i++)
    {
        KThread *target = betterSchedulerThreads.thread[i];

        if(!target)
            continue;

        if(target->shallTerminate || target->isEnded || !target->isAlive)
        {
            KAutoObject *obj = (KAutoObject *)target;
            obj->vtable->DecrementReferenceCount(obj);

            //Remove from the list.
            for(u8 k = (i + 1); k < betterSchedulerThreads.registeredThreads; k++)
                betterSchedulerThreads.thread[k - 1] = betterSchedulerThreads.thread[k];

            betterSchedulerThreads.thread[betterSchedulerThreads.registeredThreads - 1] = NULL;
            betterSchedulerThreads.registeredThreads--;
            continue;
        }
    }
}

static bool BetterSchedulerCheckIsReady(u8 numOfCores)
{
    bool isSchedulerReady = true;

    for(u8 i = 0; i < numOfCores; i++)
    {
        if(!betterSchedulerEvents[i])
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

    for(u8 i = 0; i < betterSchedulerThreads.registeredThreads; i++)
    {
        KThread *target = betterSchedulerThreads.thread[i];

        if(betterSchedulerCurrentThreads[target->coreId] == target)
            continue;

        //We seek for the thread that wants to run (scheduled) but couldn't (because of other threads).
        //So, skip if at least one of them is true :
        //1. Target thread is NOT scheduled (schedulingMask != 1).
        //2. Target thread is running.
        //3. Target thread is in SVC (trying to switch between cores here will cause crash).
        //4. Selection is disabled for target thread.
        if((target->schedulingMask != 0x01) || ((target->padding & BETTER_SCHEDULER_RUNNING_MASK) == BETTER_SCHEDULER_RUNNING_MASK)
        || ((target->padding & BETTER_SCHEDULER_IN_SVC_MASK) == BETTER_SCHEDULER_IN_SVC_MASK)
        || ((target->padding & BETTER_SCHEDULER_DISABLE_SELECTION_MASK) == BETTER_SCHEDULER_DISABLE_SELECTION_MASK))
            continue;

        if((target->affinityMask & (1 << currentCore)) == 0)
            continue;//Target thread doesn't like current cocre, do nothing.

        if((target->affinityMask & (1 << target->coreId)) == 0)
        {
            //Target thread doesn't like the core currently running on, skip core and priority check and continue.
            finalTarget = target;
            currentMaxPriority = finalTarget->dynamicPriority;
            break;
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
