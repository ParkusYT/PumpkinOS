/* ===========================================================================
 * PumpkinOS - minimal ACPI support (just enough for soft power-off)
 * ---------------------------------------------------------------------------
 * We do not bring up a full ACPI subsystem. At boot we locate the RSDP, follow
 * it to the FADT, and parse the \_S5 object out of the DSDT's AML - that gives
 * us the PM1 control ports and the SLP_TYP values needed to ask the hardware to
 * turn itself off (the "soft off" ACPI sleep state S5).
 * ========================================================================= */
#ifndef PUMPKIN_ACPI_H
#define PUMPKIN_ACPI_H

/* Scan firmware tables and cache what soft power-off needs. Safe to call once,
 * after paging is on (ACPI tables can live in high memory). */
void acpi_init(void);

/* Non-zero if acpi_init() found everything soft power-off requires. */
int acpi_available(void);

/* Ask the machine to power off via ACPI S5. On success this does not return;
 * returns -1 if ACPI power-off is unavailable. */
int acpi_poweroff(void);

#endif /* PUMPKIN_ACPI_H */
