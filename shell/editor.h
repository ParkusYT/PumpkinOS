/* ===========================================================================
 * PumpkinOS - full-screen text editor
 * ========================================================================= */
#ifndef PUMPKIN_EDITOR_H
#define PUMPKIN_EDITOR_H

/* Edit 'name' (a file in the current directory; created on save if new).
 * Takes over the text screen until the user quits, then returns to the shell. */
void editor_run(const char *name);

#endif /* PUMPKIN_EDITOR_H */
