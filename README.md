# PumpkinOS

A tiny 32-bit, BIOS-only operating system for old x86 machines, written in C
and a little assembly. It boots from a 1.44 MB floppy image, switches the CPU
into 32-bit protected mode, and drops you into an interactive shell driven by
a PS/2 keyboard, with output mirrored to the VGA text console and COM1 serial.

It currently has: a two-stage-ish boot into protected mode, an interrupt
descriptor table with a CPU-exception handler, a remapped 8259 PIC, an
interrupt-driven PS/2 keyboard driver, and **PumpkinShell (PSH)**.

## What's here

| File                 | Role                                                          |
|----------------------|--------------------------------------------------------------|
| `boot/boot.asm`      | 512-byte BIOS boot sector: loads the kernel (INT 13h), enables A20, sets up a flat GDT, enters 32-bit protected mode, jumps to the kernel. |
| `kernel/entry.asm`   | 32-bit entry stub: zeroes `.bss`, sets up the stack, calls `kernel_main`. |
| `kernel/kernel.c`    | Kernel entry: brings up console, IDT, PIC, keyboard, then runs the shell. |
| `kernel/console.{c,h}` | VGA text driver (colour, scrolling, hardware cursor) + COM1 serial mirror. |
| `kernel/isr.asm`     | Interrupt stubs for CPU exceptions 0–31 and IRQs 0–15.       |
| `kernel/idt.{c,h}`   | Builds/loads the IDT; dispatches exceptions and IRQs.        |
| `kernel/pic.{c,h}`   | Remaps the 8259 PIC (IRQs → vectors 32–47) and sends EOIs.   |
| `kernel/keyboard.{c,h}` | PS/2 keyboard driver: IRQ1, scancode set 1, shift/caps, ring buffer. |
| `kernel/shell.{c,h}` | PumpkinShell (PSH): the interactive command loop.            |
| `kernel/string.{c,h}`| Freestanding `memset`/`memcpy`/`strcmp`/…                    |
| `kernel/io.h`        | `inb`/`outb`/`io_wait` port helpers.                         |
| `linker.ld`          | Links the kernel as a flat binary at physical address `0x1000`. |
| `Makefile`           | Builds everything into `pumpkinos.img`.                      |

## PumpkinShell commands

```
help          list the built-in commands
clear, cls    clear the screen
echo <text>   print <text>
banner        draw the PumpkinOS banner
about         about PumpkinOS
colors        show the 16-colour VGA palette
reboot        restart the machine (via the 8042 controller)
halt          stop the CPU
```

## Boot flow

```
BIOS  --loads sector 0 to 0x7C00-->  boot.asm (16-bit real mode)
  1. set up segments + stack, save boot drive
  2. read the kernel from the floppy into memory at 0x1000  (LBA -> CHS, with retries)
  3. enable the A20 line
  4. load the GDT, set CR0.PE, far-jump into 32-bit protected mode
  5. jump to 0x1000
                       |
                       v
entry.asm (32-bit): zero .bss, set stack --> kernel_main() in kernel.c
  1. console_init()  -- VGA + serial
  2. idt_init()      -- interrupt descriptor table + exception handler
  3. pic_remap()     -- 8259 PIC: IRQs -> vectors 32..47
  4. keyboard_init() -- drain the 8042, unmask IRQ1
  5. sti             -- enable interrupts
  6. shell_run()     -- PumpkinShell REPL (reads keys via IRQ1, never returns)
```

## Interrupts & the keyboard

The IDT points all 256 vectors at assembly stubs in `isr.asm`, which funnel
into one C dispatcher (`isr_handler`). CPU exceptions (0–31) print a diagnostic
and halt instead of silently triple-faulting. Hardware IRQs (remapped to
32–47) are acknowledged with an EOI to the PIC. IRQ1 (the keyboard) reads a
scancode from port `0x60`, translates it (scancode set 1, US layout, with
shift/caps handling), and pushes the character into a ring buffer. The shell's
`keyboard_getchar()` sleeps on `hlt` until a key arrives, so the CPU idles
between keystrokes instead of busy-waiting.

## Memory layout (physical)

```
0x00000000  Real-mode IVT / BIOS data area
0x00001000  Kernel image (loaded here, grows upward)
   ...      (kernel load budget ends at 0x7000 -- see MAX_KERNEL_SECTORS)
0x00007C00  Boot sector (running) + stack growing down from here
0x00090000  Protected-mode kernel stack top
0x000B8000  VGA text framebuffer (80x25)
```

The build measures the kernel and tells the boot sector *exactly* how many
sectors to read, so the disk load never overruns into the boot code at `0x7C00`.

## Build & run

You need: `gcc` (with 32-bit support), `binutils` (`ld`, `objcopy`), `nasm`,
and `qemu-system-i386`.

```sh
make            # build pumpkinos.img
make run        # boot it in QEMU (graphical window)
make run-serial # boot headless, mirror the kernel's serial output to your terminal
make clean      # remove build artifacts
```

### Writing to a real floppy

```sh
sudo dd if=pumpkinos.img of=/dev/fd0 bs=512
```

(Or write the image to a USB stick / use it with a floppy emulator on real
hardware. The image is a standard 1.44 MB raw floppy.)

## Notes / next steps

- Done so far: protected mode, IDT + exception handler, PIC remap, PS/2
  keyboard, and the shell. Natural next steps: a PIT timer (IRQ0) for uptime /
  scheduling, a simple physical memory manager, then paging.
- Compiled with `-march=i386` so it should run on genuine 386/486-era hardware.
- COM1 serial output (38400 8N1) mirrors the console, which makes debugging on
  real hardware or headless QEMU much easier.
