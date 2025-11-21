#ifndef _KERNEL_SCHED_H
#define _KERNEL_SCHED_H

#include <stdint.h>

typedef void (*kthread_fn)(void*);

#define SCHED_PRIORITY_REALTIME   0
#define SCHED_PRIORITY_INTERACTIVE 1
#define SCHED_PRIORITY_BACKGROUND  2
#define SCHED_PRIORITY_BATCH       3
#define SCHED_PRIORITY_LEVELS      4

void sched_init(void);
int  kthread_create(kthread_fn fn, void* arg, const char* name);
int  sched_set_priority(int pid, int priority);
void sched_yield(void);
void sched_tick(void); /* call from timer IRQ */
void sched_ps(void);

#endif
