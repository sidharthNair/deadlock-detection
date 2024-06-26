// deadlock.c
// Runs on LM4F120/TM4C123

#include <stdint.h>
#include <stdio.h>

#include "../common/ADC.h"
#include "../common/Interpreter.h"
#include "../common/OS.h"
#include "../common/ST7735.h"
#include "../common/UART0int.h"
#include "../common/eDisk.h"
#include "../common/eFile.h"
#include "../common/heap.h"
#include "../deadlock/bankers.h"
#include "../inc/CortexM.h"
#include "../inc/LPF.h"
#include "../inc/LaunchPad.h"
#include "../inc/PLL.h"
#include "../inc/tm4c123gh6pm.h"

uint32_t NumCreated;  // number of foreground threads created
uint32_t IdleCount;   // CPU idle counter

//---------------------User debugging-----------------------
#define PD0 (*((volatile uint32_t *)0x40007004))
#define PD1 (*((volatile uint32_t *)0x40007008))
#define PD2 (*((volatile uint32_t *)0x40007010))
#define PD3 (*((volatile uint32_t *)0x40007020))

void PortD_Init(void) {
    SYSCTL_RCGCGPIO_R |= 0x08;  // activate port D
    while ((SYSCTL_RCGCGPIO_R & 0x08) == 0) {
    };
    GPIO_PORTD_DIR_R |= 0x0F;     // make PD3-0 output heartbeats
    GPIO_PORTD_AFSEL_R &= ~0x0F;  // disable alt funct on PD3-0
    GPIO_PORTD_DEN_R |= 0x0F;     // enable digital I/O on PD3-0
    GPIO_PORTD_PCTL_R = ~0x0000FFFF;
    GPIO_PORTD_AMSEL_R &= ~0x0F;
    ;  // disable analog functionality on PD
}

extern uint32_t num_killed;

//------------------Idle Task--------------------------------
// foreground thread, runs when nothing else does
// never blocks, never sleeps, never dies
// inputs:  none
// outputs: none
void Idle(void) {
    IdleCount = 0;
    while (1) {
        if (IdleCount % 1024 == 0) {
            ST7735_Message(1, 7, "num_killed: ", num_killed);
        }
        IdleCount++;
        PD0 ^= 0x01;
        WaitForInterrupt();
    }
}
//--------------end of Idle Task-----------------------------

int realmain(void) {
    OS_Init();
    PortD_Init();

    NumCreated = 0;
    NumCreated += OS_AddThread(&Interpreter, 128, 2);
    NumCreated += OS_AddThread(&Idle, 128, 5);

    OS_Launch(TIME_2MS);
    return 0;
}

Lock first;
Lock second;

void BasicThread1(void) {
    PD1 ^= 0x02;
    OS_LockAcquire(&first);
    OS_Sleep(1000);
    OS_LockAcquire(&second);
    OS_LockRelease(&first);
    OS_LockRelease(&second);
    PD1 ^= 0x02;
}

void BasicThread2(void) {
    PD2 ^= 0x04;
    OS_LockAcquire(&second);
    OS_Sleep(1000);
    OS_LockAcquire(&first);
    OS_LockRelease(&first);
    OS_LockRelease(&second);
    PD2 ^= 0x04;
}

int TestmainBasic(void) {
    OS_Init();
    PortD_Init();

    OS_InitLock(&first);
    OS_InitLock(&second);

    NumCreated = 0;
    NumCreated += OS_AddThread(&BasicThread1, 128, 3);
    NumCreated += OS_AddThread(&BasicThread2, 128, 3);
    NumCreated += OS_AddThread(&Idle, 128, 5);

    OS_Launch(TIME_2MS);
    return 0;
}

#define NUM_PHILOSOPHERS 8

Lock forks[NUM_PHILOSOPHERS];

void DiningPhilosopher() {
    int philosopher_id = OS_Id();
    int left_fork = philosopher_id;
    int right_fork = (philosopher_id + 1) % NUM_PHILOSOPHERS;

    while (1) {
        // ST7735_Message(0, philosopher_id, "acquiring left", philosopher_id);
        OS_LockAcquire(&forks[left_fork]);
        OS_Sleep(100);
        ST7735_Message(0, philosopher_id, "acquiring right", philosopher_id);
        OS_LockAcquire(&forks[right_fork]);

        // Philosopher eats...
        OS_Sleep(100);

        // Philosopher releases both forks
        ST7735_Message(0, philosopher_id, "releasing left", philosopher_id);
        OS_LockRelease(&forks[left_fork]);
        OS_Sleep(100);
        ST7735_Message(0, philosopher_id, "releasing right", philosopher_id);
        OS_LockRelease(&forks[right_fork]);

        // Philosopher thinks...
        OS_Sleep(100);
    }
}

#define MIN(a, b) (a < b ? a : b)
#define MAX(a, b) (a > b ? a : b)
void DiningPhilosopherFixed() {
    int philosopher_id = OS_Id();
    int left_fork = MIN(philosopher_id, (philosopher_id + 1) % NUM_PHILOSOPHERS);
    int right_fork = MAX(philosopher_id, (philosopher_id + 1) % NUM_PHILOSOPHERS);

    while (1) {
        // ST7735_Message(0, philosopher_id, "acquiring left", philosopher_id);
        OS_LockAcquire(&forks[left_fork]);
        OS_Sleep(100);
        ST7735_Message(0, philosopher_id, "acquiring right", philosopher_id);
        OS_LockAcquire(&forks[right_fork]);

        // Philosopher eats...
        OS_Sleep(100);

        // Philosopher releases both forks
        ST7735_Message(0, philosopher_id, "releasing left", philosopher_id);
        OS_LockRelease(&forks[left_fork]);
        OS_Sleep(100);
        ST7735_Message(0, philosopher_id, "releasing right", philosopher_id);
        OS_LockRelease(&forks[right_fork]);

        // Philosopher thinks...
        OS_Sleep(100);
    }
}

int TestmainDining() {
    OS_Init();
    PortD_Init();

    NumCreated = 0;
    for (int i = 0; i < NUM_PHILOSOPHERS; i++) {
        OS_InitLock(&forks[i]);
        NumCreated += OS_AddThread(&DiningPhilosopherFixed, 128, 3);
    }

    NumCreated += OS_AddThread(&Idle, 128, 5);

    OS_Launch(TIME_2MS);
    return 0;
}

void Requestor0(void) {
    int max_demand[] = {7, 5, 3};
    Bankers_SetMaxDemand(-1, max_demand);
    OS_Sleep(1000);
    int request[] = {0, 1, 0};
    Bankers_RequestResourcesBlocking(-1, request);
    OS_Sleep(1000);
    Bankers_ReleaseResources(-1, request);
    ST7735_Message(0, 0, "Requestor done: ", 0);
    OS_Kill();
}

void Requestor1(void) {
    int max_demand[] = {3, 2, 2};
    Bankers_SetMaxDemand(-1, max_demand);
    OS_Sleep(1000);
    int request[] = {2, 0, 0};
    Bankers_RequestResourcesBlocking(-1, request);
    OS_Sleep(1000);
    Bankers_ReleaseResources(-1, request);
    ST7735_Message(0, 1, "Requestor done: ", 1);
    OS_Kill();
}

void Requestor2(void) {
    int max_demand[] = {9, 0, 2};
    Bankers_SetMaxDemand(-1, max_demand);
    OS_Sleep(1000);
    int request[] = {3, 0, 2};
    Bankers_RequestResourcesBlocking(-1, request);
    OS_Sleep(1000);
    Bankers_ReleaseResources(-1, request);
    ST7735_Message(0, 2, "Requestor done: ", 2);
    OS_Kill();
}

void Requestor3(void) {
    int max_demand[] = {2, 2, 2};
    Bankers_SetMaxDemand(-1, max_demand);
    OS_Sleep(1000);
    int request[] = {2, 1, 1};
    Bankers_RequestResourcesBlocking(-1, request);
    OS_Sleep(1000);
    Bankers_ReleaseResources(-1, request);
    ST7735_Message(0, 3, "Requestor done: ", 3);
    OS_Kill();
}

void Requestor4(void) {
    int max_demand[] = {4, 3, 3};
    Bankers_SetMaxDemand(-1, max_demand);
    OS_Sleep(1000);
    int request[] = {0, 0, 2};
    Bankers_RequestResourcesBlocking(-1, request);
    OS_Sleep(1000);
    Bankers_ReleaseResources(-1, request);
    ST7735_Message(0, 4, "Requestor done: ", 4);
    OS_Kill();
}

// all requests succeed
int TestmainBankers0(void) {
    OS_Init();
    PortD_Init();

    printf("\r\n==== TestMain Bankers0 ====\r\n");

    int resources[] = {10, 5, 7};
    int status = Bankers_Init(3, -1, resources);
    if (status) {
        printf("Error with Bankers_Init: %d\r\n", status);
    }

    NumCreated = 0;
    NumCreated += OS_AddThread(&Requestor0, 128, 3);
    NumCreated += OS_AddThread(&Requestor1, 128, 3);
    NumCreated += OS_AddThread(&Requestor2, 128, 3);
    NumCreated += OS_AddThread(&Requestor3, 128, 3);
    NumCreated += OS_AddThread(&Requestor4, 128, 3);
    NumCreated += OS_AddThread(&Idle, 128, 5);

    OS_Launch(TIME_2MS);
    return 0;
}

// some requests will be rejected (they will retry until they succeed) due to potential deadlock
int TestmainBankers1(void) {
    OS_Init();
    PortD_Init();

    printf("\r\n==== TestMain Bankers1 ====\r\n");

    int resources[] = {9, 5, 3};
    int status = Bankers_Init(3, -1, resources);
    if (status) {
        printf("Error with Bankers_Init: %d\r\n", status);
    }

    NumCreated = 0;
    NumCreated += OS_AddThread(&Requestor0, 128, 3);
    NumCreated += OS_AddThread(&Requestor1, 128, 3);
    NumCreated += OS_AddThread(&Requestor2, 128, 3);
    NumCreated += OS_AddThread(&Requestor3, 128, 3);
    NumCreated += OS_AddThread(&Requestor4, 128, 3);
    NumCreated += OS_AddThread(&Idle, 128, 5);

    OS_Launch(TIME_2MS);
    return 0;
}

void Basic0(void) {
    int max_demand[] = {1, 1};
    Bankers_SetMaxDemand(-1, max_demand);
    OS_Sleep(1000);
    int request1[] = {1, 0};
    int request2[] = {0, 1};
    Bankers_RequestResourcesBlocking(-1, request1);
    OS_Sleep(1000);
    Bankers_RequestResourcesBlocking(-1, request2);
    OS_Sleep(1000);
    int release[] = {1, 1};
    Bankers_ReleaseResources(-1, release);
    ST7735_Message(0, 0, "Requestor done: ", 0);
    OS_Kill();
}

void Basic1(void) {
    int max_demand[] = {1, 1};
    Bankers_SetMaxDemand(-1, max_demand);
    OS_Sleep(1000);
    int request1[] = {0, 1};
    int request2[] = {1, 0};
    Bankers_RequestResourcesBlocking(-1, request1);
    OS_Sleep(1000);
    Bankers_RequestResourcesBlocking(-1, request2);
    OS_Sleep(1000);
    int release[] = {1, 1};
    Bankers_ReleaseResources(-1, release);
    ST7735_Message(0, 1, "Requestor done: ", 1);
    OS_Kill();
}

// very basic example
int TestmainBankersSimple(void) {
    OS_Init();
    PortD_Init();

    printf("\r\n==== TestMain BankersSimple ====\r\n");

    int resources[] = {1, 1};
    int status = Bankers_Init(2, -1, resources);
    if (status) {
        printf("Error with Bankers_Init: %d\r\n", status);
    }

    NumCreated = 0;
    NumCreated += OS_AddThread(&Basic0, 128, 3);
    NumCreated += OS_AddThread(&Basic1, 128, 3);
    NumCreated += OS_AddThread(&Idle, 128, 5);

    OS_Launch(TIME_2MS);
    return 0;
}

#define NUM_RESOURCES 128
#define NUM_REQUESTORS 8
int max_demand[NUM_RESOURCES] = {[0 ... NUM_RESOURCES-1] = 1};
int request[] = {[0 ... NUM_RESOURCES-1] = 1};
int resources[] = {[0 ... NUM_RESOURCES-1] = NUM_REQUESTORS-1};

void Basic(void) {
    Bankers_SetMaxDemand(-1, max_demand);
    OS_Sleep(1000);
    Bankers_RequestResourcesBlocking(-1, request);
    OS_Sleep(1000);
    Bankers_ReleaseResources(-1, request);
    printf("Requestor done: %d\r\n", OS_Id());
    OS_Kill();
}

int TestmainBankers(void) {
    OS_Init();
    PortD_Init();

    printf("\r\n==== TestMain Bankers ====\r\n");

        int status = Bankers_Init(NUM_RESOURCES, -1, resources);
    if (status) {
        printf("Error with Bankers_Init: %d\r\n", status);
    }

    NumCreated = 0;
    for (int i = 0; i < NUM_REQUESTORS; i++) {
        NumCreated += OS_AddThread(&Basic, 128, 3);
    }
    
    NumCreated += OS_AddThread(&Idle, 128, 5);

    OS_Launch(TIME_2MS);
    return 0;
}

//*******************Trampoline for selecting main to execute**********
int main(void) {
    TestmainDining();
}
