#include <kernel/sched.h>
#include <kernel/kmalloc.h>
#include <kernel/stdio.h>
#include <string.h>

#define MAX_THREADS 16
#define STACK_SIZE  (8*1024)
#define AGING_THRESHOLD 32

static const int priority_quantum[SCHED_PRIORITY_LEVELS] = {4, 6, 10, 18};
static const char* priority_names[SCHED_PRIORITY_LEVELS] = {
    "RT", "INT", "BG", "BATCH"
};
static const int DEFAULT_PRIORITY = SCHED_PRIORITY_INTERACTIVE;

typedef enum { T_UNUSED=0, T_READY, T_RUNNING, T_BLOCKED } tstate_t;

struct kthread {
    uint32_t esp;
    tstate_t state;
    char     name[16];
    uint8_t  priority;
    uint8_t  slice_left;
    uint16_t wait_ticks;
};

static struct kthread th[MAX_THREADS];
static int current = -1;

extern void ctx_switch(uint32_t* old_esp, uint32_t new_esp);

static void kthread_trampoline(void);
static void apply_aging(void);
static int select_next(void);
static void refill_slice(int tid);

struct start_pack { kthread_fn fn; void* arg; };

static uint32_t new_stack_with_trampoline(kthread_fn fn, void* arg){
    uint8_t* stk = (uint8_t*)kmalloc(STACK_SIZE);
    if (!stk) return 0;
    memset(stk, 0, STACK_SIZE);
    uint32_t* sp = (uint32_t*)(stk + STACK_SIZE);
    /* push start args for trampoline */
    struct start_pack* pack = (struct start_pack*)(sp - (sizeof(struct start_pack)/4));
    pack->fn = fn; pack->arg = arg;
    sp = (uint32_t*)pack;
    /* simulate call frame return address (unused) */
    *(--sp) = 0;
    /* entry EIP for switch return path */
    *(--sp) = (uint32_t)(uintptr_t)&kthread_trampoline;
    /* minimal callee-saved registers for ctx switch compatibility */
    /* reserve pusha frame: edi,esi,ebp,esp,ebx,edx,ecx,eax if needed by asm */
    for (int i=0;i<8;i++) *(--sp) = 0;
    return (uint32_t)(uintptr_t)sp;
}

void sched_init(void){
    memset(th, 0, sizeof(th));
    /* slot 0 is the bootstrap thread (current CPU context) */
    th[0].state = T_RUNNING; current = 0;
    th[0].name[0]='i'; th[0].name[1]='d'; th[0].name[2]='l'; th[0].name[3]='e'; th[0].name[4]='\0';
    th[0].priority = SCHED_PRIORITY_BATCH;
    th[0].wait_ticks = 0;
    refill_slice(0);
}

int kthread_create(kthread_fn fn, void* arg, const char* name){
    for (int i=1;i<MAX_THREADS;i++){
        if (th[i].state == T_UNUSED){
            uint32_t esp = new_stack_with_trampoline(fn,arg);
            if (!esp) return -1;
            th[i].esp = esp;
            th[i].state = T_READY;
            int j=0; if (name){ while (name[j] && j<15){ th[i].name[j]=name[j]; j++; } }
            th[i].name[j]=0;
            th[i].priority = DEFAULT_PRIORITY;
            th[i].wait_ticks = 0;
            refill_slice(i);
            return i;
        }
    }
    return -1;
}

int sched_set_priority(int pid, int priority){
    if (pid < 0 || pid >= MAX_THREADS) return -1;
    if (priority < SCHED_PRIORITY_REALTIME || priority >= SCHED_PRIORITY_LEVELS) return -1;
    if (th[pid].state == T_UNUSED) return -1;
    th[pid].priority = priority;
    th[pid].wait_ticks = 0;
    refill_slice(pid);
    return 0;
}

void sched_ps(void){
    printf("PID  STATE     PRI  NAME\n");
    for (int i=0;i<MAX_THREADS;i++){
        if (th[i].state!=T_UNUSED){
            const char* st = (th[i].state==T_RUNNING)?"RUNNING":(th[i].state==T_READY?"READY":"BLOCKED");
            const char* pr = (th[i].priority < SCHED_PRIORITY_LEVELS) ? priority_names[th[i].priority] : "??";
            printf("%2d   %-8s %-4s %s%s\n", i, st, pr, th[i].name, (i==current)?" *":"");
        }
    }
}

void sched_yield(void){
    apply_aging();
    int next = select_next();
    if (next < 0 || next == current) return;
    int prev = current;
    th[prev].state = T_READY;
    th[prev].wait_ticks = 0;
    refill_slice(prev);
    th[next].state = T_RUNNING;
    th[next].wait_ticks = 0;
    refill_slice(next);
    current = next;
    ctx_switch(&th[prev].esp, th[next].esp);
}

void sched_tick(void){
    if (current < 0) return;
    for (int i=1;i<MAX_THREADS;i++){
        if (i != current && th[i].state == T_READY){
            th[i].wait_ticks++;
        }
    }
    if (th[current].slice_left > 0) th[current].slice_left--;
    apply_aging();
    int next = select_next();
    if (next >= 0 && (th[next].priority < th[current].priority || th[current].slice_left == 0)){
        th[current].slice_left = priority_quantum[th[current].priority];
        sched_yield();
        return;
    }
    if (th[current].slice_left == 0){
        refill_slice(current);
    }
}

/* Runs on a fresh stack for the new thread */
static void kthread_trampoline(void){
    struct start_pack pack;
    /* stack layout: [pusha..], ret, pack */
    uint32_t* sp; __asm__ volatile ("movl %%esp,%0":"=r"(sp));
    memcpy(&pack, (void*)(sp + 1), sizeof(pack));
    pack.fn(pack.arg);
    /* If function returns, just park */
    for(;;) { __asm__ volatile("hlt"); }
}

static void refill_slice(int tid){
    if (tid < 0 || tid >= MAX_THREADS) return;
    th[tid].slice_left = priority_quantum[th[tid].priority];
}

static void apply_aging(void){
    for (int i = 1; i < MAX_THREADS; i++){
        if (th[i].state == T_READY && th[i].wait_ticks >= AGING_THRESHOLD && th[i].priority > SCHED_PRIORITY_REALTIME){
            th[i].priority--;
            th[i].wait_ticks = 0;
            refill_slice(i);
        }
    }
}

static int select_next(void){
    int best = -1;
    int best_prio = SCHED_PRIORITY_LEVELS;
    int best_wait = -1;
    for (int i = 1; i < MAX_THREADS; i++){
        if (th[i].state == T_READY){
            int prio = th[i].priority;
            int wait = th[i].wait_ticks;
            if (best < 0 || prio < best_prio || (prio == best_prio && wait > best_wait)){
                best = i;
                best_prio = prio;
                best_wait = wait;
            }
        }
    }
    return best;
}
