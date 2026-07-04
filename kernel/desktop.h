/* ===========================================================================
 * PumpkinOS - a tiny graphical desktop
 * ---------------------------------------------------------------------------
 * Switches the display into 320x200 graphics, draws a desktop with a taskbar,
 * a live clock and a couple of draggable windows, and lets you push them around
 * with the mouse. Press Esc to return to the text-mode shell.
 * ========================================================================= */
#ifndef PUMPKIN_DESKTOP_H
#define PUMPKIN_DESKTOP_H

/* Run the desktop. Returns to the caller (the shell) when the user exits. */
void desktop_run(void);

#endif /* PUMPKIN_DESKTOP_H */
