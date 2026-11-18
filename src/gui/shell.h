/**
 * @file shell.h
 * @brief Win32 plumbing for rxx_gui: control creation and lookup, fonts,
 * tab-page registry, common dialogs, and the message pump.
 *
 * main.c owns the control-ID enum and all UI behavior; this module owns the
 * generic boilerplate those interactions sit on. Controls are created
 * through shell_add() and afterwards addressed by their integer ID alone.
 */
#ifndef SHELL_H
#define SHELL_H

#include <windows.h>

/**
 * @brief Create a child control and remember it under @p id.
 *
 * @param page tab page to file the control under for shell_show_page()
 *             (negative: not on any page, always visible)
 * @param id   control/command ID, also used as the child window ID
 */
HWND shell_add(
    HWND parent,
    int page,
    int id,
    const char *cls,
    const char *text,
    DWORD style,
    int x,
    int y,
    int w,
    int h);

/** @brief Handle for a previously created control ID (NULL if unknown). */
HWND shell_hwnd(int id);

/** @brief Set / read back a control's text (ANSI). */
void shell_text_set(int id, const char *s);
void shell_text_get(int id, char *buf, size_t n);

/** @brief Combobox: append one string / read the current selection (-1: none). */
void shell_combo_add(int id, const char *s);
int shell_combo_sel(int id);

/** @brief Checkbox/radio state as 0/1. */
int shell_checked(int id);

/** @brief Show/hide or enable/disable a list of control IDs. */
void shell_show_ids(int on, int n, ...);
void shell_enable_ids(int on, int n, ...);

/** @brief Show only tab page @p page's controls and hide the other page's. */
void shell_show_page(int page);

/** @brief Common file dialog writing the picked path into control @p id. */
void shell_browse(HWND owner, int id, int save);

/**
 * @brief Create the UI font, apply it to every child of @p parent.
 * @return the monospace font for log/preview panes (not auto-applied)
 */
HFONT shell_apply_fonts(HWND parent);

/**
 * @brief Register the window class, create the main window, run the loop.
 *
 * Fixed-size window sized to a @p client_w x @p client_h client area.
 *
 * @return the message-loop exit code (-1 if window creation failed)
 */
int shell_run(
    HINSTANCE inst,
    const char *cls,
    const char *title,
    int client_w,
    int client_h,
    WNDPROC proc,
    int show);

#endif /* SHELL_H */
