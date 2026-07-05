/* ===========================================================================
 * PumpkinOS - PumpkinShell (PKSH)
 * ---------------------------------------------------------------------------
 * A tiny read-eval-print loop. It reads a line from the keyboard (with echo
 * and backspace editing), splits off the first word as the command, and runs
 * the matching built-in. Everything after the command is passed along as the
 * argument string.
 * ========================================================================= */
#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "timer.h"
#include "pmm.h"
#include "fat12.h"
#include "rtc.h"
#include "ata.h"
#include "acpi.h"
#include "elf.h"
#include "desktop.h"
#include "rtl8139.h"
#include "net.h"
#include "dhcp.h"
#include "dns.h"
#include "string.h"
#include "io.h"

#define LINE_MAX    128

/* ---- the pumpkin banner (shared with the boot screen) --------------------- */
void shell_banner(void) {
    console_set_color(VGA_YELLOW, VGA_BLACK);
    console_write("\n");
    console_write("   ____                       _    _         ___  ____  \n");
    console_write("  |  _ \\ _   _ _ __ ___  _ __ | | _(_)_ __   / _ \\/ ___| \n");
    console_write("  | |_) | | | | '_ ` _ \\| '_ \\| |/ / | '_ \\ | | | \\___ \\ \n");
    console_write("  |  __/| |_| | | | | | | |_) |   <| | | | || |_| |___) |\n");
    console_write("  |_|    \\__,_|_| |_| |_| .__/|_|\\_\\_|_| |_| \\___/|____/ \n");
    console_write("                        |_|                              \n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* ---- built-in commands ---------------------------------------------------- */
static void cmd_help(void) {
    console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    console_write("PumpkinShell (PKSH) commands:\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  General : help  clear  echo <text>  banner  about\n");
    console_write("  Files   : ls  cd <d>  pwd  cat <f>  write <f> <t>  touch <f>\n");
    console_write("            mkdir <d>  rm [-r] <f>  rmdir <d>\n");
    console_write("  System  : disks [read N]  meminfo  date  uptime  sleep <s>\n");
    console_write("  Network : net  dhcp  dns <host>\n");
    console_write("  Programs: run <file.elf>  desktop\n");
    console_write("  Power   : reboot  poweroff  halt\n");
}

static void cmd_about(void) {
    shell_banner();
    console_write("\n");
    console_write("  PumpkinOS - a 32-bit, BIOS-only hobby OS.\n");
    console_write("  Protected mode, VGA + serial console, PS/2 keyboard,\n");
    console_write("  and this very shell: PumpkinShell (PKSH).\n\n");
}

/* Print a value as at least two digits (zero-padded), for HH:MM:SS. */
static void print_2d(uint32_t v) {
    if (v < 10)
        console_putc('0');
    console_write_dec(v);
}

static void cmd_uptime(void) {
    uint32_t t  = timer_ticks();
    uint32_t hz = timer_hz();
    if (hz == 0) {
        console_write("uptime: timer is not running\n");
        return;
    }

    uint32_t total = t / hz;                 /* whole seconds since boot */
    uint32_t days  = total / 86400;
    uint32_t hours = (total / 3600) % 24;
    uint32_t mins  = (total / 60) % 60;
    uint32_t secs  = total % 60;

    console_write("up ");
    if (days) {
        console_write_dec(days);
        console_write("d ");
    }
    print_2d(hours);
    console_putc(':');
    print_2d(mins);
    console_putc(':');
    print_2d(secs);
    console_write("   (");
    console_write_dec(t);
    console_write(" ticks @ ");
    console_write_dec(hz);
    console_write(" Hz)\n");
}

/* Parse a non-negative decimal integer; stops at the first non-digit. */
static uint32_t parse_uint(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

static void cmd_sleep(const char *args) {
    uint32_t secs = parse_uint(args);
    if (secs == 0) {
        console_write("usage: sleep <seconds>\n");
        return;
    }
    if (secs > 3600)
        secs = 3600;                         /* keep the shell responsive */

    console_write("sleeping for ");
    console_write_dec(secs);
    console_write("s ...\n");
    timer_sleep(secs * 1000);
    console_write("awake!\n");
}

static void cmd_meminfo(void) {
    pmm_report();
}

static void cmd_ls(void) {
    fs_list();
}

static void cmd_cat(const char *args) {
    if (args[0] == '\0') {
        console_write("usage: cat <filename>\n");
        return;
    }
    if (fs_cat(args) != 0) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_write("cat: file not found: ");
        console_write(args);
        console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

static void cmd_cd(const char *args) {
    fs_cd(args[0] ? args : "/");
}

static void print2(uint32_t v) {           /* zero-padded 2-digit */
    if (v < 10) console_putc('0');
    console_write_dec(v);
}

static void cmd_date(void) {
    struct rtc_time t;
    rtc_read(&t);
    console_write_dec(t.year);
    console_putc('-'); print2(t.month);
    console_putc('-'); print2(t.day);
    console_putc(' '); print2(t.hour);
    console_putc(':'); print2(t.minute);
    console_putc(':'); print2(t.second);
    console_putc('\n');
}

/* Pad a string out to 'width' columns with trailing spaces. */
static void print_padded(const char *s, int width) {
    int n = 0;
    while (s[n]) { console_putc(s[n]); n++; }
    while (n++ < width) console_putc(' ');
}

/* List the IDE/ATA hard disks the driver found at boot, with their channel
 * position, capacity and model. Optionally read+dump a sector: `disks read N`
 * reads LBA N off disk 0 and shows the first 16 bytes as a sanity check. */
static void cmd_disks(const char *args) {
    int n = ata_drive_count();
    if (n == 0) {
        console_write("no IDE/ATA disks detected\n");
        return;
    }

    if (strncmp(args, "read", 4) == 0) {
        const char *p = args + 4;
        while (*p == ' ' || *p == '\t') p++;
        uint32_t lba = parse_uint(p);
        static uint8_t sec[512];
        if (ata_read_sectors(0, lba, 1, sec) != 0) {
            console_write("read failed (out of range or controller error)\n");
            return;
        }
        console_write("disk0 LBA ");
        console_write_dec(lba);
        console_write(" first 16 bytes:");
        for (int i = 0; i < 16; i++) {
            console_putc(' ');
            console_write_hex(sec[i]);
        }
        console_putc('\n');
        return;
    }

    console_write("  #  pos          size        model\n");
    for (int i = 0; i < n; i++) {
        const struct ata_drive *d = ata_get_drive(i);
        console_write("  ");
        console_write_dec((uint32_t)i);
        console_write("  ");
        console_write(d->channel ? "secondary/" : "primary/");
        print_padded(d->slave ? "slave" : "master", 6);
        console_write("  ");
        console_write_dec(d->sectors / 2048);   /* 2048 sectors = 1 MiB */
        console_write(" MiB");
        console_write("  (");
        console_write_dec(d->sectors);
        console_write(" sec)  ");
        console_write(d->model);
        console_putc('\n');
    }
}

static void cmd_mkdir(const char *args) {
    if (args[0] == '\0') { console_write("usage: mkdir <name>\n"); return; }
    fs_mkdir(args);
}

static void cmd_touch(const char *args) {
    if (args[0] == '\0') { console_write("usage: touch <name>\n"); return; }
    fs_create(args, "", 0);
}

static void cmd_rm(const char *args) {
    int recursive = 0;
    if (args[0] == '-' && args[1] == 'r' && (args[2] == ' ' || args[2] == '\0')) {
        recursive = 1;
        args += 2;
        while (*args == ' ') args++;
    }
    if (args[0] == '\0') { console_write("usage: rm [-r] <file>\n"); return; }

    int rc = recursive ? fs_rmrf(args) : fs_remove(args);
    if (rc != 0) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_write("rm: not found: ");
        console_write(args);
        console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

static void cmd_rmdir(const char *args) {
    if (args[0] == '\0') { console_write("usage: rmdir <dir>\n"); return; }
    if (fs_rmdir(args) != 0) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_write("rmdir: not found: ");
        console_write(args);
        console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

/* write <file> <text> - create/overwrite a file with the given text. */
static void cmd_write(char *args) {
    if (args[0] == '\0') { console_write("usage: write <file> <text>\n"); return; }
    char *content = args;
    while (*content && *content != ' ')
        content++;                        /* skip the filename */
    if (*content) {
        *content = '\0';                  /* terminate the name */
        content++;
    }

    static char buf[LINE_MAX + 2];
    uint32_t n = 0;
    for (char *p = content; *p && n < sizeof(buf) - 1; p++)
        buf[n++] = *p;
    buf[n++] = '\n';                      /* end the file with a newline */
    fs_create(args, buf, n);
}

/* ---- networking ----------------------------------------------------------- */
static void print_ip(uint32_t ip) {
    console_write_dec((ip >> 24) & 0xFF); console_putc('.');
    console_write_dec((ip >> 16) & 0xFF); console_putc('.');
    console_write_dec((ip >> 8) & 0xFF);  console_putc('.');
    console_write_dec(ip & 0xFF);
}

static void print_mac(void) {
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        console_putc(hex[net_mac[i] >> 4]);
        console_putc(hex[net_mac[i] & 0xF]);
        if (i < 5) console_putc(':');
    }
}

static void cmd_net(void) {
    if (!rtl8139_present()) {
        console_write("no network card detected\n");
        return;
    }
    console_write("  MAC     : "); print_mac(); console_putc('\n');

    uint8_t msr = rtl8139_msr();
    console_write("  link    : ");
    console_write((msr & 0x04) ? "DOWN" : "up");
    console_write((msr & 0x08) ? "  10Mbps" : "  100Mbps");
    console_write("   (MSR="); console_write_hex(msr); console_write(")\n");
    console_write("  TX / RX : ");
    console_write_dec(rtl8139_tx_count()); console_write(" / ");
    console_write_dec(rtl8139_rx_count()); console_write(" frames");
    if (rtl8139_tx_err()) {
        console_write("  (");
        console_write_dec(rtl8139_tx_err());
        console_write(" TX errors)");
    }
    console_putc('\n');

    console_write("  status  : ");
    if (!net_up) { console_write("no IP  (run 'dhcp')\n"); return; }
    console_write("up\n");
    console_write("  IP      : "); print_ip(net_ip);      console_putc('\n');
    console_write("  netmask : "); print_ip(net_mask);    console_putc('\n');
    console_write("  gateway : "); print_ip(net_gateway); console_putc('\n');
    console_write("  DNS     : "); print_ip(net_dns);     console_putc('\n');
}

static void cmd_dhcp(void) {
    if (!rtl8139_present()) {
        console_write("no network card detected\n");
        return;
    }
    console_write("Requesting a DHCP lease...\n");
    if (dhcp_configure()) {
        console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        console_write("lease acquired\n");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        console_write("  IP      : "); print_ip(net_ip);      console_putc('\n');
        console_write("  gateway : "); print_ip(net_gateway); console_putc('\n');
        console_write("  DNS     : "); print_ip(net_dns);     console_putc('\n');
    } else {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_write("DHCP failed (no response)\n");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

static void cmd_dns(const char *args) {
    if (args[0] == '\0') { console_write("usage: dns <hostname>\n"); return; }
    if (!net_up) { console_write("network is down; run 'dhcp' first\n"); return; }

    console_write("Resolving ");
    console_write(args);
    console_write(" ...\n");

    uint32_t ip;
    if (dns_resolve(args, &ip)) {
        console_write("  ");
        console_write(args);
        console_write(" -> ");
        print_ip(ip);
        console_putc('\n');
    } else {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_write("could not resolve\n");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

/* run <file.elf> - load an ELF program off the disk and run it in ring 3. */
static void cmd_run(const char *args) {
    if (args[0] == '\0') { console_write("usage: run <program.elf>\n"); return; }
    if (elf_exec(args) != 0) {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_write("run: not a runnable ELF program: ");
        console_write(args);
        console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

static void cmd_poweroff(void) {
    console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    console_write("Powering off via ACPI...\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    acpi_poweroff();                    /* returns only if it failed */
    console_set_color(VGA_YELLOW, VGA_BLACK);
    console_write("ACPI power-off unavailable; halting instead.\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (;;)
        __asm__ volatile("cli; hlt");
}

static void cmd_reboot(void) {
    console_write("Rebooting...\n");
    /* Pulse the CPU reset line via the 8042 keyboard controller. */
    uint8_t good = 0x02;
    while (good & 0x02)
        good = inb(0x64);
    outb(0x64, 0xFE);
    /* If that somehow didn't take, fall through to a halt. */
    for (;;)
        __asm__ volatile("cli; hlt");
}

static void cmd_halt(void) {
    console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    console_write("System halted. It is now safe to turn off your computer.\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (;;)
        __asm__ volatile("cli; hlt");
}

/* ---- command dispatch ----------------------------------------------------- */

/* Skip leading spaces/tabs. */
static char *skip_spaces(char *s) {
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

static void shell_execute(char *line) {
    char *cmd = skip_spaces(line);

    /* Split the command word from its argument string. */
    char *args = cmd;
    while (*args && *args != ' ' && *args != '\t')
        args++;
    if (*args) {
        *args = '\0';         /* terminate the command word */
        args = skip_spaces(args + 1);
    }

    if (cmd[0] == '\0')
        return;                                  /* empty line */
    else if (strcmp(cmd, "help") == 0)
        cmd_help();
    else if (strcmp(cmd, "clear") == 0 || strcmp(cmd, "cls") == 0)
        console_clear();
    else if (strcmp(cmd, "echo") == 0) {
        console_write(args);
        console_putc('\n');
    }
    else if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "dir") == 0)
        cmd_ls();
    else if (strcmp(cmd, "cat") == 0 || strcmp(cmd, "type") == 0)
        cmd_cat(args);
    else if (strcmp(cmd, "cd") == 0)
        cmd_cd(args);
    else if (strcmp(cmd, "pwd") == 0)
        fs_pwd();
    else if (strcmp(cmd, "date") == 0 || strcmp(cmd, "time") == 0)
        cmd_date();
    else if (strcmp(cmd, "mkdir") == 0)
        cmd_mkdir(args);
    else if (strcmp(cmd, "write") == 0)
        cmd_write(args);
    else if (strcmp(cmd, "touch") == 0)
        cmd_touch(args);
    else if (strcmp(cmd, "rm") == 0 || strcmp(cmd, "del") == 0)
        cmd_rm(args);
    else if (strcmp(cmd, "rmdir") == 0)
        cmd_rmdir(args);
    else if (strcmp(cmd, "disks") == 0 || strcmp(cmd, "hdd") == 0)
        cmd_disks(args);
    else if (strcmp(cmd, "banner") == 0) {
        shell_banner();
        console_putc('\n');
    }
    else if (strcmp(cmd, "about") == 0)
        cmd_about();
    else if (strcmp(cmd, "uptime") == 0)
        cmd_uptime();
    else if (strcmp(cmd, "sleep") == 0)
        cmd_sleep(args);
    else if (strcmp(cmd, "meminfo") == 0)
        cmd_meminfo();
    else if (strcmp(cmd, "net") == 0 || strcmp(cmd, "ipconfig") == 0)
        cmd_net();
    else if (strcmp(cmd, "dhcp") == 0)
        cmd_dhcp();
    else if (strcmp(cmd, "dns") == 0 || strcmp(cmd, "nslookup") == 0)
        cmd_dns(args);
    else if (strcmp(cmd, "run") == 0 || strcmp(cmd, "exec") == 0)
        cmd_run(args);
    else if (strcmp(cmd, "desktop") == 0 || strcmp(cmd, "gui") == 0)
        desktop_run();
    else if (strcmp(cmd, "poweroff") == 0 || strcmp(cmd, "shutdown") == 0)
        cmd_poweroff();
    else if (strcmp(cmd, "reboot") == 0)
        cmd_reboot();
    else if (strcmp(cmd, "halt") == 0)
        cmd_halt();
    else {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_write("pksh: command not found: ");
        console_write(cmd);
        console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

/* Print the prompt, including the current working directory. */
static void print_prompt(void) {
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("pksh:");
    console_write(fs_cwd());
    console_write("> ");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* ---- command history (recalled with the up/down arrows) ------------------- */
#define HIST_SIZE 16
static char hist[HIST_SIZE][LINE_MAX];
static int  hist_n;        /* number of stored commands (<= HIST_SIZE) */
static int  hist_next;     /* ring write position */

static void hist_add(const char *cmd) {
    if (cmd[0] == '\0') return;
    size_t n = 0;
    while (cmd[n] && n < LINE_MAX - 1) { hist[hist_next][n] = cmd[n]; n++; }
    hist[hist_next][n] = '\0';
    hist_next = (hist_next + 1) % HIST_SIZE;
    if (hist_n < HIST_SIZE) hist_n++;
}

/* view 0 = most recent ... hist_n-1 = oldest. */
static const char *hist_get(int view) {
    int idx = (hist_next - 1 - view + HIST_SIZE * 2) % HIST_SIZE;
    return hist[idx];
}

/* Erase the on-screen input line and replace it with 'text'. */
static void replace_line(char *line, size_t *len, const char *text) {
    while (*len > 0) { console_putc('\b'); (*len)--; }
    size_t n = 0;
    for (const char *p = text; *p && n < LINE_MAX - 1; p++) {
        line[n++] = *p;
        console_putc(*p);
    }
    line[n] = '\0';
    *len = n;
}

/* ---- the read-eval-print loop --------------------------------------------- */
void shell_run(void) {
    static char line[LINE_MAX];
    size_t len = 0;
    int view = -1;                     /* -1 = editing a fresh line */

    print_prompt();

    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n') {
            console_putc('\n');
            line[len] = '\0';
            hist_add(line);
            shell_execute(line);
            len = 0;
            view = -1;
            print_prompt();
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                console_putc('\b');   /* console erases the glyph for us */
            }
        } else if ((unsigned char)c == KEY_UP) {
            if (view + 1 < hist_n) {
                view++;
                replace_line(line, &len, hist_get(view));
            }
        } else if ((unsigned char)c == KEY_DOWN) {
            if (view > 0) {
                view--;
                replace_line(line, &len, hist_get(view));
            } else if (view == 0) {
                view = -1;
                replace_line(line, &len, "");
            }
        } else if (c == '\t') {
            /* complete the current word against the current directory */
            size_t ws = len;
            while (ws > 0 && line[ws - 1] != ' ') ws--;
            char prefix[16];
            size_t pl = 0;
            for (size_t i = ws; i < len && pl < sizeof(prefix) - 1; i++)
                prefix[pl++] = line[i];
            prefix[pl] = '\0';

            char comp[16];
            if (fs_complete(prefix, comp, sizeof(comp)) >= 1 && strlen(comp) > pl) {
                while (len > ws) { console_putc('\b'); len--; }   /* erase word */
                for (const char *p = comp; *p && len < LINE_MAX - 1; p++) {
                    line[len++] = *p;
                    console_putc(*p);
                }
            }
        } else if ((unsigned char)c >= ' ' && (unsigned char)c < 127) {
            if (len < LINE_MAX - 1) {
                line[len++] = c;
                console_putc(c);
            }
        }
        /* anything else (Esc, unmapped keys) is ignored */
    }
}
