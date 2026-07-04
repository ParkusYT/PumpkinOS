/* ===========================================================================
 * PumpkinOS - scheduler
 * ---------------------------------------------------------------------------
 * A round-robin scheduler over a circular list of kernel threads. Each task
 * has its own kmalloc'd stack; the boot context becomes task 0 (which goes on
 * to run the shell). The timer IRQ calls sched_tick() to preempt, and tasks
 * may also task_yield() voluntarily.
 *
 * The magic is in switch.asm's context_switch(): switching out a task saves
 * its registers on its own stack and remembers its ESP; switching back in
 * restores them. A brand-new task gets a hand-crafted stack that makes the
 * first switch "return" into task_bootstrap().
 * ========================================================================= */
#include "sched.h"
#include "kheap.h"
#include "gdt.h"
#include <stdint.h>

#define STACK_SIZE 8192

extern void context_switch(uint32_t *old_esp, uint32_t new_esp);

static task_t  *current = 0;
static uint32_t next_id = 0;
static uint32_t ntasks  = 0;

static void task_bootstrap(void);

task_t  *sched_current(void) { return current; }
uint32_t sched_count(void)   { return ntasks; }

/* Save EFLAGS and disable interrupts; returned value is passed to irq_restore. */
static inline uint32_t irq_save(void) {
    uint32_t flags;
    __asm__ volatile("pushf ; pop %0 ; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void irq_restore(uint32_t flags) {
    __asm__ volatile("push %0 ; popf" : : "r"(flags) : "memory", "cc");
}

void sched_init(void) {
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    t->esp     = 0;                 /* filled the first time it is switched out */
    t->id      = next_id++;
    t->name    = "kmain";
    t->state   = 0;
    t->entry   = 0;
    t->arg     = 0;
    t->stack   = 0;                 /* runs on the boot stack */
    t->kstack_top = 0x90000;        /* the boot stack top set up in entry.asm */
    t->counter = 0;
    t->next    = t;                 /* a ring of one */
    current    = t;
    ntasks     = 1;
}

task_t *task_spawn(void (*entry)(void *), void *arg, const char *name) {
    task_t  *t     = (task_t *)kmalloc(sizeof(task_t));
    uint8_t *stack = (uint8_t *)kmalloc(STACK_SIZE);

    t->id      = next_id++;
    t->name    = name;
    t->state   = 0;
    t->entry   = entry;
    t->arg     = arg;
    t->stack   = stack;
    t->kstack_top = (uint32_t)(stack + STACK_SIZE);
    t->counter = 0;

    /* Craft an initial stack matching what context_switch pops:
     * (low) EFLAGS, EDI, ESI, EBX, EBP, return-address (high). */
    uint32_t *sp = (uint32_t *)(stack + STACK_SIZE);
    *(--sp) = (uint32_t)task_bootstrap;   /* context_switch 'ret' lands here */
    *(--sp) = 0;                          /* ebp */
    *(--sp) = 0;                          /* ebx */
    *(--sp) = 0;                          /* esi */
    *(--sp) = 0;                          /* edi */
    *(--sp) = 0x202;                      /* eflags: reserved bit + IF set   */
    t->esp  = (uint32_t)sp;

    /* Splice into the ring right after the current task. */
    uint32_t flags = irq_save();
    t->next        = current->next;
    current->next  = t;
    ntasks++;
    irq_restore(flags);
    return t;
}

/* Next alive task in the ring. */
static task_t *pick_next(void) {
    task_t *t = current->next;
    while (t->state != 0 && t != current)
        t = t->next;
    return t;
}

/* Switch to 'next', updating the TSS so that if 'next' is (or becomes) a
 * ring-3 task, its next trap lands on its own kernel stack. */
static void switch_to(task_t *next) {
    task_t *prev = current;
    current = next;
    tss_set_esp0(next->kstack_top);
    context_switch(&prev->esp, next->esp);
}

/* Preemptive entry point: called from the timer IRQ with interrupts disabled. */
void sched_tick(void) {
    if (current == 0)
        return;
    task_t *next = pick_next();
    if (next != current)
        switch_to(next);
}

void task_yield(void) {
    uint32_t flags = irq_save();
    task_t  *next  = pick_next();
    if (next != current)
        switch_to(next);
    irq_restore(flags);
}

/* First thing a new task runs: call its entry, then retire cleanly. */
static void task_bootstrap(void) {
    task_t *self = current;
    self->entry(self->arg);
    task_exit();
}

void task_exit(void) {
    __asm__ volatile("cli");
    current->state = 1;                 /* mark dead */

    /* Unlink from the ring. */
    task_t *p = current;
    while (p->next != current)
        p = p->next;
    p->next = current->next;
    ntasks--;

    /* Switch away for good. (The dead task's stack/TCB are leaked for now -
     * a reaper would free them from another task's context.) */
    task_t *dead = current;
    current = current->next;
    tss_set_esp0(current->kstack_top);
    context_switch(&dead->esp, current->esp);
    /* not reached */
}
