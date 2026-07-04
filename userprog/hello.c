/* ===========================================================================
 * PumpkinOS - example ring-3 program (built to a real ELF, run off the disk)
 * ---------------------------------------------------------------------------
 * This is NOT part of the kernel. It is compiled and linked on its own into a
 * standalone ELF executable (see userprog/user.ld), copied onto the FAT12
 * floppy as HELLO.ELF, and launched by the kernel's ELF loader with `run
 * HELLO.ELF`. It runs at CPL 3 and can only reach the kernel through int 0x80.
 * ========================================================================= */

#define SYS_EXIT   0
#define SYS_WRITE  2
#define SYS_GETCPL 3

/* int 0x80 returns a value in EAX, so EAX must be an output (or at least
 * clobbered) - otherwise GCC assumes the syscall number is still in EAX and
 * skips reloading it before the next call. */
static int sys_write(const char *s, unsigned len) {
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret) : "a"(SYS_WRITE), "b"(s), "c"(len) : "memory");
    return ret;
}
static int sys_getcpl(void) {
    int cpl;
    __asm__ volatile("int $0x80" : "=a"(cpl) : "a"(SYS_GETCPL));
    return cpl;
}
static void sys_exit(void) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(SYS_EXIT));
}

static unsigned slen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static const char banner[] =
    "Hello from HELLO.ELF - a real ELF binary running in ring 3!\n"
    "Loaded off the FAT12 floppy and launched through int 0x80.\n"
    "My privilege level is CPL ";

void _start(void) {
    sys_write(banner, slen(banner));

    char c = (char)('0' + (sys_getcpl() & 3));
    sys_write(&c, 1);
    sys_write(".\n", 2);

    sys_exit();
    for (;;)            /* unreachable: SYS_EXIT does not return */
        ;
}
