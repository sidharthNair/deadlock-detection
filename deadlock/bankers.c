#include "../deadlock/bankers.h"
#include "../common/OS.h"
#include "../common/heap.h"
#include <stdio.h>
#include <string.h>

#define BANKERS_DEBUG 1

uint32_t size_resources = -1;
uint32_t size_customers = -1;

Sema4Type bankers_lock;
Sema4Type blocked;
int *available;

struct Customer
{
    int *maximum;
    int *allocation;
    int *need;
    int8_t initialized;
};
typedef struct Customer Customer;

Customer *customers;
int8_t *finished;

int Bankers_Init(int num_resources, int num_threads, int *available_init)
{
    if (num_resources < 0 || available_init == NULL)
    {
        return BANKERS_INVALID;
    }

    if (num_threads < 0)
    {
        num_threads = MAX_THREADS;
    }

    available = Heap_Calloc(sizeof(int) * num_resources);
    if (available == NULL)
        return BANKERS_MALLOC_ERR;

    customers = Heap_Calloc(sizeof(Customer) * num_threads);
    if (customers == NULL)
        return BANKERS_MALLOC_ERR;
    for (int i = 0; i < num_threads; i++)
    {
        customers[i].maximum = Heap_Calloc(sizeof(int) * num_resources);
        if (customers[i].maximum == NULL)
            return BANKERS_MALLOC_ERR;
        customers[i].allocation = Heap_Calloc(sizeof(int) * num_resources);
        if (customers[i].allocation == NULL)
            return BANKERS_MALLOC_ERR;
        customers[i].need = Heap_Calloc(sizeof(int) * num_resources);
        if (customers[i].need == NULL)
            return BANKERS_MALLOC_ERR;
    }
    finished = Heap_Calloc(sizeof(int8_t) * num_threads);

    if (available_init != NULL)
    {
        for (int i = 0; i < num_resources; i++)
        {
            available[i] = available_init[i];
        }
    }

    OS_InitSemaphore(&bankers_lock, 1);
    OS_InitSemaphore(&blocked, 0);
    size_resources = num_resources;
    size_customers = num_threads;

#if (BANKERS_DEBUG)
    printf("Initial state of the system:\r\n");
    printf("Available resources: ");
    for (int i = 0; i < num_resources; i++)
    {
        printf("%d ", available[i]);
    }
    printf("\r\n");
#endif

    return BANKERS_OK;
}

int Bankers_SetMaxDemand(int customer, int *max_demand)
{
    if (customer < 0)
    {
        customer = OS_Id();
    }

    if (customer >= size_customers)
    {
        return BANKERS_INVALID;
    }

    for (int i = 0; i < size_resources; i++)
    {
        if (max_demand[i] < 0)
        {
            return BANKERS_INVALID;
        }
    }

    OS_bWait(&bankers_lock);
    for (int i = 0; i < size_resources; i++)
    {
        customers[customer].maximum[i] = max_demand[i];
        customers[customer].need[i] = max_demand[i] - customers[customer].allocation[i];
    }
    customers[customer].initialized = 1;
    OS_bSignal(&bankers_lock);

    return BANKERS_OK;
}

// Helper function to check if there exists a safe exit sequence of the processes.
// bankers_lock is assumed to be held before calling this function.
int Bankers_CheckSafeSequence()
{
    int done = 0;
    int *curr_available = Heap_Malloc(sizeof(int) * size_resources);
    if (available == NULL)
        return BANKERS_MALLOC_ERR;
    memcpy(curr_available, available, sizeof(int) * size_resources);
    memset(finished, 0, sizeof(int8_t) * size_customers);

#if (BANKERS_DEBUG)
    printf("Running bankers algorithm to check for safe sequence\r\n");
#endif

    while (done < size_customers)
    {
        int found_customer = 0;
        for (int i = 0; i < size_customers; i++)
        {
            if (!finished[i])
            {
                int can_allocate = 1;
                for (int j = 0; j < size_resources; j++)
                {
                    if (customers[i].need[j] > curr_available[j])
                    {
                        can_allocate = 0;
                        break;
                    }
                }
                if (can_allocate)
                {
#if (BANKERS_DEBUG)
                    if (customers[i].initialized)
                        printf("customer %d is safe\r\n", i);
#endif
                    for (int j = 0; j < size_resources; j++)
                    {
                        curr_available[j] += customers[i].allocation[j];
                    }
                    done++;
                    found_customer = 1;
                    finished[i] = 1;
                }
            }
        }

        if (!found_customer)
            break;
    }

    Heap_Free(curr_available);
    if (done < size_customers)
    {
#if (BANKERS_DEBUG)
        printf("Safe sequence does not exist\r\n");
#endif
        return BANKERS_UNSAFE;
    }
    else
    {
#if (BANKERS_DEBUG)
        printf("Safe sequence exists\r\n");
#endif
        return BANKERS_OK;
    }
}

int Bankers_RequestResourcesNonBlocking(int customer, int *request)
{
    if (customer < 0)
    {
        customer = OS_Id();
    }

    if (request == NULL || customer >= size_customers)
    {
        return BANKERS_INVALID;
    }

    OS_bWait(&bankers_lock);

#if (BANKERS_DEBUG)
    printf("Customer %d requesting resources: ", customer);
    for (int i = 0; i < size_resources; i++)
    {
        printf("%d ", request[i]);
    }
    printf("\r\n");
#endif

    for (int i = 0; i < size_resources; i++)
    {
        if (request[i] > customers[customer].need[i] || request[i] > available[i])
        {
            OS_bSignal(&bankers_lock);
            return BANKERS_UNSAFE;
        }
    }

    for (int i = 0; i < size_resources; i++)
    {
        available[i] -= request[i];
        customers[customer].allocation[i] += request[i];
        customers[customer].need[i] -= request[i];
    }

    int status = Bankers_CheckSafeSequence();
    if (status != BANKERS_OK)
    {
        // Request was not granted, undo changes
        for (int i = 0; i < size_resources; i++)
        {
            available[i] += request[i];
            customers[customer].allocation[i] -= request[i];
            customers[customer].need[i] += request[i];
        }
    }

#if (BANKERS_DEBUG)
    printf("Current available resources: ");
    for (int i = 0; i < size_resources; i++)
    {
        printf("%d ", available[i]);
    }
    printf("\r\n");
#endif

    OS_bSignal(&bankers_lock);
    return status;
}

int Bankers_RequestResourcesBlocking(int customer, int *request)
{
    if (customer < 0)
    {
        customer = OS_Id();
    }

    if (request == NULL || customer >= size_customers)
    {
        return BANKERS_INVALID;
    }

    int status = Bankers_RequestResourcesNonBlocking(customer, request);
    while (status == BANKERS_UNSAFE)
    {
        OS_Wait(&blocked);
        status = Bankers_RequestResourcesNonBlocking(customer, request);
    }

    return status;
}

int Bankers_ReleaseResources(int customer, int *release)
{
    if (customer < 0)
    {
        customer = OS_Id();
    }

    if (release == NULL || customer >= size_customers)
    {
        return BANKERS_INVALID;
    }

    OS_bWait(&bankers_lock);

#if (BANKERS_DEBUG)
    printf("Customer %d releasing resources: ", customer);
    for (int i = 0; i < size_resources; i++)
    {
        printf("%d ", release[i]);
    }
    printf("\r\n");
#endif

    for (int i = 0; i < size_resources; i++)
    {
        if (customers[customer].allocation[i] < release[i])
        {
            OS_bSignal(&bankers_lock);
            return BANKERS_INVALID;
        }
    }

    for (int i = 0; i < size_resources; i++)
    {
        available[i] += release[i];
        customers[customer].allocation[i] -= release[i];
        customers[customer].need[i] += release[i];
    }
    OS_SignalAll(&blocked);

#if (BANKERS_DEBUG)
    printf("Current available resources: ");
    for (int i = 0; i < size_resources; i++)
    {
        printf("%d ", available[i]);
    }
    printf("\r\n");
#endif
    OS_bSignal(&bankers_lock);

    return BANKERS_OK;
}
