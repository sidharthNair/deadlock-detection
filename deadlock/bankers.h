// filename: bankers.h
// Implements deadlock avoidance using Banker's algorithm

#ifndef BANKERS_H
#define BANKERS_H

// Error codes returned by Banker's algorithm functions
#define BANKERS_OK 0
#define BANKERS_ALREADY_INIT 1
#define BANKERS_MALLOC_ERR 2
#define BANKERS_INVALID 3
#define BANKERS_UNSAFE 4

// Initializes Banker's algorithm
// Parameters:
//   num_resources: Number of resources in the system
//   num_threads: Number of threads (customers) in the system
//   available_init: Initial availability of each resource
// Return value:
//   BANKERS_OK if initialization succeeds, otherwise error code
int Bankers_Init(int num_resources, int num_threads, int *available_init);

// Sets the maximum demand of a customer for a resource
// Parameters:
//   customer: Customer number (-1 defaults to OS_Id())
//   resource: Resource number
//   max_demand: Maximum demand of the customer for each resource
// Return value:
//   BANKERS_OK if setting maximum demand succeeds, otherwise error code
int Bankers_SetMaxDemand(int customer, int *max_demand);

// Requests resources in a non-blocking manner
// Parameters:
//   customer: Customer number (-1 defaults to OS_Id())
//   request: Array representing the requested resources
// Return value:
//   BANKERS_OK if the request is granted, otherwise error code
int Bankers_RequestResourcesNonBlocking(int customer, int *request);

// Requests resources in a blocking manner
// Parameters:
//   customer: Customer number (-1 defaults to OS_Id())
//   request: Array representing the requested resources
// Return value:
//   BANKERS_OK if the request is granted, otherwise error code
int Bankers_RequestResourcesBlocking(int customer, int *request);

// Releases resources held by a customer
// Parameters:
//   customer: Customer number (-1 defaults to OS_Id())
//   release: Array representing the released resources
// Return value:
//   BANKERS_OK if resources are released successfully, otherwise error code
int Bankers_ReleaseResources(int customer, int *release);

#endif // BANKERS_H
