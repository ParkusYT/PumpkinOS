# PumpkinOS

A tiny 32-bit, BIOS-only operating system for old x86 machines, written in C
and a little assembly. It boots from a 1.44 MB floppy image, switches the CPU
into 32-bit protected mode, and drops you into an interactive shell driven by
a PS/2 keyboard, with output mirrored to the VGA text console and COM1 serial.

It currently has: a two-stage-ish boot into protected mode, an interrupt
descriptor table with CPU-exception and page-fault handlers, a remapped 8259
PIC, a PIT timer (system tick), an interrupt-driven PS/2 keyboard driver, a
physical memory manager (bitmap frame allocator over the BIOS E820 map),
paging (identity-mapped, with a `map`/`unmap` API), a kernel heap
(`kmalloc`/`kfree`), and **PumpkinShell (PSH)**.

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
| `kernel/timer.{c,h}` | PIT (8254) on IRQ0: 100 Hz system tick, uptime, `timer_sleep`. |
| `kernel/pmm.{c,h}`   | Physical memory manager: bitmap allocator over the E820 map. |
| `kernel/paging.{c,h}`| 32-bit paging: identity map, `paging_map`/`unmap`, page-fault reporter. |
| `kernel/kheap.{c,h}` | Kernel heap: first-fit `kmalloc`/`kfree` over paged frames. |
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
uptime        time since boot (from the PIT tick counter)
sleep <sec>   pause for <sec> seconds (uses timer_sleep)
meminfo       print the BIOS E820 map + frame allocator stats
memtest       allocate/free a few frames (exercises the allocator)
pgtest        map a frame to a high virtual address and prove the translation
pgfault       deliberately touch unmapped memory (shows the fault handler; halts)
heap          show kernel heap statistics
htest         exercise kmalloc/kfree (alloc, write/read, free, reuse)
reboot        restart the machine (via the 8042 controller)
halt          stop the CPU
```

## Boot flow

```
BIOS  --loads sector 0 to 0x7C00-->  boot.asm (16-bit real mode)
  1. set up segments + stack, save boot drive
  2. gather the BIOS memory map (INT 15h/E820) into 0x0500  (real mode only!)
  3. read the kernel from the floppy into memory at 0x1000  (LBA -> CHS, with retries)
  4. enable the A20 line
  5. load the GDT, set CR0.PE, far-jump into 32-bit protected mode
  6. jump to 0x1000
                       |
                       v
entry.asm (32-bit): zero .bss, set stack --> kernel_main() in kernel.c
  1. console_init()  -- VGA + serial
  2. pmm_init()      -- parse the E820 map, build the frame bitmap
  3. idt_init()      -- interrupt descriptor table + exception handler
  4. pic_remap()     -- 8259 PIC: IRQs -> vectors 32..47
  5. timer_init(100) -- PIT on IRQ0, 100 Hz system tick
  6. keyboard_init() -- drain the 8042, unmask IRQ1
  7. paging_init()   -- identity-map RAM, load CR3, set CR0.PG
  8. kheap_init()    -- reserve the heap's virtual region at 3 GiB
  9. sti             -- enable interrupts
 10. shell_run()     -- PumpkinShell REPL (reads keys via IRQ1, never returns)
```

## Interrupts & the keyboard

The IDT points all 256 vectors at assembly stubs in `isr.asm`, which funnel
into one C dispatcher (`isr_handler`). CPU exceptions (0–31) print a diagnostic
and halt instead of silently triple-faulting. Hardware IRQs (remapped to
32–47) are acknowledged with an EOI to the PIC. IRQ0 (the PIT) bumps a tick
counter 100 times a second, which backs `uptime` and `timer_sleep`. IRQ1
(the keyboard) reads a
scancode from port `0x60`, translates it (scancode set 1, US layout, with
shift/caps handling), and pushes the character into a ring buffer. The shell's
`keyboard_getchar()` sleeps on `hlt` until a key arrives, so the CPU idles
between keystrokes instead of busy-waiting.

## Physical memory

BIOS calls only work in real mode, so the bootloader queries the memory map
(`INT 15h`, `EAX=0xE820`) *before* switching to protected mode and leaves the
entry count + array at `0x0500`. In the kernel, `pmm_init()` reads that map and
builds a **bitmap frame allocator**: one bit per 4 KiB physical frame, sized to
the highest usable address. It marks everything used, frees the usable E820
regions, then re-reserves the first 1 MiB (BIOS/kernel/video) and the bitmap's
own storage. `pmm_alloc_frame()` / `pmm_free_frame()` hand out and reclaim
frames; `meminfo` prints the map and stats. All arithmetic is 32-bit / shift
based on purpose — the kernel links without libgcc, so there is no `__udivdi3`
for 64-bit division.

## Paging

`paging_init()` builds classic 32-bit two-level paging: a 1024-entry page
directory, each entry pointing at a 1024-entry page table, each mapping a 4 KiB
frame (page tables come from the PMM). It **identity-maps** all usable RAM
(virtual == physical), loads CR3, and sets `CR0.PG` — so the running kernel,
stack, PMM bitmap, VGA buffer, and the page structures themselves keep working
the instant paging turns on. Only 4 KiB pages are used and the TLB is flushed
by reloading CR3 (not `invlpg`), so it runs on a plain i386 with no PSE.

`paging_map(virt, phys, flags)` / `paging_unmap(virt)` add and remove mappings
(allocating page tables on demand); `paging_get_phys(virt)` walks the tables.
Vector 14 (page fault) is routed to `paging_fault()`, which decodes CR2 and the
error code before halting. The `pgtest` and `pgfault` shell commands exercise
both paths.

## Kernel heap

`kmalloc`/`kfree` run a **first-fit free-list allocator** over a virtual region
at `0xC0000000` (3 GiB) — deliberately above the identity map, so every heap
page is a physical frame pulled from the PMM and mapped in on demand via
`paging_map`. When the heap runs low it grows by mapping more frames onto its
tail. Blocks carry a 16-byte header (payloads are 8-byte aligned); `kmalloc`
splits an oversized free block, and `kfree` marks a block free and coalesces
adjacent free neighbours. `htest` allocates, writes/reads a pattern (proving the
pages are real), frees, and shows the freed space getting reused.

## Memory layout (physical)

```
0x00000000  Real-mode IVT / BIOS data area
0x00000500  BIOS E820 memory map (count + entries, from the bootloader)
0x00001000  Kernel image (loaded here, grows upward)
   ...      (kernel load budget ends at 0x7000 -- see MAX_KERNEL_SECTORS)
0x00007C00  Boot sector (running) + stack growing down from here
0x00090000  Protected-mode kernel stack top
0x000B8000  VGA text framebuffer (80x25)
0x00100000  Physical-memory-manager bitmap, then free frames
0xC0000000  Kernel heap (virtual; frames mapped in on demand)
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

- Done so far: protected mode, IDT + exception/page-fault handlers, PIC remap,
  PIT timer, PS/2 keyboard, physical memory manager, paging, a kernel heap, and
  the shell. Natural next steps: **multitasking** (a task struct + context
  switch on the timer tick), then user mode (ring 3) + syscalls, then a simple
  filesystem so the shell can load programs.
- The bootloader gathers the memory map with `INT 15h/E820`, which every PC
  BIOS since ~1994 supports; genuinely ancient machines without it would need
  an `E801`/`AH=88h` fallback added to `do_e820`.
- Compiled with `-march=i386` so it should run on genuine 386/486-era hardware.
- COM1 serial output (38400 8N1) mirrors the console, which makes debugging on
  real hardware or headless QEMU much easier.
