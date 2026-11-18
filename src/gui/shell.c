/**
 * @file shell.c
 * @brief Implementation of the rxx_gui Win32 plumbing (see shell.h).
 */
#include "shell.h"

#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>
#include <windowsx.h>

/** @brief Highest control ID addressable by the shell. */
#define SHELL_MAX_ID 256

/* Flow-layout metrics (client-area pixels) */
#define ROW_GAP 8        /* vertical space between rows */
#define COL_GAP 10       /* horizontal space between items in a row */
#define PAD_X 16         /* inset of row content inside a group box */
#define GROUP_TITLE 26   /* group box strip reserved for the caption */
#define GROUP_PAD_BOT 12 /* group box space below the last row */
#define SECTION_GAP 12   /* vertical space between sections */
#define COLUMN_TOP 44    /* y where the first section of a column starts */

/** @brief Created-control registry: geometry + page, indexed by ID. */
static struct
{
    HWND hwnd;
    const shell_ctl_t *def;
} s_ctl[SHELL_MAX_ID];

void shell_create(HWND parent, const shell_ctl_t *ctls, int n)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE);
    for (int i = 0; i < n; i++)
    {
        const shell_ctl_t *c = &ctls[i];
        if (c->id < 0 || c->id >= SHELL_MAX_ID)
            continue;
        HWND h = CreateWindowA(
            c->cls,
            c->text,
            WS_CHILD | WS_VISIBLE | c->style,
            0,
            0,
            c->w,
            c->h,
            parent,
            (HMENU)(INT_PTR)c->id,
            inst,
            NULL);
        s_ctl[c->id].hwnd = h;
        s_ctl[c->id].def = c;
    }
}

HWND shell_hwnd(int id) { return id >= 0 && id < SHELL_MAX_ID ? s_ctl[id].hwnd : NULL; }

void shell_text_set(int id, const char *s) { SetWindowTextA(shell_hwnd(id), s); }

void shell_text_get(int id, char *buf, size_t n) { GetWindowTextA(shell_hwnd(id), buf, (int)n); }

void shell_combo_add(int id, const char *s) { ComboBox_AddString(shell_hwnd(id), s); }

int shell_combo_sel(int id) { return ComboBox_GetCurSel(shell_hwnd(id)); }

int shell_checked(int id) { return Button_GetCheck(shell_hwnd(id)) == BST_CHECKED; }

void shell_browse(HWND owner, int id, int save)
{
    char file[MAX_PATH];
    shell_text_get(id, file, sizeof(file));
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrFilter = "All files\0*.*\0";
    ofn.Flags = OFN_HIDEREADONLY | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameA(&ofn) : GetOpenFileNameA(&ofn))
        shell_text_set(id, file);
}

/** @brief True when a control belongs to the active page (or is chrome). */
static int on_page(const shell_ctl_t *def, int page) { return def->page < 0 || def->page == page; }

/** @brief Layout geometry of one item: MoveWindow size + row height. */
static void item_geom(const shell_ctl_t *def, int w, int *mw, int *mh, int *rh)
{
    *mw = w > 0 ? w : def->w;
    *mh = strcmp(def->cls, "COMBOBOX") == 0 ? def->h + 180 : def->h;
    *rh = def->h; /* combos occupy only their field height in the flow */
}

int shell_layout(HWND parent, int page, const shell_group_t *groups, int n, int (*visible)(int id))
{
    (void)parent;
    /* hide everything that is not on the active tab page */
    for (int i = 0; i < SHELL_MAX_ID; i++)
        if (s_ctl[i].def && s_ctl[i].def->page >= 0 && s_ctl[i].def->page != page)
            ShowWindow(s_ctl[i].hwnd, SW_HIDE);

    int cy[2] = {COLUMN_TOP, COLUMN_TOP};
    int bottom = 0;

    for (int g = 0; g < n; g++)
    {
        const shell_group_t *grp = &groups[g];
        if (grp->group_id && (!visible(grp->group_id) || !on_page(s_ctl[grp->group_id].def, page)))
        {
            ShowWindow(shell_hwnd(grp->group_id), SW_HIDE);
            continue;
        }
        int y = cy[grp->col] + (grp->group_id ? GROUP_TITLE : 0);
        int y0 = cy[grp->col];

        for (const shell_row_t *row = grp->rows; row->items; row++)
        {
            /* measure pass: row height, stretch budget */
            int nvis = 0, nstretch = 0, fixed = 0, rowh = 0;
            for (const shell_item_t *it = row->items; it->id; it++)
            {
                if (!on_page(s_ctl[it->id].def, page) || !visible(it->id))
                    continue;
                const shell_ctl_t *def = s_ctl[it->id].def;
                int mw, mh, rh;
                item_geom(def, it->w, &mw, &mh, &rh);
                fixed += mw;
                if (it->w < 0)
                    nstretch++;
                nvis++;
                rowh = rowh > rh ? rowh : rh;
            }
            if (!nvis)
                continue;
            int extra = nstretch ? (grp->w - 2 * PAD_X - fixed) / nstretch : 0;

            /* place pass; statics ride centered on taller neighbors */
            int x = grp->x + PAD_X;
            for (const shell_item_t *it = row->items; it->id; it++)
            {
                int id = it->id;
                if (!on_page(s_ctl[id].def, page) || !visible(id))
                {
                    ShowWindow(shell_hwnd(id), SW_HIDE);
                    continue;
                }
                const shell_ctl_t *def = s_ctl[id].def;
                int mw, mh, rh;
                item_geom(def, it->w, &mw, &mh, &rh);
                if (it->w < 0)
                    mw += extra;
                int iy = y;
                if (rowh > rh && strcmp(def->cls, "STATIC") == 0)
                    iy += (rowh - rh) / 2;
                MoveWindow(shell_hwnd(id), x, iy, mw, mh, TRUE);
                ShowWindow(shell_hwnd(id), SW_SHOW);
                x += mw + COL_GAP;
            }
            y += rowh + ROW_GAP;
        }

        if (grp->group_id)
        {
            MoveWindow(
                shell_hwnd(grp->group_id),
                grp->x,
                y0,
                grp->w,
                y - y0 - ROW_GAP + GROUP_PAD_BOT,
                TRUE);
            ShowWindow(shell_hwnd(grp->group_id), SW_SHOW);
        }
        cy[grp->col] = y + SECTION_GAP;
        if (y > bottom)
            bottom = y;
    }
    return bottom - ROW_GAP;
}

/** @brief EnumChildWindows callback applying the UI font. */
static BOOL CALLBACK set_font_cb(HWND child, LPARAM font)
{
    SendMessage(child, WM_SETFONT, (WPARAM)font, FALSE);
    return TRUE;
}

/** @brief Create a Segoe UI / Consolas font at UI size. */
static HFONT make_font(const char *face, BYTE pitch_family)
{
    return CreateFontA(
        -12,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        pitch_family,
        face);
}

static HFONT s_mono;

HFONT shell_apply_fonts(HWND parent)
{
    HFONT ui = make_font("Segoe UI", DEFAULT_PITCH | FF_DONTCARE);
    s_mono = make_font("Consolas", FIXED_PITCH | FF_MODERN);
    EnumChildWindows(parent, set_font_cb, (LPARAM)ui);
    return s_mono;
}

int shell_run(
    HINSTANCE inst, const char *cls, const char *title, int client_w, WNDPROC proc, int show)
{
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_TAB_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassA(&wc);

    RECT r = {0, 0, client_w, 400};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowA(
        cls,
        title,
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        r.right - r.left,
        r.bottom - r.top,
        NULL,
        NULL,
        inst,
        NULL);
    if (!hwnd)
        return -1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        /* Tab navigation: IsDialogMessage must only see keyboard messages —
         * fed everything, it synthesizes a WM_CLOSE at startup */
        BOOL dialog =
            msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST && IsDialogMessage(hwnd, &msg);
        if (!dialog)
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return (int)msg.wParam;
}
