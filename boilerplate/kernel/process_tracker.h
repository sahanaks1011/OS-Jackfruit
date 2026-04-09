#ifndef PROCESS_TRACKER_H
#define PROCESS_TRACKER_H

#include "../include/common.h"

void init_process_table();
int create_process(int pid);
int terminate_process(int pid);
PCB* get_process_table();

#endif
