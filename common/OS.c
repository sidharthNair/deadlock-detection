// *************os.c**************
// EE445M/EE380L.6 Labs 1, 2, 3, and 4
// High-level OS functions
// Students will implement these functions as part of Lab
// Runs on LM4F120/TM4C123
// Jonathan W. Valvano
// Jan 12, 2020, valvano@mail.utexas.edu

#include <stdint.h>
#include <stdio.h>
#include "../inc/tm4c123gh6pm.h"
#include "../inc/CortexM.h"
#include "../inc/PLL.h"
#include "../inc/LaunchPad.h"
#include "../inc/Timer0A.h"
#include "../inc/Timer1A.h"
#include "../inc/Timer2A.h"
#include "../inc/Timer3A.h"
#include "../inc/Timer4A.h"
#include "../inc/WTimer0A.h"
#include "../inc/ADCT0ATrigger.h"
#include "../common/OS.h"
#include "../common/ST7735.h"
#include "../common/UART0int.h"
#include "../common/eFile.h"
#include "../common/heap.h"

// ASM Function Declarations
void ContextSwitch(void);
void StartOS(void);

// Performance Measurements
int32_t MaxJitter; // largest time jitter between interrupts in usec
#define JITTERSIZE 64
uint32_t const JitterSize = JITTERSIZE;
uint32_t JitterHistogram[JITTERSIZE] = {
    0,
};

#define MEASURE_PERIODIC_JITTER 1
int32_t PeriodicJitter1;
uint32_t PeriodicJitterHist1[JITTERSIZE] = {
    0,
};
int32_t PeriodicJitter2;
uint32_t PeriodicJitterHist2[JITTERSIZE] = {
    0,
};

#define MEASURE_CRITICAL 1
uint32_t t1, dt;
uint32_t start_time;
uint32_t SumCritical = 0;
uint32_t MaxCritical = 0;
#if (MEASURE_CRITICAL)
#define OSCRITICAL_ENTER()    \
    {                         \
        sr = StartCritical(); \
        t1 = OS_Time();       \
    }
#define OSCRITICAL_EXIT()                      \
    {                                          \
        dt = OS_TimeDifference(t1, OS_Time()); \
        SumCritical += dt;                     \
        if (dt > MaxCritical)                  \
        {                                      \
            MaxCritical = dt;                  \
        }                                      \
        EndCritical(sr);                       \
    }
#else
#define OSCRITICAL_ENTER()    \
    {                         \
        sr = StartCritical(); \
    }
#define OSCRITICAL_EXIT() \
    {                     \
        EndCritical(sr);  \
    }
#endif

volatile uint32_t IdleCountRef = 0;

// TCBs
uint32_t num_created = 0;
uint32_t num_killed = 0;
TCB *RunPt = NULL;
TCB *NextPt = NULL;
TCB tcb_pool[MAX_THREADS];
uint32_t stack_pool[MAX_THREADS][STACK_SIZE];

// PCBs
PCB pcb_pool[MAX_PROCESSES];

// Priority Lists
TCB *PriorityPts[PRIORITY_LEVELS];

// Adds an element to the end of the list and returns the head
TCB *tcb_list_add(TCB *head, TCB *elem)
{
    if (elem == NULL)
    {
        return head;
    }
    if (head == NULL)
    {
        // Initialize the list
        head = elem;
        head->next = head;
        head->prev = head;
    }
    else
    {
        // Add elem to the end of the list
        elem->next = head;
        elem->prev = head->prev;
        head->prev->next = elem;
        head->prev = elem;
    }
    return head;
}

// Removes an element from the front of the list and returns the new head
TCB *tcb_list_remove(TCB *head)
{
    if (head == NULL || (head == head->next))
    {
        return NULL;
    }

    // Remove head from the list
    head->prev->next = head->next;
    head->next->prev = head->prev;

    // Return new head
    return head->next;
}

// Fifo
#define FIFOSIZE 32
Sema4Type CurrentSize;
Sema4Type FIFOmutex;      // exclusive access to FIFO
uint32_t volatile *PutPt; // put next
uint32_t volatile *GetPt; // get next
uint32_t static Fifo[FIFOSIZE];

// Mailbox
Sema4Type BoxFree;
Sema4Type MailValid;
uint32_t Mail;
/*------------------------------------------------------------------------------
  Systick Interrupt Handler
  SysTick interrupt happens every 10 ms
  used for preemptive thread switch
 *------------------------------------------------------------------------------*/
void SysTick_Handler(void)
{
    OS_Suspend();
}

unsigned long OS_LockScheduler(void)
{
    // lab 4 might need this for disk formating
    return 0; // replace with solution
}
void OS_UnLockScheduler(unsigned long previous)
{
    // lab 4 might need this for disk formating
}

void SysTick_Init(unsigned long period)
{
    // defined in OS_Launch()
}

volatile uint32_t calibrate_flag = 0;
void Timer2Calibrate()
{
    calibrate_flag = 1;
}
void Timer2Dummy() {}

#if (DEADLOCK_DETECTION)
int CheckForDeadlocks(TCB *thread)
{
    printf("checking for deadlocks starting from thread %d\r\n", thread->id);
    for (uint32_t tid = 0; tid < MAX_THREADS; tid++)
    {
        tcb_pool[tid].visited = 0;
    }

    TCB *curr = thread;
    int found_cycle = 0;
    while (curr->status == BLOCKED &&
           curr->LockPt != NULL)
    {
        curr->visited = 1;
        curr = curr->LockPt->holder;
        if (curr->visited)
        {
            found_cycle = 1;
            break;
        }
    }

    if (found_cycle)
    {
        printf("detected cycle: ");
        thread = curr;
        do
        {
            printf("%d -> ", curr->id);
            OS_Kill_Thread(curr->id);
            curr = curr->LockPt->holder;
        } while (curr != thread);
        printf("%d\r\n", curr->id);
        printf("killed all threads in cycle\r\n");
    }

    return found_cycle;
}

void DeadlockTask()
{
    for (uint32_t tid = 0; tid < MAX_THREADS; tid++)
    {
        if (tcb_pool[tid].status == BLOCKED &&
            tcb_pool[tid].LockPt != NULL &&
            OS_TimeDifference(tcb_pool[tid].lockStart, OS_MsTime()) > (TIME_1MS * 3000))
        {
            // Thread has been waiting for a lock for over 3 seconds -- check for cycle
            if (CheckForDeadlocks(&tcb_pool[tid]))
            {
                break;
            }
        }
    }
}
#endif

/**
 * @details  Initialize operating system, disable interrupts until OS_Launch.
 * Initialize OS controlled I/O: serial, ADC, systick, LaunchPad I/O and timers.
 * Interrupts not yet enabled.
 * @param  none
 * @return none
 * @brief  Initialize OS
 */
void OS_Init(void)
{
    PLL_Init(Bus80MHz);
    LaunchPad_Init();
    Timer2A_Init(&Timer2Calibrate, TIME_1MS * 10, 7);
    while (!calibrate_flag)
    {
        IdleCountRef++;
    }
    Timer2A_Init(&Timer2Dummy, 0xFFFFFFFF, 7);
    UART_Init();
    ST7735_InitR(INITR_REDTAB);
    Heap_Init();

#if (DEADLOCK_DETECTION)
    Timer0A_Init(&DeadlockTask, (TIME_1MS * 3000), 7);
#endif

    DisableInterrupts();

    for (uint32_t i = 0; i < PRIORITY_LEVELS; i++)
    {
        PriorityPts[i] = NULL;
    }
};

// ******** OS_InitSemaphore ************
// initialize semaphore
// input:  pointer to a semaphore
// output: none
void OS_InitSemaphore(Sema4Type *semaPt, int32_t value)
{
    semaPt->Value = value;
    for (uint32_t i = 0; i < PRIORITY_LEVELS; i++)
    {
        semaPt->BlockedPts[i] = NULL;
    }
};

// ******** OS_Wait ************
// decrement semaphore
// Lab2 spinlock
// Lab3 block if less than zero
// input:  pointer to a counting semaphore
// output: none
void OS_Wait(Sema4Type *semaPt)
{
    int32_t sr;
    OSCRITICAL_ENTER();
    semaPt->Value -= 1;
    if (semaPt->Value < 0)
    {
        RunPt->status = BLOCKED;
        RunPt->SemaPt = semaPt;
        // Remove thread from priority lists
        PriorityPts[RunPt->priority] = tcb_list_remove(RunPt);
        if (PriorityPts[RunPt->priority] != NULL)
        {
            PriorityPts[RunPt->priority] = PriorityPts[RunPt->priority]->prev;
        }
        // Add thread to semaphore blocked lists
        semaPt->BlockedPts[RunPt->priority] = tcb_list_add(semaPt->BlockedPts[RunPt->priority], RunPt);
        OSCRITICAL_EXIT();
        OS_Suspend(); // Force context switch
        OSCRITICAL_ENTER();
    }
    OSCRITICAL_EXIT();
};

// ******** OS_Signal ************
// increment semaphore
// Lab2 spinlock
// Lab3 wakeup blocked thread if appropriate
// input:  pointer to a counting semaphore
// output: none
void OS_Signal(Sema4Type *semaPt)
{
    int32_t sr;
    OSCRITICAL_ENTER();
    semaPt->Value += 1;
    if (semaPt->Value <= 0)
    {
        uint32_t priority;
        for (priority = 0; priority < PRIORITY_LEVELS; priority++)
        {
            if (semaPt->BlockedPts[priority] != NULL)
            {
                // Remove first thread in semaphore blocked list
                TCB *thread = semaPt->BlockedPts[priority];
                semaPt->BlockedPts[priority] = tcb_list_remove(semaPt->BlockedPts[priority]);

                // Update thread
                thread->status = ACTIVE;
                thread->SemaPt = NULL;

                // Add blocked thread back to priority list
                PriorityPts[priority] = tcb_list_add(PriorityPts[priority], thread);

                break;
            }
        }
    }
    OSCRITICAL_EXIT();
};

void OS_SignalAll(Sema4Type *semaPt)
{
    int32_t sr;
    OSCRITICAL_ENTER();
    while (semaPt->Value < 0)
    {
        OS_Signal(semaPt);
    }
    OSCRITICAL_EXIT();
}

// ******** OS_bWait ************
// Lab2 spinlock, set to 0
// Lab3 block if less than zero
// input:  pointer to a binary semaphore
// output: none
void OS_bWait(Sema4Type *semaPt)
{
    OS_Wait(semaPt);
};

// ******** OS_bSignal ************
// Lab2 spinlock, set to 1
// Lab3 wakeup blocked thread if appropriate
// input:  pointer to a binary semaphore
// output: none
void OS_bSignal(Sema4Type *semaPt)
{
    OS_Signal(semaPt);
};

#if (DEADLOCK_DETECTION)
// Adds an element to the list and returns the head
Lock *lock_list_append(Lock *head, Lock *elem)
{
    if (head == NULL)
    {
        return elem;
    }

    Lock *curr = head;
    while (curr->next != NULL)
    {
        curr = curr->next;
    }
    curr->next = elem;
    elem->next = NULL;

    return head;
}

// Removes an element from the list and returns the new head
Lock *lock_list_remove(Lock *head, Lock *elem)
{
    if (head == NULL)
    {
        return NULL;
    }

    if (elem == NULL)
    {
        return head;
    }

    if (head == elem)
    {
        return head->next;
    }

    Lock *curr = head;
    Lock *prev = head;
    while (curr != NULL || curr != elem)
    {
        prev = curr;
        curr = curr->next;
    }

    if (curr != NULL)
    {
        prev->next = curr->next;
        curr->next = NULL;
    }

    return head;
}
#endif

// Initializes a Lock instance
void OS_InitLock(Lock *lock)
{
    OS_InitSemaphore(&(lock->sema), 1);
    lock->holder = NULL;
    lock->next = NULL;
}

// Acquires the lock
void OS_LockAcquire(Lock *lock)
{
#if (DEADLOCK_DETECTION)
    RunPt->lockStart = OS_Time();
    RunPt->LockPt = lock;
#endif
    OS_Wait(&(lock->sema));
    lock->holder = RunPt;

#if (DEADLOCK_DETECTION)
    RunPt->lockStart = 0;
    RunPt->LockPt = NULL;
    RunPt->acquired = lock_list_append(RunPt->acquired, lock);
#endif
}

// Releases the lock
int OS_LockRelease(Lock *lock)
{
    if (lock->holder != RunPt)
    {
        return 1;
    }

    lock->holder = NULL;

#if (DEADLOCK_DETECTION)
    RunPt->acquired = lock_list_remove(RunPt->acquired, lock);
#endif
    OS_Signal(&(lock->sema));
    return 0;
}

//******** OS_AddThread ***************
// add a foregound thread to the scheduler
// Inputs: pointer to a void/void foreground task
//         number of bytes allocated for its stack
//         priority, 0 is highest, 5 is the lowest
// Outputs: 1 if successful, 0 if this thread can not be added
// stack size must be divisable by 8 (aligned to double word boundary)
// In Lab 2, you can ignore both the stackSize and priority fields
// In Lab 3, you can ignore the stackSize fields
int OS_AddThread(void (*task)(void),
                 uint32_t stackSize, uint32_t priority)
{
    int32_t sr;
    OSCRITICAL_ENTER();
    uint32_t tid;

    // Search for an available TCB
    for (tid = 0; tid < MAX_THREADS; tid++)
    {
        if (tcb_pool[tid].status == DEAD)
        {
            break;
        }
    }

    if (tid == MAX_THREADS)
    {
        goto OS_AddThread_exit;
    }

    // Clamp priority to maximum value
    priority = (priority < PRIORITY_LEVELS) ? priority : PRIORITY_LEVELS - 1;

    // Initialize TCB and stack for new thread
    tcb_pool[tid].id = tid;
    tcb_pool[tid].priority = priority;
    tcb_pool[tid].sleepCount = 0;
    tcb_pool[tid].status = ACTIVE;
    tcb_pool[tid].sp = &stack_pool[tid][STACK_SIZE - 16];
    if (RunPt != NULL)
    {
        tcb_pool[tid].process = RunPt->process;
        tcb_pool[tid].process->num_threads++;
    }
#if (DEADLOCK_DETECTION)
    tcb_pool[tid].lockStart = 0;
    tcb_pool[tid].LockPt = NULL;
    tcb_pool[tid].acquired = NULL;
    tcb_pool[tid].visited = 0;
#endif
    stack_pool[tid][STACK_SIZE - 1] = 0x01000000;                                                                              // PSR (thumb bit = 1)
    stack_pool[tid][STACK_SIZE - 2] = (uint32_t)task;                                                                          // PC
    stack_pool[tid][STACK_SIZE - 3] = (uint32_t)&OS_Kill;                                                                      // R14
    stack_pool[tid][STACK_SIZE - 4] = 0x12121212;                                                                              // R12
    stack_pool[tid][STACK_SIZE - 5] = 0x03030303;                                                                              // R3
    stack_pool[tid][STACK_SIZE - 6] = 0x02020202;                                                                              // R2
    stack_pool[tid][STACK_SIZE - 7] = 0x01010101;                                                                              // R1
    stack_pool[tid][STACK_SIZE - 8] = 0x00000000;                                                                              // R0
    stack_pool[tid][STACK_SIZE - 9] = 0x11111111;                                                                              // R11
    stack_pool[tid][STACK_SIZE - 10] = 0x10101010;                                                                             // R10
    stack_pool[tid][STACK_SIZE - 11] = (tcb_pool[tid].process != NULL) ? (uint32_t)(tcb_pool[tid].process->data) : 0x09090909; // R9
    stack_pool[tid][STACK_SIZE - 12] = 0x08080808;                                                                             // R8
    stack_pool[tid][STACK_SIZE - 13] = 0x07070707;                                                                             // R7
    stack_pool[tid][STACK_SIZE - 14] = 0x06060606;                                                                             // R6
    stack_pool[tid][STACK_SIZE - 15] = 0x05050505;                                                                             // R5
    stack_pool[tid][STACK_SIZE - 16] = 0x04040404;                                                                             // R4

    // Add new thread to end of priority linked list
    PriorityPts[priority] = tcb_list_add(PriorityPts[priority], &tcb_pool[tid]);
    if (RunPt == NULL)
    {
        RunPt = &tcb_pool[tid];
    }

    if (NextPt == NULL)
    {
        NextPt = RunPt;
    }
    num_created++;

OS_AddThread_exit:
    OSCRITICAL_EXIT();
    return (tid == MAX_THREADS) ? 0 : 1;
};

int OS_ProcessAddInitialThread(void (*task)(void),
                               uint32_t stackSize, uint32_t priority, PCB *process)
{
    int32_t sr;
    OSCRITICAL_ENTER();
    uint32_t tid;

    // Search for an available TCB
    for (tid = 0; tid < MAX_THREADS; tid++)
    {
        if (tcb_pool[tid].status == DEAD)
        {
            break;
        }
    }

    if (tid == MAX_THREADS)
    {
        goto OS_AddThread_exit;
    }

    // Clamp priority to maximum value
    priority = (priority < PRIORITY_LEVELS) ? priority : PRIORITY_LEVELS - 1;

    // Initialize TCB and stack for new thread
    tcb_pool[tid].id = tid;
    tcb_pool[tid].priority = priority;
    tcb_pool[tid].sleepCount = 0;
    tcb_pool[tid].status = ACTIVE;
    tcb_pool[tid].sp = &stack_pool[tid][STACK_SIZE - 16];
    tcb_pool[tid].process = process;
#if (DEADLOCK_DETECTION)
    tcb_pool[tid].lockStart = 0;
    tcb_pool[tid].LockPt = NULL;
    tcb_pool[tid].acquired = NULL;
    tcb_pool[tid].visited = 0;
#endif
    stack_pool[tid][STACK_SIZE - 1] = 0x01000000;                                                  // PSR (thumb bit = 1)
    stack_pool[tid][STACK_SIZE - 2] = (uint32_t)task;                                              // PC
    stack_pool[tid][STACK_SIZE - 3] = (uint32_t)&OS_Kill;                                          // R14
    stack_pool[tid][STACK_SIZE - 4] = 0x12121212;                                                  // R12
    stack_pool[tid][STACK_SIZE - 5] = 0x03030303;                                                  // R3
    stack_pool[tid][STACK_SIZE - 6] = 0x02020202;                                                  // R2
    stack_pool[tid][STACK_SIZE - 7] = 0x01010101;                                                  // R1
    stack_pool[tid][STACK_SIZE - 8] = 0x00000000;                                                  // R0
    stack_pool[tid][STACK_SIZE - 9] = 0x11111111;                                                  // R11
    stack_pool[tid][STACK_SIZE - 10] = 0x10101010;                                                 // R10
    stack_pool[tid][STACK_SIZE - 11] = (process != NULL) ? (uint32_t)(process->data) : 0x09090909; // R9
    stack_pool[tid][STACK_SIZE - 12] = 0x08080808;                                                 // R8
    stack_pool[tid][STACK_SIZE - 13] = 0x07070707;                                                 // R7
    stack_pool[tid][STACK_SIZE - 14] = 0x06060606;                                                 // R6
    stack_pool[tid][STACK_SIZE - 15] = 0x05050505;                                                 // R5
    stack_pool[tid][STACK_SIZE - 16] = 0x04040404;                                                 // R4

    // Add new thread to end of priority linked list
    PriorityPts[priority] = tcb_list_add(PriorityPts[priority], &tcb_pool[tid]);
    if (RunPt == NULL)
    {
        RunPt = &tcb_pool[tid];
    }

    if (NextPt == NULL)
    {
        NextPt = RunPt;
    }
    num_created++;

OS_AddThread_exit:
    OSCRITICAL_EXIT();
    return (tid == MAX_THREADS) ? 0 : 1;
};

//******** OS_AddProcess ***************
// add a process with foregound thread to the scheduler
// Inputs: pointer to a void/void entry point
//         pointer to process text (code) segment
//         pointer to process data segment
//         number of bytes allocated for its stack
//         priority (0 is highest)
// Outputs: 1 if successful, 0 if this process can not be added
// This function will be needed for Lab 5
// In Labs 2-4, this function can be ignored
int OS_AddProcess(void (*entry)(void), void *text, void *data,
                  unsigned long stackSize, unsigned long priority)
{
    // put Lab 5 solution here
    // Need to add PCB structure that will track PID (will also need to be
    // added to TCB), pointers to data and code segments, and number of threads
    // spawned (so that we know when to free memory / reclaim resources.
    // Note: based on hint in lab doc it seems that the pointers to data and code segments
    // may just be dummy segments on the heap -- main thing is the start location
    int32_t sr;
    OSCRITICAL_ENTER();
    uint32_t pid;

    // Search for an available PCB
    for (pid = 0; pid < MAX_PROCESSES; pid++)
    {
        if (pcb_pool[pid].status == DEAD)
        {
            break;
        }
    }

    if (pid == MAX_PROCESSES)
    {
        goto OS_AddProcess_Exit;
    }

    pcb_pool[pid].id = pid;
    pcb_pool[pid].num_threads = 1;
    pcb_pool[pid].text = text;
    pcb_pool[pid].data = data;
    pcb_pool[pid].status = ACTIVE;

    OS_ProcessAddInitialThread(entry, stackSize, priority, &pcb_pool[pid]);

OS_AddProcess_Exit:
    OSCRITICAL_EXIT();
    return (pid == MAX_PROCESSES) ? 0 : 1;
}

//******** OS_Id ***************
// returns the thread ID for the currently running thread
// Inputs: none
// Outputs: Thread ID, number greater than zero
uint32_t OS_Id(void)
{
    return RunPt->id;
};

#if (MEASURE_PERIODIC_JITTER)
void (*periodicTask1)(void);
void (*periodicTask2)(void);
uint32_t period1, period2;
uint32_t periodic_task1_calls = 0;
uint32_t periodic_task2_calls = 0;
uint32_t thisTime1, lastTime1;
uint32_t thisTime2, lastTime2;
void PeriodicTask1Wrapper(void)
{
    uint32_t jitter;
    thisTime1 = OS_Time();
    periodic_task1_calls++;
    if (periodic_task1_calls > 1)
    {
        uint32_t diff = OS_TimeDifference(lastTime1, thisTime1);
        if (diff > period1)
        {
            jitter = (diff - period1 + 4) / 8;
        }
        else
        {
            jitter = (period1 - diff + 4) / 8;
        }
        if (jitter > PeriodicJitter1)
        {
            PeriodicJitter1 = jitter;
        }
        if (jitter >= JitterSize)
        {
            jitter = JitterSize - 1;
        }
        PeriodicJitterHist1[jitter]++;
    }
    lastTime1 = thisTime1;
    periodicTask1();
}
void PeriodicTask2Wrapper(void)
{
    uint32_t jitter;
    thisTime2 = OS_Time();
    periodic_task2_calls++;
    if (periodic_task2_calls > 1)
    {
        uint32_t diff = OS_TimeDifference(lastTime2, thisTime2);
        if (diff > period2)
        {
            jitter = (diff - period2 + 4) / 8;
        }
        else
        {
            jitter = (period2 - diff + 4) / 8;
        }
        if (jitter > PeriodicJitter2)
        {
            PeriodicJitter2 = jitter;
        }
        if (jitter >= JitterSize)
        {
            jitter = JitterSize - 1;
        }
        PeriodicJitterHist2[jitter]++;
    }
    lastTime2 = thisTime2;
    periodicTask2();
}
#endif

uint32_t numPeriodicThreads = 0;
//******** OS_AddPeriodicThread ***************
// add a background periodic task
// typically this function receives the highest priority
// Inputs: pointer to a void/void background function
//         period given in system time units (12.5ns)
//         priority 0 is the highest, 5 is the lowest
// Outputs: 1 if successful, 0 if this thread can not be added
// You are free to select the time resolution for this function
// It is assumed that the user task will run to completion and return
// This task can not spin, block, loop, sleep, or kill
// This task can call OS_Signal  OS_bSignal   OS_AddThread
// This task does not have a Thread ID
// In lab 1, this command will be called 1 time
// In lab 2, this command will be called 0 or 1 times
// In lab 2, the priority field can be ignored
// In lab 3, this command will be called 0 1 or 2 times
// In lab 3, there will be up to four background threads, and this priority field
//           determines the relative priority of these four threads
int OS_AddPeriodicThread(void (*task)(void),
                         uint32_t period, uint32_t priority)
{
    // Simple implementation for now, will need to generalize implementation later.
    switch (numPeriodicThreads)
    {
    case 0:
#if (MEASURE_PERIODIC_JITTER)
        periodicTask1 = task;
        period1 = period;
        Timer3A_Init(PeriodicTask1Wrapper, period, priority);
#else
        Timer3A_Init(task, period, priority);
#endif
        numPeriodicThreads++;
        break;
    case 1:
#if (MEASURE_PERIODIC_JITTER)
        periodicTask2 = task;
        period2 = period;
        Timer4A_Init(PeriodicTask2Wrapper, period, priority);
#else
        Timer4A_Init(task, period, priority);
#endif
        numPeriodicThreads++;
        break;
    default:
        // Unsuccessful because numPeriodicThreads > 2
        return 0;
    }
    return 1;
};

/*----------------------------------------------------------------------------
  PF1 Interrupt Handler
 *----------------------------------------------------------------------------*/
void (*SW1Task)(void);
void (*SW2Task)(void);

void GPIOPortF_Handler(void)
{
    if (GPIO_PORTF_RIS_R & 0x10)
    { // if trigger flag set SW1/PF4
        SW1Task();
        GPIO_PORTF_ICR_R = 0x10; // clear trigger flag
        GPIO_PORTF_IM_R |= 0x10; // rearm interrupt
    }
    if (GPIO_PORTF_RIS_R & 0x01)
    { // if trigger flag set SW2/PF0
        SW2Task();
        GPIO_PORTF_ICR_R = 0x01; // clear trigger flag
        GPIO_PORTF_IM_R |= 0x01; // rearm interrupt
    }
}

void SW1_Init(uint32_t priority)
{
    if (priority > 7)
        priority = 7;
    SYSCTL_RCGCGPIO_R |= 0x00000020; // activate clock for Port F
    while ((SYSCTL_PRGPIO_R & 0x20) == 0)
    {
    };                                                           // allow time for clock to stabilize
    GPIO_PORTF_LOCK_R = 0x4C4F434B;                              // unlock GPIO Port F
    GPIO_PORTF_CR_R = 0x10;                                      // allow changes to PF4
    GPIO_PORTF_DIR_R &= ~0x10;                                   // PF4 in
    GPIO_PORTF_PUR_R |= 0x10;                                    // enable pull-up on  PF4
    GPIO_PORTF_DEN_R |= 0x10;                                    // enable digital I/O on PF4
    GPIO_PORTF_IS_R &= ~0x10;                                    // PF4 is edge-sensitive
    GPIO_PORTF_IEV_R &= ~0x10;                                   // PF4 falling edge event
    GPIO_PORTF_ICR_R = 0x10;                                     // clear trigger
    GPIO_PORTF_IM_R |= 0x10;                                     // arm interrupt on PF4
    NVIC_PRI7_R = (NVIC_PRI7_R & 0xFF00FFFF) | (priority << 21); // set priority
    NVIC_EN0_R = 0x40000000;                                     // enable interrupt 30 in NVIC
}

void SW2_Init(uint32_t priority)
{
    if (priority > 7)
        priority = 7;
    SYSCTL_RCGCGPIO_R |= 0x00000020; // activate clock for Port F
    while ((SYSCTL_PRGPIO_R & 0x20) == 0)
    {
    };                                                           // allow time for clock to stabilize
    GPIO_PORTF_LOCK_R = 0x4C4F434B;                              // unlock GPIO Port F
    GPIO_PORTF_CR_R = 0x01;                                      // allow changes to PF0
    GPIO_PORTF_DIR_R &= ~0x01;                                   // PF0 in
    GPIO_PORTF_PUR_R |= 0x01;                                    // enable pull-up on  PF0
    GPIO_PORTF_DEN_R |= 0x01;                                    // enable digital I/O on PF0
    GPIO_PORTF_IS_R &= ~0x01;                                    // PF0 is edge-sensitive
    GPIO_PORTF_IEV_R &= ~0x01;                                   // PF0 falling edge event
    GPIO_PORTF_ICR_R = 0x01;                                     // clear trigger
    GPIO_PORTF_IM_R |= 0x01;                                     // arm interrupt on PF0
    NVIC_PRI7_R = (NVIC_PRI7_R & 0xFF00FFFF) | (priority << 21); // set priority
    NVIC_EN0_R = 0x40000000;                                     // enable interrupt 30 in NVIC
}

//******** OS_AddSW1Task ***************
// add a background task to run whenever the SW1 (PF4) button is pushed
// Inputs: pointer to a void/void background function
//         priority 0 is the highest, 5 is the lowest
// Outputs: 1 if successful, 0 if this thread can not be added
// It is assumed that the user task will run to completion and return
// This task can not spin, block, loop, sleep, or kill
// This task can call OS_Signal  OS_bSignal   OS_AddThread
// This task does not have a Thread ID
// In labs 2 and 3, this command will be called 0 or 1 times
// In lab 2, the priority field can be ignored
// In lab 3, there will be up to four background threads, and this priority field
//           determines the relative priority of these four threads
int OS_AddSW1Task(void (*task)(void), uint32_t priority)
{
    SW1Task = task;
    SW1_Init(priority);
    return 1; // replace this line with solution
};

//******** OS_AddSW2Task ***************
// add a background task to run whenever the SW2 (PF0) button is pushed
// Inputs: pointer to a void/void background function
//         priority 0 is highest, 5 is lowest
// Outputs: 1 if successful, 0 if this thread can not be added
// It is assumed user task will run to completion and return
// This task can not spin block loop sleep or kill
// This task can call issue OS_Signal, it can call OS_AddThread
// This task does not have a Thread ID
// In lab 2, this function can be ignored
// In lab 3, this command will be called will be called 0 or 1 times
// In lab 3, there will be up to four background threads, and this priority field
//           determines the relative priority of these four threads
int OS_AddSW2Task(void (*task)(void), uint32_t priority)
{
    SW2Task = task;
    SW2_Init(priority);
    return 1; // replace this line with solution
};

// ******** OS_Sleep ************
// place this thread into a dormant state
// input:  number of msec to sleep
// output: none
// You are free to select the time resolution for this function
// OS_Sleep(0) implements cooperative multitasking
void OS_Sleep(uint32_t sleepTime)
{
    if (sleepTime > 0)
    {
        RunPt->status = SLEEPING; // change status to sleep
        RunPt->sleepCount = sleepTime;
    }
    OS_Suspend();
};

// ******** OS_Kill ************
// kill the currently running thread, release its TCB and stack
// input:  none
// output: none
void OS_Kill(void)
{
    int32_t sr;
    OSCRITICAL_ENTER();
#if (DEADLOCK_DETECTION)
    // Release all held locks
    Lock *curr = RunPt->acquired;
    while (curr != NULL)
    {
        OS_LockRelease(curr);
        curr = curr->next;
    }
#endif
    RunPt->status = DEAD;
    if (RunPt->process != NULL)
    {
        RunPt->process->num_threads--;
        if (RunPt->process->num_threads == 0)
        {
            Heap_Free(RunPt->process->text);
            Heap_Free(RunPt->process->data);
            RunPt->process->status = DEAD;
        }
    }
    PriorityPts[RunPt->priority] = tcb_list_remove(RunPt);
    if (PriorityPts[RunPt->priority] != NULL)
    {
        PriorityPts[RunPt->priority] = PriorityPts[RunPt->priority]->prev;
    }
    num_killed++;
    OSCRITICAL_EXIT();
    OS_Suspend();
};

// ******** OS_Kill_Thread ************
// kill a thread, release its TCB and stack
// input:  thread id to kill
// output: none
void OS_Kill_Thread(uint32_t tid)
{
    int32_t sr;
    OSCRITICAL_ENTER();
    TCB *thread = &tcb_pool[tid];
#if (DEADLOCK_DETECTION)
    // Release all held locks
    Lock *curr = thread->acquired;
    while (curr != NULL)
    {
        OS_LockRelease(curr);
        curr = curr->next;
    }
#endif
    thread->status = DEAD;
    if (thread->process != NULL)
    {
        thread->process->num_threads--;
        if (thread->process->num_threads == 0)
        {
            Heap_Free(thread->process->text);
            Heap_Free(thread->process->data);
            thread->process->status = DEAD;
        }
    }
    if (thread == PriorityPts[thread->priority])
    {
        PriorityPts[thread->priority] = tcb_list_remove(thread);
        if (PriorityPts[thread->priority] != NULL)
        {
            PriorityPts[thread->priority] = PriorityPts[thread->priority]->prev;
        }
    }
    else if (thread->SemaPt != NULL && thread == thread->SemaPt->BlockedPts[thread->priority])
    {
        thread->SemaPt->BlockedPts[thread->priority] = tcb_list_remove(thread);
    }
    num_killed++;
    OSCRITICAL_EXIT();
    OS_Suspend();
};

// ******** OS_Suspend ************
// suspend execution of currently running thread
// scheduler will choose another thread to execute
// Can be used to implement cooperative multitasking
// Same function as OS_Sleep(0)
// input:  none
// output: none
void OS_Suspend(void)
{
    // Rotate current priority list
    if (PriorityPts[RunPt->priority] != NULL)
    {
        PriorityPts[RunPt->priority] = PriorityPts[RunPt->priority]->next;
    }

    // Find next lowest priority
    uint32_t priority;
    for (priority = 0; priority < PRIORITY_LEVELS; priority++)
    {
        if (PriorityPts[priority] != NULL)
        {
            if (PriorityPts[priority]->status != ACTIVE)
            {
                // Priority list head is blocked/sleeping, so look for next thread
                TCB *curr = PriorityPts[priority]->next;
                while (curr != PriorityPts[priority] && curr->status != ACTIVE)
                {
                    curr = curr->next;
                }
                if (curr == PriorityPts[priority])
                {
                    // All threads are inactive
                    continue;
                }

                // Move new thread the front of priority list
                PriorityPts[priority] = curr;
            }
            break;
        }
    }
    // How to handle if all threads are inactive? Not sure if we need to consider this
    NextPt = PriorityPts[priority];
    ContextSwitch();
};

// ******** OS_Fifo_Init ************
// Initialize the Fifo to be empty
// Inputs: size
// Outputs: none
// In Lab 2, you can ignore the size field
// In Lab 3, you should implement the user-defined fifo size
// In Lab 3, you can put whatever restrictions you want on size
//    e.g., 4 to 64 elements
//    e.g., must be a power of 2,4,8,16,32,64,128
void OS_Fifo_Init(uint32_t size)
{
    PutPt = GetPt = &Fifo[0];          // set pointers to bottom
    OS_InitSemaphore(&CurrentSize, 0); // curr size can be encapsulated in semaphore value
    OS_InitSemaphore(&FIFOmutex, 1);
};

// ******** OS_Fifo_Put ************
// Enter one data sample into the Fifo
// Called from the background, so no waiting
// Inputs:  data
// Outputs: true if data is properly saved,
//          false if data not saved, because it was full
// Since this is called by interrupt handlers
//  this function can not disable or enable interrupts
int OS_Fifo_Put(uint32_t data)
{
    if (CurrentSize.Value == FIFOSIZE)
    {
        return 0;
    }
    *(PutPt) = data; // Put
    PutPt++;         // place to put next
    if (PutPt == &Fifo[FIFOSIZE])
    {
        PutPt = &Fifo[0]; // wrap
    }
    OS_Signal(&CurrentSize); // Increments size and alerts readers
    return 1;
};

// ******** OS_Fifo_Get ************
// Remove one data sample from the Fifo
// Called in foreground, will spin/block if empty
// Inputs:  none
// Outputs: data
uint32_t OS_Fifo_Get(void)
{
    OS_Wait(&CurrentSize);    // Waits if empty, decrements size once data available
    OS_bWait(&FIFOmutex);     // Exclusive read access
    uint32_t data = *(GetPt); // get data
    GetPt++;                  // points to next data to get
    if (GetPt == &Fifo[FIFOSIZE])
    {
        GetPt = &Fifo[0]; // wrap
    }
    OS_bSignal(&FIFOmutex);
    return data;
};

// ******** OS_Fifo_Size ************
// Check the status of the Fifo
// Inputs: none
// Outputs: returns the number of elements in the Fifo
//          greater than zero if a call to OS_Fifo_Get will return right away
//          zero or less than zero if the Fifo is empty
//          zero or less than zero if a call to OS_Fifo_Get will spin or block
int32_t OS_Fifo_Size(void)
{
    return CurrentSize.Value;
};

// Assume Mailbox used by Foreground threads only

// ******** OS_MailBox_Init ************
// Initialize communication channel
// Inputs:  none
// Outputs: none
void OS_MailBox_Init(void)
{
    OS_InitSemaphore(&BoxFree, 1);   // initialize to free
    OS_InitSemaphore(&MailValid, 0); // init to not valid
};

// ******** OS_MailBox_Send ************
// enter mail into the MailBox
// Inputs:  data to be sent
// Outputs: none
// This function will be called from a foreground thread
// It will spin/block if the MailBox contains data not yet received
void OS_MailBox_Send(uint32_t data)
{
    OS_bWait(&BoxFree);
    Mail = data;
    OS_bSignal(&MailValid);
};

// ******** OS_MailBox_Recv ************
// remove mail from the MailBox
// Inputs:  none
// Outputs: data received
// This function will be called from a foreground thread
// It will spin/block if the MailBox is empty
uint32_t OS_MailBox_Recv(void)
{
    OS_bWait(&MailValid);
    uint32_t data = Mail;
    OS_bSignal(&BoxFree);
    return data;
};

// ******** OS_Time ************
// return the system time
// Inputs:  none
// Outputs: time in 12.5ns units, 0 to 4294967295
// The time resolution should be less than or equal to 1us, and the precision 32 bits
// It is ok to change the resolution and precision of this function as long as
//   this function and OS_TimeDifference have the same resolution and precision
uint32_t OS_Time(void)
{
    // Reload value - Current value = Elapsed time since last reload
    return TIMER2_TAILR_R - TIMER2_TAV_R;
};

// ******** OS_TimeDifference ************
// Calculates difference between two times
// Inputs:  two times measured with OS_Time
// Outputs: time difference in 12.5ns units
// The time resolution should be less than or equal to 1us, and the precision at least 12 bits
// It is ok to change the resolution and precision of this function as long as
//   this function and OS_Time have the same resolution and precision
uint32_t OS_TimeDifference(uint32_t start, uint32_t stop)
{
    if (start <= stop)
    {
        return stop - start;
    }
    else
    {
        // Wrap around computation
        return (TIMER2_TAILR_R - start) + stop;
    }
};

//***OSMs Task***
uint32_t time;
void OS_MsTask(void)
{
    time++;

    for (uint32_t priority = 0; priority < PRIORITY_LEVELS; priority++)
    {
        TCB *curr = PriorityPts[priority];
        if (curr != NULL)
        {
            do
            {
                if (curr->status == SLEEPING)
                {
                    curr->sleepCount--;
                    if (curr->sleepCount == 0)
                    {
                        curr->status = ACTIVE;
                    }
                }
                curr = curr->next;
            } while (curr != PriorityPts[priority]);
        }
    }
}
// ******** OS_ClearMsTime ************
// sets the system time to zero (solve for Lab 1), and start a periodic interrupt
// Inputs:  none
// Outputs: none
// You are free to change how this works
void OS_ClearMsTime(void)
{
    time = 0;
    Timer1A_Init(OS_MsTask, 80000000 / 1000, 7); // 1000 Hz bec 1ms period
    // put Lab 1 solution here
};

// ******** OS_MsTime ************
// reads the current time in msec (solve for Lab 1)
// Inputs:  none
// Outputs: time in ms units
// You are free to select the time resolution for this function
// For Labs 2 and beyond, it is ok to make the resolution to match the first call to OS_AddPeriodicThread
uint32_t OS_MsTime(void)
{
    // put Lab 1 solution here
    // read TAR register instead?
    return time; // replace this line with solution
};

//******** OS_Launch ***************
// start the scheduler, enable interrupts
// Inputs: number of 12.5ns clock cycles for each time slice
//         you may select the units of this parameter
// Outputs: none (does not return)
// In Lab 2, you can ignore the theTimeSlice field
// In Lab 3, you should implement the user-defined TimeSlice field
// It is ok to limit the range of theTimeSlice to match the 24-bit SysTick
void OS_Launch(uint32_t theTimeSlice)
{
    // put Lab 2 (and beyond) solution here
    STCTRL = 0;                                    // disable SysTick during setup
    STCURRENT = 0;                                 // any write to current clears it
    SYSPRI3 = (SYSPRI3 & 0x00FFFFFF) | 0xE0000000; // systick priority 7
    SYSPRI3 = (SYSPRI3 & 0xFF00FFFF) | 0x00E00000; // pendsv priority 7
    STRELOAD = theTimeSlice - 1;                   // reload value
    STCTRL = 0x00000007;                           // enable, core clock and interrupt arm
    OS_ClearMsTime();
    StartOS(); // start on the first task
};

//************** I/O Redirection ***************
// redirect terminal I/O to UART or file (Lab 4)

int StreamToDevice = 0; // 0=UART, 1=stream to file (Lab 4)

int fputc(int ch, FILE *f)
{
    if (StreamToDevice == 1)
    { // Lab 4
        if (eFile_Write(ch))
        {                           // close file on error
            OS_EndRedirectToFile(); // cannot write to file
            return 1;               // failure
        }
        return 0; // success writing
    }

    // default UART output
    UART_OutChar(ch);
    return ch;
}

int fgetc(FILE *f)
{
    char ch = UART_InChar(); // receive from keyboard
    UART_OutChar(ch);        // echo
    return ch;
}

int OS_RedirectToFile(const char *name)
{                       // Lab 4
    eFile_Create(name); // ignore error if file already exists
    if (eFile_WOpen(name))
        return 1; // cannot open file
    StreamToDevice = 1;
    return 0;
}

int OS_EndRedirectToFile(void)
{ // Lab 4
    StreamToDevice = 0;
    if (eFile_WClose())
        return 1; // cannot close file
    return 0;
}

int OS_RedirectToUART(void)
{
    StreamToDevice = 0;
    return 0;
}

int OS_RedirectToST7735(void)
{

    return 1;
}
