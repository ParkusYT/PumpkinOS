/* ===========================================================================
 * PumpkinOS - PumpkinShell (PSH)
 * ---------------------------------------------------------------------------
 * A tiny read-eval-print loop. It reads a line from the keyboard (with echo
 * and backspace editing), splits off the first word as the command, and runs
 * the matching built-in. Everything after the command is passed along as the
 * argument string.
 * ========================================================================= */
#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "string.h"
#include "io.h"

#define PROMPT      "psh> "
#define LINE_MAX    128

/* ---- the pumpkin banner (shared with the boot screen) --------------------- */
void shell_banner(void) {
    console_set_color(VGA_YELLOW, VGA_BLACK);
    console_write("\n");
    console_write("   ____                   _    _        ___  ____\n");
    console_write("  |  _ \\ _   _ _ __ ___  | | _(_)_ __  / _ \\/ ___|\n");
    console_write("  | |_) | | | | '_ ` _ \\ | |/ / | '_ \\| | | \\___ \\\n");
    console_write("  |  __/| |_| | | | | | ||   <| | | | | |_| |___) |\n");
    console_write("  |_|    \\__,_|_| |_| |_||_|\\_\\_|_| |_|\\___/|____/\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* ---- built-in commands ---------------------------------------------------- */
static void cmd_help(void) {
    console_write("PumpkinShell (PSH) built-in commands:\n");
    console_write("  help          show this help\n");
    console_write("  clear, cls    clear the screen\n");
    console_write("  echo <text>   print <text>\n");
    console_write("  banner        draw the PumpkinOS banner\n");
    console_write("  about         about PumpkinOS\n");
    console_write("  colors        show the VGA colour palette\n");
    console_write("  reboot        restart the machine\n");
    console_write("  halt          stop the CPU\n");
}

static void cmd_about(void) {
    shell_banner();
    console_write("\n");
    console_write("  PumpkinOS - a 32-bit, BIOS-only hobby OS.\n");
    console_write("  Protected mode, VGA + serial console, PS/2 keyboard,\n");
    console_write("  and this very shell: PumpkinShell (PSH).\n\n");
}

static void cmd_colors(void) {
    for (int i = 0; i < 16; i++) {
        console_set_color((enum vga_color)i, VGA_BLACK);
        console_write(" #");
        console_write_dec((uint32_t)i);
    }
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("\n");
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
    else if (strcmp(cmd, "banner") == 0) {
        shell_banner();
        console_putc('\n');
    }
    else if (strcmp(cmd, "about") == 0)
        cmd_about();
    else if (strcmp(cmd, "colors") == 0 || strcmp(cmd, "color") == 0)
        cmd_colors();
    else if (strcmp(cmd, "reboot") == 0)
        cmd_reboot();
    else if (strcmp(cmd, "halt") == 0)
        cmd_halt();
    else {
        console_set_color(VGA_LIGHT_RED, VGA_BLACK);
        console_write("psh: command not found: ");
        console_write(cmd);
        console_putc('\n');
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

/* ---- the read-eval-print loop --------------------------------------------- */
void shell_run(void) {
    static char line[LINE_MAX];
    size_t len = 0;

    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write(PROMPT);
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n') {
            console_putc('\n');
            line[len] = '\0';
            shell_execute(line);
            len = 0;
            console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            console_write(PROMPT);
            console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                console_putc('\b');   /* console erases the glyph for us */
            }
        } else if (c == '\t') {
            /* treat tab as a single space for simplicity */
            if (len < LINE_MAX - 1) {
                line[len++] = ' ';
                console_putc(' ');
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
