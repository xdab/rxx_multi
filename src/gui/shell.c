/**
 * @file shell.c
 * @brief Implementation of the rxx_gui Win32 plumbing (see shell.h).
 */
#include "shell.h"

#include <commctrl.h>
#include <commdlg.h>
#include <stdarg.h>
#include <string.h>
#include <windowsx.h>

/** @brief Highest control ID addressable by the shell. */
#define SHELL_MAX_ID 256
/** @brief Controls per tab page. */
#define SHELL_PAGE_MAX 64
/** @brief Number of tab pages. */
#define SHELL_PAGES 2

static HWND s_ctl[SHELL_MAX_ID];
static HWND s_page[SHELL_PAGES][SHELL_PAGE_MAX];
static int s_page_n[SHELL_PAGES];
static HFONT s_font, s_mono;

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
    int h)
{
    if (id < 0 || id >= SHELL_MAX_ID)
        return NULL;
    HWND c = CreateWindowA(
        cls,
        text,
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        w,
        h,
        parent,
        (HMENU)(INT_PTR)id,
        (HINSTANCE)GetWindowLongPtrA(parent, GWLP_HINSTANCE),
        NULL);
    s_ctl[id] = c;
    if (page >= 0 && page < SHELL_PAGES && s_page_n[page] < SHELL_PAGE_MAX)
        s_page[page][s_page_n[page]++] = c;
    return c;
}

HWND shell_hwnd(int id) { return id >= 0 && id < SHELL_MAX_ID ? s_ctl[id] : NULL; }

void shell_text_set(int id, const char *s) { SetWindowTextA(shell_hwnd(id), s); }

void shell_text_get(int id, char *buf, size_t n) { GetWindowTextA(shell_hwnd(id), buf, (int)n); }

void shell_combo_add(int id, const char *s) { ComboBox_AddString(shell_hwnd(id), s); }

int shell_combo_sel(int id) { return ComboBox_GetCurSel(shell_hwnd(id)); }

int shell_checked(int id) { return Button_GetCheck(shell_hwnd(id)) == BST_CHECKED; }

void shell_show_ids(int on, int n, ...)
{
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        ShowWindow(shell_hwnd(va_arg(ap, int)), on ? SW_SHOW : SW_HIDE);
    va_end(ap);
}

void shell_enable_ids(int on, int n, ...)
{
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++)
        EnableWindow(shell_hwnd(va_arg(ap, int)), on);
    va_end(ap);
}

void shell_show_page(int page)
{
    for (int p = 0; p < SHELL_PAGES; p++)
        for (int i = 0; i < s_page_n[p]; i++)
            ShowWindow(s_page[p][i], p == page ? SW_SHOW : SW_HIDE);
}

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

HFONT shell_apply_fonts(HWND parent)
{
    s_font = make_font("Segoe UI", DEFAULT_PITCH | FF_DONTCARE);
    s_mono = make_font("Consolas", FIXED_PITCH | FF_MODERN);
    EnumChildWindows(parent, set_font_cb, (LPARAM)s_font);
    return s_mono;
}

int shell_run(
    HINSTANCE inst,
    const char *cls,
    const char *title,
    int client_w,
    int client_h,
    WNDPROC proc,
    int show)
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

    RECT r = {0, 0, client_w, client_h};
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    HWND hwnd = CreateWindowA(
        cls,
        title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
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
        if (!IsDialogMessage(hwnd, &msg)) /* Tab navigation between controls */
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return (int)msg.wParam;
}
