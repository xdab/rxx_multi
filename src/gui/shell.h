/**
 * @file shell.h
 * @brief Win32 plumbing for rxx_gui: table-driven control creation, a flow
 * layout engine, common dialogs, and the message pump.
 *
 * main.c owns the control-ID enum, the declarative control/layout tables and
 * all UI behavior; this module owns the generic boilerplate. Controls are
 * created from a shell_ctl_t table and addressed by their integer ID alone.
 * Positioning is never hand-assigned: shell_layout() flows visible controls
 * through rows and sections, so visibility toggles reflow automatically and
 * overlapping stale geometry is impossible by construction.
 */
#ifndef SHELL_H
#define SHELL_H

#include <windows.h>

/** @brief Control creation table entry. */
typedef struct
{
    int id, page;     /**< page < 0: not on a tab page (always visible) */
    const char *cls;  /**< window class ("EDIT", "BUTTON", WC_TABCONTROL, ...) */
    const char *text; /**< initial text (labels, captions; NULL for edits) */
    DWORD style;      /**< extra styles beyond WS_CHILD | WS_VISIBLE */
    int w, h;         /**< layout geometry; combos get h + dropdown room */
} shell_ctl_t;

/** @brief One control in a flow row; w < 0 stretches to the section end.
 *  A zero id terminates the row. */
typedef struct
{
    int id;
    int w;
} shell_item_t;

/** @brief A row of controls laid out left to right; NULL items terminate
 *  the section's row list. */
typedef struct
{
    const shell_item_t *items;
} shell_row_t;

/**
 * @brief A section of rows flowing in one of two columns.
 *
 * With group_id != 0 a titled group box is drawn around the rows and the
 * whole section vanishes when the group box is not visible().
 */
typedef struct
{
    int group_id;      /**< group box control ID, 0 = borderless section */
    const char *title; /**< group box caption (group_id != 0 only) */
    int col;           /**< 0 or 1: which column this section flows in */
    int x, w;          /**< section geometry within the client area */
    const shell_row_t *rows;
} shell_group_t;

/** @brief Create every control in the table (positions set by layout). */
void shell_create(HWND parent, const shell_ctl_t *ctls, int n);

/**
 * @brief Position and show/hide all controls of @p page.
 *
 * Rows are flowed left-to-right, sections top-to-bottom per column; items
 * whose visible() returns 0 take no space. Non-active-page controls are
 * hidden.
 *
 * @return the content bottom y, for sizing the tab control and window
 */
int shell_layout(HWND parent, int page, const shell_group_t *groups, int n, int (*visible)(int id));

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
 * Fixed-width window sized to a @p client_w wide client area; height is set
 * by the caller's layout pass.
 *
 * @return the message-loop exit code (-1 if window creation failed)
 */
int shell_run(
    HINSTANCE inst, const char *cls, const char *title, int client_w, WNDPROC proc, int show);

#endif /* SHELL_H */
