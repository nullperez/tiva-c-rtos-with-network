/******************************************************************************
 * File:        kernel.h
 *
 * Author:      Giancarlo Perez (based on the kernel framework by J Losh)
 *
 * Created:     12/7/24
 *
 * Description: Preemptive priority RTOS kernel with MPU-isolated task stacks
 ******************************************************************************/

// Target uC:       TM4C123GH6PM
// System Clock:    40 MHz

#ifndef KERNEL_H_
#define KERNEL_H_

//=============================================================================
// INCLUDES
//=============================================================================

#include <stdint.h>
#include <stdbool.h>

//=============================================================================
// DEFINES AND MACROS
//=============================================================================

// Places kernel data in the privileged-only KRAM area (see the linker script)
#define KERNEL_DATA __attribute__((section(".bss.kernel")))

// mutexes
#define MAX_MUTEXES 2
#define MAX_MUTEX_QUEUE_SIZE MAX_TASKS
#define NET_QUEUE_MUTEX 0   // guards the netCall() request queue
#define NET_SYNC_MUTEX  1   // one netCallSync() in flight at a time

// semaphores
#define MAX_SEMAPHORES 5
#define MAX_SEMAPHORE_QUEUE_SIZE MAX_TASKS
#define NET_EVENT     0     // wakes the network task (packet, request, ...)
#define NET_SLOTS     1     // free entries in the netCall() request queue
#define NET_CALL_DONE 2     // a netCallSync() request has run
#define MQTT_RECEIVED 3     // example app: an MQTT message is waiting
#define UART_RX       4     // UART0 received characters for the shell

// tasks
#define MAX_TASKS 8
#define PS_REFRESH_TIME 1

// task states
#define STATE_INVALID           0 // no task
#define STATE_STOPPED           1 // stopped, all memory freed
#define STATE_READY             2 // has run, can resume at any time
#define STATE_DELAYED           3 // has run, but now awaiting timer
#define STATE_BLOCKED_MUTEX     4 // has run, but now blocked by mutex
#define STATE_BLOCKED_SEMAPHORE 5 // has run, but now blocked by semaphore

// sv calls
#define SVC_START           0x00
#define SVC_YIELD           0x01
#define SVC_SLEEP           0x02
#define SVC_LOCK            0x03
#define SVC_UNLOCK          0x04
#define SVC_WAIT            0x05
#define SVC_POST            0x06
#define SVC_PRIO            0x07
#define SVC_PREEMPTION      0x09
#define SVC_PS              0x0A
#define SVC_IPCS            0x0B
#define SVC_PIDOF           0x0C
#define SVC_MEMINFO         0x0D
#define SVC_STOPTHREAD      0x0E
#define SVC_MALLOC          0x0F
#define SVC_RESTARTTHREAD   0x10
#define SVC_SETPRIORITY     0x11
#define SVC_WAIT_TIMEOUT    0x12
#define SVC_REBOOT          0xFF

#define INVALID_MUTEX 0xFF
#define INVALID_SEMAPHORE 0xFF

//=============================================================================
// TYPEDEFS AND GLOBALS
//=============================================================================

// function pointer
typedef void (*_fn)();

typedef struct _mutex {
    bool lock;
    uint8_t queueSize;
    uint8_t processQueue[MAX_MUTEX_QUEUE_SIZE];
    uint8_t lockedBy;
} mutex;

typedef struct _semaphore {
    uint8_t count;
    uint8_t queueSize;
    uint8_t processQueue[MAX_SEMAPHORE_QUEUE_SIZE];
} semaphore;

// ps SVC will write to this struct then return it back to caller
typedef struct _psInfo {
    void* pid;
    char name[16];
    uint32_t prio;
    uint32_t runtime; // ticks spent running in the last PS_REFRESH_TIME second(s)
    uint8_t state;
    uint8_t mutex_or_sem; // if blocked mutex -> mutex, else if blocked semaphore -> semaphore
} psInfo;

typedef struct _ipcsInfo {
    mutex mutexes[MAX_MUTEXES];
    semaphore semaphores[MAX_SEMAPHORES];
    char nameArr[MAX_TASKS][16];
} ipcsInfo;

typedef struct _memInfo {
    char ownerName[16];
    uint8_t valid;
    void* baseAdd;
    uint32_t size;
    uint32_t usage; // stack usage in tenths of a percent (stack allocations only)
} memInfo;

// Milliseconds since startRtos(). Kept outside of KRAM so tasks can read it.
extern volatile uint32_t systime;

//=============================================================================
// FUNCTION PROTOTYPES
//=============================================================================

// Setup, called from main() before startRtos() (privileged, MSP)
bool initMutex(uint8_t mutex);
bool initSemaphore(uint8_t semaphore, uint8_t count);
void initRtos(void);
void startRtos(void); // never returns
bool createThread(_fn fn, const char name[], uint8_t priority, uint32_t stackBytes);

// Task API (unprivileged, implemented as service calls).
// None of these may be called from an interrupt; use postFromIsr() there.
void yield(void);
void sleep(uint32_t tick);
void lock(uint8_t mutex);
void unlock(uint8_t mutex);
void wait(uint8_t semaphore);
bool waitTimeout(uint8_t semaphore, uint32_t ticks); // false if it timed out
void post(uint8_t semaphore);
uint32_t stopThread(_fn fn);    // fn = NULL stops the calling task
uint32_t restartThread(_fn fn);
void setThreadPriority(_fn fn, uint8_t priority);
void* mallocFromHeap(uint32_t size);
void getProcessInfo(psInfo info[MAX_TASKS]);
void getIpcsInfo(ipcsInfo* info);
void getMemInfo(memInfo* info); // info must hold MAX_ALLOCS entries
uint32_t pidof(const char name[]);
void setScheduler(bool priority);
void setPreemption(bool on);
void reboot(void);
uint32_t getSysTime(void);

// Interrupt API
void postFromIsr(uint8_t semaphore);

// Kernel internals, used by the fault handlers
uint32_t getCurrentTask(void);
uint32_t getCurrentPid(void);
int32_t kill_proc(uint32_t pid);

// Exception handlers (vector table)
void sysTickIsr(void);
void pendsvIsr(void);
void svCallIsr(void);

#endif
