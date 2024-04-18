// deadlock.c
// Runs on LM4F120/TM4C123

#include <stdint.h>
#include <stdio.h>
#include "../inc/tm4c123gh6pm.h"
#include "../inc/CortexM.h"
#include "../inc/LaunchPad.h"
#include "../inc/PLL.h"
#include "../inc/LPF.h"
#include "../common/UART0int.h"
#include "../common/ADC.h"
#include "../common/OS.h"
#include "../common/heap.h"
#include "../common/Interpreter.h"
#include "../common/ST7735.h"
#include "../common/eDisk.h"
#include "../common/eFile.h"

uint32_t NumCreated; // number of foreground threads created
uint32_t IdleCount;  // CPU idle counter

//---------------------User debugging-----------------------
#define PD0 (*((volatile uint32_t *)0x40007004))
#define PD1 (*((volatile uint32_t *)0x40007008))
#define PD2 (*((volatile uint32_t *)0x40007010))
#define PD3 (*((volatile uint32_t *)0x40007020))

void PortD_Init(void)
{
    SYSCTL_RCGCGPIO_R |= 0x08; // activate port D
    while ((SYSCTL_RCGCGPIO_R & 0x08) == 0)
    {
    };
    GPIO_PORTD_DIR_R |= 0x0F;    // make PD3-0 output heartbeats
    GPIO_PORTD_AFSEL_R &= ~0x0F; // disable alt funct on PD3-0
    GPIO_PORTD_DEN_R |= 0x0F;    // enable digital I/O on PD3-0
    GPIO_PORTD_PCTL_R = ~0x0000FFFF;
    GPIO_PORTD_AMSEL_R &= ~0x0F;
    ; // disable analog functionality on PD
}

//------------------Idle Task--------------------------------
// foreground thread, runs when nothing else does
// never blocks, never sleeps, never dies
// inputs:  none
// outputs: none
void Idle(void)
{
    IdleCount = 0;
    while (1)
    {
        IdleCount++;
        PD0 ^= 0x01;
        WaitForInterrupt();
    }
}
//--------------end of Idle Task-----------------------------

int realmain(void)
{
    OS_Init();
    PortD_Init();

    NumCreated = 0;
    NumCreated += OS_AddThread(&Interpreter, 128, 2);
    NumCreated += OS_AddThread(&Idle, 128, 5);

    OS_Launch(TIME_2MS);
    return 0;
}

Sema4Type lock1;
Sema4Type lock2;

void BasicThread1(void)
{
    while (1)
    {
        OS_bWait(&lock1);
        OS_bWait(&lock2);
        PD1 ^= 0x02;
        OS_Sleep(1000);
        PD1 ^= 0x02;
        OS_bSignal(&lock2);
        OS_bSignal(&lock1);
    }
}

void BasicThread2(void)
{
    while (1)
    {
        OS_bWait(&lock1);
        OS_bWait(&lock2);
        PD2 ^= 0x04;
        OS_Sleep(1000);
        PD2 ^= 0x04;
        OS_bSignal(&lock2);
        OS_bSignal(&lock1);
    }
}

int TestmainBasic(void)
{
    OS_Init();
    PortD_Init();

    OS_InitSemaphore(&lock1, 1);
    OS_InitSemaphore(&lock2, 1);

    NumCreated = 0;
    NumCreated += OS_AddThread(&BasicThread1, 128, 3);
    NumCreated += OS_AddThread(&BasicThread2, 128, 3);
    NumCreated += OS_AddThread(&Idle, 128, 5);

    OS_Launch(TIME_2MS);
    return 0;
}

//*******************Trampoline for selecting main to execute**********
int main(void)
{
    TestmainBasic();
}
