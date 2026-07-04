/* ===========================================================================
 * PumpkinOS - system call dispatch
 * ========================================================================= */
#include "syscall.h"
#include "console.h"
#include "sched.h"
#include <stdint.h>

void syscall_dispatch(struct registers *r) {
    switch (r->eax) {
    case SYS_EXIT:
        task_exit();               /* never returns */
        break;

    case SYS_PUTC:
        console_putc((char)(r->ebx & 0xFF));
        r->eax = 0;
        break;

    case SYS_WRITE: {
        /* EBX = pointer (a user virtual address the kernel can read), ECX = len */
        const char *p = (const char *)r->ebx;
        for (uint32_t i = 0; i < r->ecx; i++)
            console_putc(p[i]);
        r->eax = r->ecx;
        break;
    }

    case SYS_GETCPL:
        /* The low 2 bits of the saved CS are the caller's privilege level. */
        r->eax = r->cs & 3;
        break;

    default:
        r->eax = (uint32_t)-1;     /* unknown syscall */
        break;
    }
}
