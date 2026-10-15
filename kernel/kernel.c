/******************************************************************************
 * File:        kernel.c
 *
 * Author:      Giancarlo Perez (based on the kernel framework by J Losh)
 *
 * Created:     12/7/24
 *
 * Description: Preemptive priority RTOS kernel with MPU-isolated task stacks
 *
 * Tasks run unprivileged on the PSP and reach the kernel through SVC. All
 * kernel state lives in KRAM, which only privileged code can access. The
 * kernel handlers (SVCall, PendSV, SysTick) and every interrupt that calls
 * postFromIsr() run at the same NVIC priority (0, the reset default), so they
 * never preempt each other and don't need critical sections.
 ******************************************************************************/

// Target uC:       TM4C123GH6PM
// System Clock:    40 MHz

//=============================================================================
// INCLUDES
//=============================================================================

#include <stdint.h>
#include <stddef.h>
#include "tm4c123gh6pm.h"
#include "mm.h"
#include "kernel.h"
#include "strlib.h"

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

#define INVALID_TASK 0xFF
#define NUM_PRIORITIES 16

#define EXC_RETURN_THREAD_PSP 0xFFFFFFFD // thread mode, PSP, no FPU context
#define STACK_FILL 0xA5A5A5A5            // paints unused stack for meminfo

// Issues service call num with a0 in R0 and a1 in R1, evaluates to the R0 the
// handler leaves in the stacked frame. The register variables pin the
// arguments to R0/R1 at the SVC instruction.
#define SVC(num, a0, a1) ({                                                     \
    register uint32_t r0_ __asm("r0") = (uint32_t)(a0);                        \
    register uint32_t r1_ __asm("r1") = (uint32_t)(a1);                        \
    __asm volatile("svc %[n]" : "+r"(r0_) : [n] "i"(num), "r"(r1_) : "memory"); \
    r0_;                                                                        \
})

//=============================================================================
// GLOBALS
//=============================================================================

typedef struct _tcb {
    uint8_t state;                 // see STATE_ values in kernel.h
    void* pid;                     // used to uniquely identify thread (add of task fn)
    void* spInit;                  // original top of stack
    void* sp;                      // current stack pointer
    uint8_t priority;              // 0=highest
    uint32_t ticks;                // ticks until sleep or timed wait completes
    uint16_t srd;                  // MPU subregion disable bits
    uint16_t stackSize;            // stack size of task
    uint32_t elapsed[2];           // run time ping-pong buffers for ps
    uint32_t runtime;              // systime when the task was last dispatched
    char name[16];                 // name of task used in ps command
    uint8_t mutex;                 // index of the mutex blocking the thread
    uint8_t semaphore;             // index of the semaphore blocking the thread
    bool timedWait;                // blocked in waitTimeout()
} TCB;

KERNEL_DATA static TCB tcb[MAX_TASKS];
KERNEL_DATA static mutex mutexes[MAX_MUTEXES];
KERNEL_DATA static semaphore semaphores[MAX_SEMAPHORES];
KERNEL_DATA static uint8_t taskCurrent;        // index of last dispatched task
KERNEL_DATA static uint8_t taskCount;          // total number of valid tasks
KERNEL_DATA static bool rtosStarted;
KERNEL_DATA static bool priorityScheduler;     // priority (true) or round-robin (false)
KERNEL_DATA static bool preemption;            // preemption (true) or cooperative (false)
KERNEL_DATA static uint8_t pingpong;           // elapsed[] buffer being written
KERNEL_DATA static uint8_t rrTask;             // last task picked by round-robin
KERNEL_DATA static uint8_t lastAtPrio[NUM_PRIORITIES]; // last task picked per priority

volatile uint32_t systime = 0; // in ticks (ms)

//=============================================================================
// STATIC FUNCTIONS
//=============================================================================

static void requestSwitch(void) {
    NVIC_INT_CTRL_R = NVIC_INT_CTRL_PEND_SV;
}

static uint8_t taskFromPid(uint32_t pid) {
    uint8_t i;
    for (i = 0; i < taskCount; i++) {
        if ((uint32_t)tcb[i].pid == pid) {
            return i;
        }
    }
    return INVALID_TASK;
}

static void enqueue(uint8_t q[], uint8_t* qsize, uint8_t task) {
    q[(*qsize)++] = task;
}

static uint8_t dequeue(uint8_t q[], uint8_t* qsize) {
    uint8_t i, task = q[0];
    for (i = 1; i < *qsize; i++) {
        q[i - 1] = q[i];
    }
    (*qsize)--;
    return task;
}

static void removeFromQueue(uint8_t q[], uint8_t* qsize, uint8_t task) {
    uint8_t i, j;
    for (i = 0, j = 0; i < *qsize; i++) {
        if (q[i] != task) {
            q[j++] = q[i];
        }
    }
    *qsize = j;
}

// Hardware-stacked exception frame (R0 first) of a task that is not running.
// Its saved sp points at r4-r11 and EXC_RETURN, followed by s16-s31 when
// EXC_RETURN bit 4 is clear (the task had an active FPU context).
static uint32_t* taskFrame(uint8_t task) {
    uint32_t* sp = tcb[task].sp;
    return sp + 9 + ((sp[8] & 0x10) ? 0 : 16);
}

// A task function that returns ends up here (initial LR)
static void threadExit(void) {
    stopThread(NULL);
    while (true);
}

// Paints the stack and builds a frame that makes the thread appear as if it
// had been switched out right before its first instruction
static void initStack(uint8_t task, _fn fn) {
    uint32_t* bottom = (uint32_t*)((uint8_t*)tcb[task].spInit - tcb[task].stackSize);
    uint32_t* sp = tcb[task].spInit;
    int8_t r;
    while (bottom < sp) {
        *bottom++ = STACK_FILL;
    }
    *--sp = 0x01000000;                 // xPSR, Thumb bit set
    *--sp = (uint32_t)fn & ~1u;         // PC
    *--sp = (uint32_t)threadExit;       // LR
    *--sp = 12;                         // R12
    *--sp = 3;                          // R3
    *--sp = 2;                          // R2
    *--sp = 1;                          // R1
    *--sp = 0;                          // R0
    *--sp = EXC_RETURN_THREAD_PSP;      // EXC_RETURN, popped into LR by pendsvIsr
    for (r = 11; r >= 4; r--) {
        *--sp = r;                      // R11-R4
    }
    tcb[task].sp = sp;
}

// Gives the task a fresh stack, returns false if the heap is full
static bool allocateStack(uint8_t task, uint32_t stackBytes, _fn fn) {
    uint8_t* stack = malloc_(stackBytes, task);
    if (stack == NULL) {
        return false;
    }
    tcb[task].stackSize = getAllocSize(stack);
    tcb[task].spInit = stack + tcb[task].stackSize;
    tcb[task].srd = createNoSramAccessMask();
    addSramAccessWindow(&tcb[task].srd, stack, tcb[task].stackSize);
    tcb[task].mutex = INVALID_MUTEX;
    tcb[task].semaphore = INVALID_SEMAPHORE;
    tcb[task].timedWait = false;
    initStack(task, fn);
    tcb[task].state = STATE_READY;
    return true;
}

// Deepest stack use in tenths of a percent, from the painted fill pattern
static uint32_t stackUsage(uint8_t task) {
    uint32_t* p = (uint32_t*)((uint8_t*)tcb[task].spInit - tcb[task].stackSize);
    uint32_t unused = 0;
    while (p < (uint32_t*)tcb[task].spInit && *p++ == STACK_FILL) {
        unused += 4;
    }
    return (1000 * (tcb[task].stackSize - unused)) / tcb[task].stackSize;
}

static uint8_t rtosScheduler(void) {
    uint8_t i, task, prio = NUM_PRIORITIES;
    // the idle task (index 0) is always ready, so both loops terminate
    if (priorityScheduler) {
        for (i = 0; i < taskCount; i++) {
            if (tcb[i].state == STATE_READY && tcb[i].priority < prio) {
                prio = tcb[i].priority;
            }
        }
        // round-robin among the ready tasks at the highest priority
        task = lastAtPrio[prio];
        do {
            task = (task + 1) % taskCount;
        } while (tcb[task].state != STATE_READY || tcb[task].priority != prio);
        lastAtPrio[prio] = task;
    }
    else {
        task = rrTask;
        do {
            task = (task + 1) % taskCount;
        } while (tcb[task].state != STATE_READY);
        rrTask = task;
    }
    return task;
}

// Hands a mutex to the next waiting task, or frees it
static void releaseMutex(uint8_t m) {
    mutex* mx = &mutexes[m];
    if (mx->queueSize > 0) {
        uint8_t next = dequeue(mx->processQueue, &mx->queueSize);
        mx->lockedBy = next;
        tcb[next].mutex = INVALID_MUTEX;
        tcb[next].state = STATE_READY;
        requestSwitch();
    }
    else {
        mx->lock = false;
    }
}

// Wakes the first task waiting on the semaphore, or increments its count
static void postSemaphore(uint8_t s) {
    semaphore* sem = &semaphores[s];
    if (sem->queueSize > 0) {
        uint8_t next = dequeue(sem->processQueue, &sem->queueSize);
        if (tcb[next].timedWait) {
            taskFrame(next)[0] = true; // waitTimeout() returns true
            tcb[next].timedWait = false;
        }
        tcb[next].semaphore = INVALID_SEMAPHORE;
        tcb[next].state = STATE_READY;
        requestSwitch();
    }
    else if (sem->count < UINT8_MAX) {
        sem->count++;
    }
}

//=============================================================================
// PUBLIC FUNCTIONS
//=============================================================================

uint32_t getCurrentTask(void) {
    return taskCurrent;
}

uint32_t getCurrentPid(void) {
    return (uint32_t)tcb[taskCurrent].pid;
}

uint32_t getSysTime(void) {
    return systime;
}

bool initMutex(uint8_t mutex) {
    bool ok = (mutex < MAX_MUTEXES);
    if (ok) {
        mutexes[mutex].lock = false;
        mutexes[mutex].lockedBy = 0;
        mutexes[mutex].queueSize = 0;
    }
    return ok;
}

bool initSemaphore(uint8_t semaphore, uint8_t count) {
    bool ok = (semaphore < MAX_SEMAPHORES);
    if (ok) {
        semaphores[semaphore].count = count;
        semaphores[semaphore].queueSize = 0;
    }
    return ok;
}

void initRtos(void) {
    uint8_t i;
    taskCount = 0;
    priorityScheduler = true;
    preemption = true;
    for (i = 0; i < MAX_TASKS; i++) {
        tcb[i].state = STATE_INVALID;
        tcb[i].pid = 0;
    }
}

// The first thread created must be the idle task: it has to stay ready at all
// times and can't be killed.
bool createThread(_fn fn, const char name[], uint8_t priority, uint32_t stackBytes) {
    uint8_t i = taskCount;
    if (rtosStarted || i >= MAX_TASKS || priority >= NUM_PRIORITIES
        || str_length(name) >= sizeof(tcb[i].name) || taskFromPid((uint32_t)fn) != INVALID_TASK) {
        return false;
    }
    str_copy(tcb[i].name, name);
    tcb[i].pid = fn;
    tcb[i].priority = priority;
    tcb[i].elapsed[0] = 0;
    tcb[i].elapsed[1] = 0;
    if (!allocateStack(i, stackBytes, fn)) {
        tcb[i].state = STATE_INVALID;
        tcb[i].pid = 0;
        return false;
    }
    taskCount++;
    return true;
}

// Starts the scheduler from main(). The service call is made from privileged
// thread mode on the MSP; the first PendSV then drops into the first task
// (unprivileged, PSP) and the main stack is only used by handlers afterwards.
void startRtos(void) {
    SVC(SVC_START, 0, 0);
    while (true);
}

// Called from the SVCall handler (privileged), returns 1 if the task was stopped
int32_t kill_proc(uint32_t pid) {
    uint8_t i = taskFromPid(pid);
    uint8_t m, j;
    if (i == INVALID_TASK || i == 0 || tcb[i].state == STATE_STOPPED) {
        return 0;
    }
    for (m = 0; m < MAX_MUTEXES; m++) {
        if (mutexes[m].lock && mutexes[m].lockedBy == i) {
            releaseMutex(m);
        }
    }
    if (tcb[i].state == STATE_BLOCKED_MUTEX) {
        removeFromQueue(mutexes[tcb[i].mutex].processQueue, &mutexes[tcb[i].mutex].queueSize, i);
    }
    if (tcb[i].state == STATE_BLOCKED_SEMAPHORE) {
        removeFromQueue(semaphores[tcb[i].semaphore].processQueue, &semaphores[tcb[i].semaphore].queueSize, i);
    }
    for (j = 0; j < MAX_ALLOCS; j++) {
        if (allocTable[j].valid && allocTable[j].owner == i) {
            free_to_heap(allocTable[j].ptr);
        }
    }
    tcb[i].state = STATE_STOPPED;
    if (i == taskCurrent) {
        requestSwitch();
    }
    return 1;
}

void yield(void) {
    SVC(SVC_YIELD, 0, 0);
}

void sleep(uint32_t tick) {
    SVC(SVC_SLEEP, tick, 0);
}

void lock(uint8_t mutex) {
    SVC(SVC_LOCK, mutex, 0);
}

void unlock(uint8_t mutex) {
    SVC(SVC_UNLOCK, mutex, 0);
}

void wait(uint8_t semaphore) {
    SVC(SVC_WAIT, semaphore, 0);
}

bool waitTimeout(uint8_t semaphore, uint32_t ticks) {
    return SVC(SVC_WAIT_TIMEOUT, semaphore, ticks);
}

void post(uint8_t semaphore) {
    SVC(SVC_POST, semaphore, 0);
}

uint32_t stopThread(_fn fn) {
    return SVC(SVC_STOPTHREAD, fn, 0);
}

uint32_t restartThread(_fn fn) {
    return SVC(SVC_RESTARTTHREAD, fn, 0);
}

void setThreadPriority(_fn fn, uint8_t priority) {
    SVC(SVC_SETPRIORITY, fn, priority);
}

void* mallocFromHeap(uint32_t size) {
    return (void*)SVC(SVC_MALLOC, size, 0);
}

void getProcessInfo(psInfo info[MAX_TASKS]) {
    SVC(SVC_PS, info, 0);
}

void getIpcsInfo(ipcsInfo* info) {
    SVC(SVC_IPCS, info, 0);
}

void getMemInfo(memInfo* info) {
    SVC(SVC_MEMINFO, info, 0);
}

uint32_t pidof(const char name[]) {
    return SVC(SVC_PIDOF, name, 0);
}

void setScheduler(bool priority) {
    SVC(SVC_PRIO, priority, 0);
}

void setPreemption(bool on) {
    SVC(SVC_PREEMPTION, on, 0);
}

void reboot(void) {
    SVC(SVC_REBOOT, 0, 0);
}

// Same as post(), for interrupt handlers
void postFromIsr(uint8_t semaphore) {
    if (semaphore < MAX_SEMAPHORES) {
        postSemaphore(semaphore);
    }
}

void sysTickIsr(void) {
    bool is1sec;
    uint8_t i;
    systime++;
    is1sec = (systime % (PS_REFRESH_TIME * 1000)) == 0;
    for (i = 0; i < taskCount; i++) {
        if (tcb[i].state == STATE_DELAYED
            || (tcb[i].state == STATE_BLOCKED_SEMAPHORE && tcb[i].timedWait)) {
            if (tcb[i].ticks == 0) {
                if (tcb[i].state == STATE_BLOCKED_SEMAPHORE) {
                    // timed out, waitTimeout() returns the false already in the frame
                    semaphore* sem = &semaphores[tcb[i].semaphore];
                    removeFromQueue(sem->processQueue, &sem->queueSize, i);
                    tcb[i].semaphore = INVALID_SEMAPHORE;
                    tcb[i].timedWait = false;
                }
                tcb[i].state = STATE_READY;
            }
            else {
                tcb[i].ticks--;
            }
        }
        if (is1sec) {
            tcb[i].elapsed[!pingpong] = 0;
        }
    }
    if (is1sec) {
        pingpong ^= 1;
    }
    if (preemption) {
        requestSwitch();
    }
}

// Called by pendsvIsr with the outgoing task's saved stack pointer (NULL on
// the first dispatch from startRtos), returns the stack pointer to restore
__attribute__((used)) uint32_t* switchTask(uint32_t* sp) {
    if (sp != NULL) {
        tcb[taskCurrent].sp = sp;
        tcb[taskCurrent].elapsed[pingpong] += systime - tcb[taskCurrent].runtime;
    }
    taskCurrent = rtosScheduler();
    tcb[taskCurrent].runtime = systime;
    applySramAccessMask(tcb[taskCurrent].srd);
    return tcb[taskCurrent].sp;
}

// Saves r4-r11 and EXC_RETURN (plus s16-s31 if the task used the FPU) on the
// outgoing task's stack and restores the next task's the same way. The FPU
// store also triggers the lazy save of s0-s15 into the hardware frame.
__attribute__((naked)) void pendsvIsr(void) {
    __asm volatile(
        "   mrs     r0, psp             \n"
        "   tst     lr, #4              \n" // came from the MSP: first dispatch,
        "   bne     1f                  \n" // nothing to save
        "   movs    r0, #0              \n"
        "   b       2f                  \n"
        "1: tst     lr, #0x10           \n"
        "   it      eq                  \n"
        "   vstmdbeq r0!, {s16-s31}     \n"
        "   stmdb   r0!, {r4-r11, lr}   \n"
        "2: bl      switchTask          \n"
        "   ldmia   r0!, {r4-r11, lr}   \n"
        "   tst     lr, #0x10           \n"
        "   it      eq                  \n"
        "   vldmiaeq r0!, {s16-s31}     \n"
        "   msr     psp, r0             \n"
        "   bx      lr                  \n"
    );
}

// frame: R0, R1, R2, R3, R12, LR, PC, xPSR as stacked by the caller's SVC
__attribute__((used)) void svCallHandler(uint32_t* frame) {
    uint8_t svcNum = ((uint8_t*)frame[6])[-2]; // SVC instruction is 2 bytes behind PC
    uint32_t a0 = frame[0];
    uint32_t a1 = frame[1];
    uint32_t i, j;
    switch (svcNum) {
    case SVC_START:
        // unprivileged thread mode from now on, the PendSV return selects the PSP
        __asm volatile("msr control, %0\n isb" :: "r"(1) : "memory");
        rtosStarted = true;
        NVIC_ST_RELOAD_R = 40000 - 1; // 1 ms
        NVIC_ST_CURRENT_R = 0;
        NVIC_ST_CTRL_R = NVIC_ST_CTRL_INTEN | NVIC_ST_CTRL_CLK_SRC | NVIC_ST_CTRL_ENABLE;
        requestSwitch();
        break;
    case SVC_YIELD:
        requestSwitch();
        break;
    case SVC_SLEEP:
        tcb[taskCurrent].ticks = a0;
        tcb[taskCurrent].state = STATE_DELAYED;
        requestSwitch();
        break;
    case SVC_LOCK:
        if (a0 < MAX_MUTEXES) {
            mutex* mx = &mutexes[a0];
            if (!mx->lock) {
                mx->lock = true;
                mx->lockedBy = taskCurrent;
            }
            else if (mx->lockedBy != taskCurrent) {
                enqueue(mx->processQueue, &mx->queueSize, taskCurrent);
                tcb[taskCurrent].mutex = a0;
                tcb[taskCurrent].state = STATE_BLOCKED_MUTEX;
                requestSwitch();
            }
        }
        break;
    case SVC_UNLOCK:
        if (a0 < MAX_MUTEXES && mutexes[a0].lock && mutexes[a0].lockedBy == taskCurrent) {
            releaseMutex(a0);
        }
        break;
    case SVC_WAIT:
    case SVC_WAIT_TIMEOUT:
        if (a0 < MAX_SEMAPHORES) {
            semaphore* sem = &semaphores[a0];
            if (sem->count > 0) {
                sem->count--;
                frame[0] = true;
            }
            else if (svcNum == SVC_WAIT_TIMEOUT && a1 == 0) {
                frame[0] = false;
            }
            else {
                enqueue(sem->processQueue, &sem->queueSize, taskCurrent);
                tcb[taskCurrent].semaphore = a0;
                tcb[taskCurrent].timedWait = (svcNum == SVC_WAIT_TIMEOUT);
                tcb[taskCurrent].ticks = a1;
                tcb[taskCurrent].state = STATE_BLOCKED_SEMAPHORE;
                frame[0] = false; // overwritten by postSemaphore() if posted in time
                requestSwitch();
            }
        }
        break;
    case SVC_POST:
        if (a0 < MAX_SEMAPHORES) {
            postSemaphore(a0);
        }
        break;
    case SVC_PRIO:
        priorityScheduler = a0;
        break;
    case SVC_PREEMPTION:
        preemption = a0;
        break;
    case SVC_PS: {
        psInfo* info = (psInfo*)a0;
        for (i = 0; i < taskCount; i++) {
            info[i].pid = tcb[i].pid;
            str_copy(info[i].name, tcb[i].name);
            info[i].runtime = tcb[i].elapsed[!pingpong];
            info[i].state = tcb[i].state;
            info[i].prio = tcb[i].priority;
            if (tcb[i].state == STATE_BLOCKED_MUTEX) {
                info[i].mutex_or_sem = tcb[i].mutex;
            }
            else if (tcb[i].state == STATE_BLOCKED_SEMAPHORE) {
                info[i].mutex_or_sem = tcb[i].semaphore;
            }
            else {
                info[i].mutex_or_sem = 0xFF;
            }
        }
        break;
    }
    case SVC_IPCS: {
        ipcsInfo* info = (ipcsInfo*)a0;
        for (i = 0; i < MAX_TASKS; i++) {
            str_copy(info->nameArr[i], tcb[i].state != STATE_INVALID ? tcb[i].name : "INVALID_TASK");
        }
        for (i = 0; i < MAX_MUTEXES; i++) {
            info->mutexes[i] = mutexes[i];
        }
        for (i = 0; i < MAX_SEMAPHORES; i++) {
            info->semaphores[i] = semaphores[i];
        }
        break;
    }
    case SVC_PIDOF:
        frame[0] = 0;
        for (i = 0; i < taskCount; i++) {
            if (str_equal(tcb[i].name, (const char*)a0)) {
                frame[0] = (uint32_t)tcb[i].pid;
            }
        }
        break;
    case SVC_MEMINFO: {
        memInfo* info = (memInfo*)a0;
        for (i = 0; i < MAX_ALLOCS; i++) {
            info[i].valid = allocTable[i].valid;
            if (allocTable[i].valid) {
                j = allocTable[i].owner;
                str_copy(info[i].ownerName, tcb[j].name);
                info[i].baseAdd = allocTable[i].ptr;
                info[i].size = allocTable[i].size;
                // only the task's stack allocation has a usage figure
                bool isStack = (uint8_t*)allocTable[i].ptr + allocTable[i].size == tcb[j].spInit;
                info[i].usage = isStack ? stackUsage(j) : 0;
            }
        }
        break;
    }
    case SVC_STOPTHREAD:
        frame[0] = kill_proc(a0 != 0 ? a0 : (uint32_t)tcb[taskCurrent].pid);
        break;
    case SVC_MALLOC: {
        void* p = malloc_(a0, taskCurrent);
        if (p != NULL) {
            addSramAccessWindow(&tcb[taskCurrent].srd, p, getAllocSize(p));
            applySramAccessMask(tcb[taskCurrent].srd);
        }
        frame[0] = (uint32_t)p;
        break;
    }
    case SVC_RESTARTTHREAD:
        i = taskFromPid(a0);
        frame[0] = 0;
        if (i != INVALID_TASK && tcb[i].state == STATE_STOPPED) {
            frame[0] = allocateStack(i, tcb[i].stackSize, (_fn)a0);
        }
        break;
    case SVC_SETPRIORITY:
        i = taskFromPid(a0);
        if (i != INVALID_TASK && a1 < NUM_PRIORITIES) {
            tcb[i].priority = a1;
            requestSwitch();
        }
        break;
    case SVC_REBOOT:
        NVIC_APINT_R = NVIC_APINT_VECTKEY | NVIC_APINT_SYSRESETREQ;
        break;
    }
}

// Passes the stack the SVC frame was pushed on: MSP for startRtos(), PSP for tasks
__attribute__((naked)) void svCallIsr(void) {
    __asm volatile(
        "   tst     lr, #4              \n"
        "   ite     eq                  \n"
        "   mrseq   r0, msp             \n"
        "   mrsne   r0, psp             \n"
        "   b       svCallHandler       \n"
    );
}
