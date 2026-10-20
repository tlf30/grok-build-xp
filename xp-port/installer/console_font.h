/* Per-exe console face for the XP console host. */
#ifndef XP_CONSOLE_FONT_H
#define XP_CONSOLE_FONT_H

int xp_set_console_face(const WCHAR *exe_path);
int xp_clear_console_face(const WCHAR *exe_path);

#endif
