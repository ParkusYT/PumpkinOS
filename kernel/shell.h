/* ===========================================================================
 * PumpkinOS - PumpkinShell (PSH)
 * ========================================================================= */
#ifndef PUMPKIN_SHELL_H
#define PUMPKIN_SHELL_H

/* Run the interactive shell forever. Never returns. */
void shell_run(void);

/* Draw the PumpkinOS ASCII banner (also used by the boot screen). */
void shell_banner(void);

#endif /* PUMPKIN_SHELL_H */
