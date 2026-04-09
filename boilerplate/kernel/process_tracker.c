#include <linux/kernel.h>
#include "process_tracker.h"

static PCB process_table[MAX_PROCESSES];

void init_process_table() {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_table[i].pid = -1;
        process_table[i].state = NEW;
    }
    printk("Process table initialized\n");
}

PCB* get_process_table() {
    return process_table;
}

int create_process(int pid) {
    if (pid < 0 || pid >= MAX_PROCESSES) {
        printk("Invalid PID\n");
        return -1;
    }

    if (process_table[pid].pid != -1) {
        printk("Process already exists\n");
        return -1;
    }

    process_table[pid].pid = pid;
    process_table[pid].state = READY;

    printk("Process %d created → READY\n", pid);
    return 0;
}

int terminate_process(int pid) {
    if (pid < 0 || pid >= MAX_PROCESSES)
        return -1;

    process_table[pid].state = TERMINATED;

    printk("Process %d → TERMINATED\n", pid);
    return 0;
}
