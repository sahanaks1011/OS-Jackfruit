#ifndef COMMON_H
#define COMMON_H

#define MAX_PROCESSES 100

typedef struct {
    int pid;
    int memory_used;
    int cpu_time;
    int active;
} process_info;

#endif