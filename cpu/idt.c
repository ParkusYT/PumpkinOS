/* ===========================================================================
 * PumpkinOS - Interrupt Descriptor Table + top-level interrupt dispatcher
 * ========================================================================= */
#include "idt.h"
#include "isr.h"
#include "pic.h"
#include "console.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "floppy.h"
#include "paging.h"
#include "sched.h"
#include "syscall.h"
#include <stdint.h>

/* A single 32-bit IDT gate descriptor. */
struct idt_entry {
    uint16_t offset_low;    /* handler address bits  0..15 */
    uint16_t selector;      /* code segment selector (0x08) */
    uint8_t  zero;          /* always 0 */
    uint8_t  type_attr;     /* gate type + attributes */
    uint16_t offset_high;   /* handler address bits 16..31 */
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr   idtp;

/* The stubs defined in isr.asm. */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

extern void isr128(void);   /* the int 0x80 syscall stub */

static void (*const isr_stubs[32])(void) = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
};

static void (*const irq_stubs[16])(void) = {
    irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7,
    irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15,
};

static const char *const exception_names[32] = {
    "Divide-by-zero",           "Debug",
    "Non-maskable interrupt",   "Breakpoint",
    "Overflow",                 "Bound range exceeded",
    "Invalid opcode",           "Device not available",
    "Double fault",             "Coprocessor segment overrun",
    "Invalid TSS",              "Segment not present",
    "Stack-segment fault",      "General protection fault",
    "Page fault",               "Reserved",
    "x87 floating-point",       "Alignment check",
    "Machine check",            "SIMD floating-point",
    "Virtualization",           "Control protection",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved",
};

#define KERNEL_CS   0x08
#define GATE_INT32  0x8E   /* present, ring 0, 32-bit interrupt gate */
#define GATE_SYS32  0xEE   /* present, ring 3, 32-bit interrupt gate (syscalls) */

static void idt_set_gate(int n, uint32_t handler, uint16_t sel, uint8_t flags) {
    idt[n].offset_low  = handler & 0xFFFF;
    idt[n].selector    = sel;
    idt[n].zero        = 0;
    idt[n].type_attr   = flags;
    idt[n].offset_high = (handler >> 16) & 0xFFFF;
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;

    for (int i = 0; i < 256; i++)
        idt_set_gate(i, 0, 0, 0);

    for (int i = 0; i < 32; i++)
        idt_set_gate(i, (uint32_t)isr_stubs[i], KERNEL_CS, GATE_INT32);

    for (int i = 0; i < 16; i++)
        idt_set_gate(32 + i, (uint32_t)irq_stubs[i], KERNEL_CS, GATE_INT32);

    /* The syscall gate needs DPL 3 so ring-3 code is allowed to invoke it. */
    idt_set_gate(128, (uint32_t)isr128, KERNEL_CS, GATE_SYS32);

    __asm__ volatile("lidt %0" : : "m"(idtp));
}

/* ---- the actual dispatcher, called from isr.asm --------------------------- */
void isr_handler(struct registers *r) {
    if (r->int_no == 128) {
        /* int 0x80: a system call from user (or kernel) code. */
        syscall_dispatch(r);
        return;
    }

    if (r->int_no == 14) {
        /* Page fault gets a dedicated reporter that decodes CR2. */
        paging_fault(r);
    }

    if (r->int_no < 32) {
        /* Unhandled CPU exception: report it and stop rather than silently
         * triple-faulting. */
        console_set_color(VGA_WHITE, VGA_RED);
        console_write("\n\n *** CPU EXCEPTION *** ");
        console_write(exception_names[r->int_no]);
        console_write(" (vector ");
        console_write_dec(r->int_no);
        console_write(", error code ");
        console_write_hex(r->err_code);
        console_write(")\n at eip=");
        console_write_hex(r->eip);
        console_write("\n PumpkinOS halted.\n");
        for (;;)
            __asm__ volatile("cli; hlt");
    }

    /* Otherwise it is a hardware IRQ (vectors 32..47). */
    int irq = r->int_no - 32;
    if (irq == 0) {
        timer_irq();
        pic_send_eoi(irq);      /* ack before we possibly switch tasks */
        sched_tick();           /* preempt: may not return to here for a while */
        return;
    }
    if (irq == 1)
        keyboard_irq();
    else if (irq == 6)
        floppy_irq();
    else if (irq == 12)
        mouse_irq();

    pic_send_eoi(irq);
}
