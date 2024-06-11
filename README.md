# deadlock-detection

This project aims to enhance the functionality of a real-time operating system (RTOS) designed for the TM4C123G micro-controller, by implementing deadlock avoidance and detection mechanisms. For avoidance, we explore Banker's algorithm to ensure safe resource allocation in a system with a pre-defined amount of threads and knowledge on the maximum request limits of each thread. However, this knowledge is often not known ahead of time, so we also explore periodically detecting (and breaking) potential deadlocks by constructing a wait-for graph.
