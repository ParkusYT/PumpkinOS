/* ===========================================================================
 * PumpkinOS - a tiny graphical desktop
 * ---------------------------------------------------------------------------
 * Switches the display into graphics mode and draws a desktop: the files in
 * the root directory as clickable icons, draggable windows, and a taskbar with
 * a Start menu (Enter shell / Reboot / Power Off) and a live clock. Press Esc
 * (or Start -> Enter shell) to return to the text-mode shell.
 * ========================================================================= */
#ifndef PUMPKIN_DESKTOP_H
#define PUMPKIN_DESKTOP_H

/* Run the desktop. Returns to the caller (the shell) when the user exits. */
void desktop_run(void);

#endif /* PUMPKIN_DESKTOP_H */
