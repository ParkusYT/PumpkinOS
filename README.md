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
(`kmalloc`/`kfree`), preemptive multitasking (round-robin kernel threads
switched on the timer tick), user mode (ring 3 with a TSS and an `int 0x80`
syscall interface), a floppy disk driver (8272 FDC + ISA DMA) with a read/write
FAT12 filesystem (files and directories) the kernel boots from as a file, and
**PumpkinShell (PKSH)**.

## What's here

The source is organised by subsystem; the build compiles every `.c`/`.asm`
under these directories (with each on the `-I` header path).

| Path                    | Role                                                       |
|-------------------------|------------------------------------------------------------|
| `boot/boot.asm`         | 512-byte FAT12 boot sector: carries a BPB, finds `KERNEL.BIN`, follows its cluster chain to load it at 0x10000, gathers the E820 map, enables A20, loads a GDT, enters protected mode, jumps to the kernel. |
| `kernel/entry.asm`      | 32-bit kernel entry: zeroes `.bss`, sets up the stack, calls `kernel_main`. |
| `kernel/kernel.c`       | Kernel entry: brings every subsystem up in order, then runs the shell. |
| `cpu/gdt.{c,h}`         | Kernel GDT (ring 0/3 segments) + TSS for ring-3 → ring-0 traps. |
| `cpu/idt.{c,h}`         | Builds/loads the IDT; dispatches exceptions, IRQs, syscalls. |
| `cpu/isr.asm`           | Interrupt stubs for exceptions 0–31, IRQs 0–15, `int 0x80`. |
| `cpu/pic.{c,h}`         | Remaps the 8259 PIC (IRQs → vectors 32–47) and sends EOIs. |
| `cpu/syscall.{c,h}`     | System-call dispatch (exit/putc/write/getcpl).            |
| `cpu/usermode.asm`      | `enter_user_mode` - iret down to ring 3.                   |
| `cpu/io.h`              | `inb`/`outb`/`io_wait` port helpers.                       |
| `mm/pmm.{c,h}`          | Physical memory manager: bitmap allocator over the E820 map. |
| `mm/paging.{c,h}`       | 32-bit paging: identity map, `paging_map`/`unmap`, page-fault reporter. |
| `mm/kheap.{c,h}`        | Kernel heap: first-fit `kmalloc`/`kfree` over paged frames. |
| `drivers/console.{c,h}` | VGA text driver (colour, scrolling, hardware cursor) + COM1 serial mirror. |
| `drivers/timer.{c,h}`   | PIT (8254) on IRQ0: 100 Hz system tick, uptime, `timer_sleep`. |
| `drivers/keyboard.{c,h}`| PS/2 keyboard driver: IRQ1, scancode set 1, shift/caps, ring buffer. |
| `drivers/floppy.{c,h}`  | Floppy disk driver: 8272 FDC + ISA DMA channel 2, IRQ6, reads/writes sectors. |
| `sched/sched.{c,h}`     | Round-robin scheduler: kernel threads, `task_spawn`, preemption. |
| `sched/switch.asm`      | `context_switch` - saves/restores registers and swaps stacks. |
| `shell/shell.{c,h}`     | PumpkinShell (PKSH): the interactive command loop.         |
| `fs/fat12.{c,h}`        | Read/write FAT12 driver: files, directories, `ls`/`cat`/`cd`/`mkdir`/`write`/`rm`. |
| `user/user.asm`         | Position-independent ring-3 demo programs (run via syscalls). |
| `lib/string.{c,h}`      | Freestanding `memset`/`memcpy`/`strcmp`/…                  |
| `fsroot/`               | Files copied into the FAT12 image at build time.          |
| `linker.ld`             | Links the kernel as a flat binary at physical address `0x1000`. |
| `Makefile`           | Builds everything into `pumpkinos.img`.                      |

## PumpkinShell commands

```
help          list the built-in commands
clear, cls    clear the screen
echo <text>   print <text>
ls            list the current directory
cd <dir>      change directory (`..` and `/` work)
pwd           print the working directory
cat <file>    print a file's contents (case-insensitive name)
write <f> <t> create/overwrite file <f> with text <t>
touch <file>  create an empty file
mkdir <name>  create a directory
rm <file>     delete a file
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
tasks         list scheduler tasks + counters (run twice: workers climb)
user          drop to ring 3 and run a program that talks via int 0x80
userfault     ring-3 task reads kernel memory -> protection fault (halts)
reboot        restart the machine (via the 8042 controller)
halt          stop the CPU
```

## Boot flow

```
BIOS  --loads sector 0 to 0x7C00-->  boot.asm (16-bit real mode, FAT12)
  1. set up segments + stack, save boot drive
  2. read the root directory, scan it for "KERNEL  BIN"
  3. read the FAT, follow KERNEL.BIN's cluster chain, load it HIGH at 0x10000
  4. gather the BIOS memory map (INT 15h/E820) into 0x0500  (real mode only!)
  5. enable A20, load the GDT, set CR0.PE
  6. far-jump into 32-bit protected mode at 0x10000
                       |
                       v
entry.asm (32-bit): zero .bss, set stack --> kernel_main() in kernel.c
  1. console_init()  -- VGA + serial
  2. gdt_init()      -- kernel GDT with ring 0/3 segments + TSS
  3. pmm_init()      -- parse the E820 map, build the frame bitmap
  4. idt_init()      -- IDT: exception, IRQ and int 0x80 (syscall) gates
  5. pic_remap()     -- 8259 PIC: IRQs -> vectors 32..47
  6. timer_init(100) -- PIT on IRQ0, 100 Hz system tick
  7. keyboard_init() -- drain the 8042, unmask IRQ1
  8. paging_init()   -- identity-map RAM, load CR3, set CR0.PG
  9. kheap_init()    -- reserve the heap's virtual region at 3 GiB
 10. sched_init()    -- turn this boot context into task 0, spawn demo threads
 11. sti             -- enable interrupts (preemption starts)
 12. floppy_init()   -- reset the FDC (needs IRQ6 + the timer, so after sti)
 13. fs_init()       -- read the FAT12 BPB/FAT/root off the floppy
 14. shell_run()     -- PumpkinShell REPL (task 0; reads keys via IRQ1)
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

## Multitasking

The boot context becomes task 0 (which runs the shell). `task_spawn()` creates
a kernel thread: it `kmalloc`s a stack and hand-crafts an initial frame so the
first switch "returns" into a bootstrap that calls the thread's entry function.
Tasks live in a circular run queue. On every timer tick, the IRQ0 handler
(after acking the PIC) calls `sched_tick()`, which round-robins to the next
task via `context_switch` (in `switch.asm`): it pushes the callee-saved
registers + EFLAGS onto the outgoing task's stack, saves its ESP, loads the
incoming task's ESP, and pops - so each task resumes exactly where it left off,
on its own stack. Because the interrupt frame lives on the task's own stack,
switching away mid-interrupt and back later Just Works.

The two `worker-*` threads spawned at boot spin forever incrementing a counter
and never yield, so `tasks` showing their counters climb between runs is direct
proof that the timer is preempting them - all while the shell (task 0) stays
responsive.

## User mode (ring 3)

`gdt_init()` installs a kernel-owned GDT with ring-3 code/data descriptors and
a **TSS**. The `user` command spawns a kernel thread that allocates a user
code + stack page (mapped `USER`, at `0x40000000`), copies in a
position-independent ring-3 blob (`user/user.asm`), points `tss.esp0` at its
own kernel stack, and `enter_user_mode()` builds an `iret` frame to drop to
CPL 3.

From there the program can only reach the kernel through `int 0x80`: the number
goes in `EAX`, args in `EBX`/`ECX`, the result comes back in `EAX`. The demo
prints a message with `SYS_PUTC`, asks `SYS_GETCPL` (the kernel returns the low
2 bits of the caller's saved CS), prints the digit - a literal **3** proving it
runs at ring 3 - then `SYS_EXIT`s. On every context switch the scheduler
updates `tss.esp0` to the incoming task's kernel stack, so a ring-3 task can be
preempted and resumed safely.

Protection is real: the identity map is supervisor-only, so `userfault` (a
ring-3 read of a kernel page) takes a page fault - *protection violation, on
read, user mode* - rather than being allowed.

## Floppy driver + FAT12 filesystem

The **whole floppy is a single FAT12 volume** (a normal disk you can inspect
with `mdir`/`mcopy` or mount on Linux). The kernel itself lives inside it as the
file `KERNEL.BIN`, next to the `fsroot/` text files - so `ls` shows the actual
kernel that boots the machine. The boot sector is a real FAT12 boot sector: it
carries a BPB and parses the root directory + FAT to find and load `KERNEL.BIN`.

Once running, the kernel reads the same disk **off the real drive** with a
floppy disk controller driver (`drivers/floppy.c`). To fetch a sector it spins
up the motor, seeks the cylinder, programs **ISA DMA channel 2** to receive the
data into a low bounce buffer, issues the FDC *Read Data* command, and waits
for the drive's **IRQ 6**; the sector arrives in memory by DMA. Because that
needs interrupts (IRQ 6) and the timer (motor spin-up), the driver and
filesystem come up *after* `sti`.

`fs/fat12.c` mounts the volume by caching the FAT in memory (flushed back to
both on-disk copies whenever it changes); directory sectors and file data are
read-modify-written on demand. It supports the root directory and
subdirectories, with a current working directory shown in the prompt:

- `ls` / `cd` / `pwd` - list and walk directories (`.`, `..`, `/`).
- `cat <file>` - stream a file off the disk, following its cluster chain.
- `write <f> <t>` / `touch` - allocate clusters, write the data, and add a
  directory entry.
- `mkdir <name>` - allocate a cluster, seed it with `.`/`..` entries, and link
  it into the parent.
- `rm <file>` - free the cluster chain and mark the entry deleted.

Everything is written straight to the floppy, so changes **persist across
reboots** and the disk stays a valid FAT12 volume - you can create a file in
PumpkinOS and read it back on Linux with `mtype`/`mdir`. (Directory-entry
timestamps are left at zero; there is no real-time clock yet.)

Files placed in `fsroot/` are copied into the volume at build time; `make`
formats the disk, copies `KERNEL.BIN` and those files in, and overlays the boot
sector.

## Memory layout (physical)

```
0x00000000  Real-mode IVT / BIOS data area
0x00000500  BIOS E820 memory map (count + entries, from the bootloader)
0x00007C00  Boot sector (running) + real-mode stack below it
0x00007E00  Boot-time scratch: FAT (0x7E00) then root directory (0x9000)
0x00010000  KERNEL.BIN (loaded here, grows upward; < 0x80000 -- MAX_KERNEL_SECTORS)
0x00080000  Floppy DMA bounce buffer (ISA DMA, < 16 MB, 64 KB-aligned)
0x00090000  Protected-mode kernel stack top
0x000B8000  VGA text framebuffer (80x25)
0x00100000  Physical-memory-manager bitmap, then free frames
0x40000000  User-mode code + stack pages (ring 3, mapped USER)
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
  PIT timer, PS/2 keyboard, physical memory manager, paging, a kernel heap,
  preemptive multitasking, ring-3 user mode + syscalls, a FAT12 filesystem, and
  the shell. Natural next steps: an **ELF loader** so ring-3 programs can be
  read from the FAT12 disk and executed on demand (instead of being baked into
  the kernel), then an ATA/IDE driver so PumpkinOS can also use a hard disk.
- Known simplifications: the FAT12 driver has no long-file-name or timestamp
  support and `rm` only removes files (not directories); dead tasks leak their
  stack/TCB (no reaper); all tasks share one address space; user pages are not
  freed on exit.
- `KERNEL.BIN` loads at 0x10000 and grows toward the floppy DMA buffer at
  0x80000, so it can be up to ~448 KiB (capped at 512 sectors / 256 KiB in the
  Makefile; currently ~43). Plenty of headroom.
- The bootloader gathers the memory map with `INT 15h/E820`, which every PC
  BIOS since ~1994 supports; genuinely ancient machines without it would need
  an `E801`/`AH=88h` fallback added to `do_e820`.
- Compiled with `-march=i386` so it should run on genuine 386/486-era hardware.
- COM1 serial output (38400 8N1) mirrors the console, which makes debugging on
  real hardware or headless QEMU much easier.
