/* ===========================================================================
 * PumpkinOS - cooperative/preemptive round-robin scheduler (kernel threads)
 * ========================================================================= */
#ifndef PUMPKIN_SCHED_H
#define PUMPKIN_SCHED_H

#include <stdint.h>

typedef struct task {
    uint32_t          esp;      /* saved stack pointer while switched out */
    uint32_t          id;
    const char       *name;
    int               state;    /* 0 = alive, 1 = dead */
    void            (*entry)(void *);
    void             *arg;
    uint8_t          *stack;    /* kmalloc'd stack (NULL for the boot task) */
    uint32_t          kstack_top; /* top of the kernel stack (loaded into TSS esp0) */
    volatile uint32_t counter;  /* per-task scratch counter (used by demos) */
    struct task      *next;     /* circular run queue */
} task_t;

/* Turn the currently-running boot context into task 0. */
void sched_init(void);

/* Create a new kernel thread that will run entry(arg). */
task_t *task_spawn(void (*entry)(void *), void *arg, const char *name);

/* Round-robin to the next task. Called from the timer IRQ (preemptive). */
void sched_tick(void);

/* Voluntarily give up the CPU. */
void task_yield(void);

/* End the current task (never returns). */
void task_exit(void);

/* The task running right now. */
task_t *sched_current(void);

/* How many tasks are in the run queue. */
uint32_t sched_count(void);

#endif /* PUMPKIN_SCHED_H */
