/**
 * @file main.c
 * @brief rxx_gui — Windows launcher UI for rxx_multi.
 *
 * Tabbed layout: the "Channels" tab holds the working set (channel list,
 * editor, live command-line preview); the "Source" tab holds the input
 * selection and its relevant settings. All controls are declared in tables
 * and positioned by the shell.c flow-layout engine; is_visible() is the
 * single source of truth for what shows, so visibility toggles reflow
 * automatically. All state maps to a backend command line through
 * args_mapper.h; Launch/Stop manage the backend child process.
 */
#include <windows.h>
#include <windowsx.h>

#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#include "args_mapper.h"
#include "shell.h"

/** @brief Child control / command IDs. */
enum
{
    /* chrome */
    IDC_TAB = 100,
    IDC_STATUSLINE,
    IDC_LAUNCH,
    IDC_STOP,

    /* channels page */
    IDC_CH_LIST,
    IDC_PREVIEW_L,
    IDC_PREVIEW,
    IDC_CH_FREQ_L,
    IDC_CH_FREQ,
    IDC_CH_MODE_L,
    IDC_CH_MODE,
    IDC_CH_OUT_L,
    IDC_CH_OUT,
    IDC_CH_TCP_L,
    IDC_CH_TCP_PORT,
    IDC_CH_UDP_HOST_L,
    IDC_CH_UDP_HOST,
    IDC_CH_UDP_PORT_L,
    IDC_CH_UDP_PORT,
    IDC_CH_FILE_L,
    IDC_CH_FILE,
    IDC_CH_FILE_BROWSE,
    IDC_CH_ADD,
    IDC_CH_UPDATE,
    IDC_CH_REMOVE,

    /* source page */
    IDC_GRP_SOURCE,
    IDC_BACKEND,
    IDC_DEVICE_L,
    IDC_DEVICE,
    IDC_DEVICE_HINT,
    IDC_FILE_PATH_L,
    IDC_FILE_PATH,
    IDC_FILE_BROWSE,
    IDC_FILE_RATE_L,
    IDC_FILE_RATE,
    IDC_FILE_CENTER_L,
    IDC_FILE_CENTER,
    IDC_GRP_RATES,
    IDC_RATE_IN_L,
    IDC_RATE_IN,
    IDC_RATE_OUT_L,
    IDC_RATE_OUT,
    IDC_TIMEOUT_L,
    IDC_TIMEOUT,
    IDC_DC,
    IDC_DEEMP,
    IDC_GRP_RX,
    IDC_RTL_GAIN_L,
    IDC_RTL_GAIN,
    IDC_RTL_PPM_L,
    IDC_RTL_PPM,
    IDC_BIASTEE,
    IDC_DIRECT_L,
    IDC_DIRECT,
    IDC_RSP_GRDB_L,
    IDC_RSP_GRDB,
    IDC_RSP_LNA_L,
    IDC_RSP_LNA,
    IDC_RSP_NOTE,
    IDC_GRP_RECORD,
    IDC_RECORD_L,
    IDC_RECORD_PATH,
    IDC_RECORD_BROWSE
};

/** @brief Tab pages. */
enum
{
    PAGE_CHANNELS = 0,
    PAGE_SOURCE = 1
};

/** @brief Source selector entries (the IDC_BACKEND combo). */
enum
{
    GUI_SRC_RTL = 0,
    GUI_SRC_RSP = 1,
    GUI_SRC_FILE = 2
};

/** @brief Main window handle (owner for dialogs, target for notifications). */
static HWND g_main;
/** @brief Channel list master state (the -f/-M/-O lists, positional). */
static gui_channel_t g_channels[GUI_MAX_CHANNELS];
static int g_chan_count;
/** @brief Channel currently loaded in the editor pane. */
static gui_channel_t g_edit;
/** @brief Running child process state. */
static PROCESS_INFORMATION g_pi;
static int g_running;

#define WM_CHILD_EXIT (WM_APP + 1)

/* ------------------------------------------------------------------ */
/* State <-> controls                                                  */
/* ------------------------------------------------------------------ */

/** @brief Set the one-line status text. */
static void set_status(const char *s) { shell_text_set(IDC_STATUSLINE, s); }

/** @brief Read the channel editor into g_edit (all output fields). */
static void editor_to_edit(void)
{
    shell_text_get(IDC_CH_FREQ, g_edit.freq, sizeof(g_edit.freq));
    g_edit.mode = (gui_mode_t)shell_combo_sel(IDC_CH_MODE);
    g_edit.output = (gui_output_type_t)shell_combo_sel(IDC_CH_OUT);
    shell_text_get(IDC_CH_TCP_PORT, g_edit.tcp_port, sizeof(g_edit.tcp_port));
    shell_text_get(IDC_CH_UDP_HOST, g_edit.udp_host, sizeof(g_edit.udp_host));
    shell_text_get(IDC_CH_UDP_PORT, g_edit.udp_port, sizeof(g_edit.udp_port));
    shell_text_get(IDC_CH_FILE, g_edit.file_name, sizeof(g_edit.file_name));
}

/** @brief Fill the channel editor from g_edit. */
static void load_editor(void)
{
    shell_text_set(IDC_CH_FREQ, g_edit.freq);
    ComboBox_SetCurSel(shell_hwnd(IDC_CH_MODE), g_edit.mode);
    ComboBox_SetCurSel(shell_hwnd(IDC_CH_OUT), g_edit.output);
    shell_text_set(IDC_CH_TCP_PORT, g_edit.tcp_port);
    shell_text_set(IDC_CH_UDP_HOST, g_edit.udp_host);
    shell_text_set(IDC_CH_UDP_PORT, g_edit.udp_port);
    shell_text_set(IDC_CH_FILE, g_edit.file_name);
}

/** @brief Copy every control's value into a launch-options struct. */
static void collect_options(gui_launch_options_t *o)
{
    memset(o, 0, sizeof(*o));
    int src = shell_combo_sel(IDC_BACKEND);
    if (src < 0)
        src = GUI_SRC_RTL;
    o->backend = src == GUI_SRC_RSP ? GUI_BACKEND_RSP : GUI_BACKEND_RTL;
    o->use_file_input = src == GUI_SRC_FILE;
    shell_text_get(IDC_DEVICE, o->device, sizeof(o->device));
    shell_text_get(IDC_RATE_IN, o->rate_in, sizeof(o->rate_in));
    shell_text_get(IDC_RATE_OUT, o->rate_out, sizeof(o->rate_out));
    shell_text_get(IDC_TIMEOUT, o->timeout, sizeof(o->timeout));
    o->dc_block = shell_checked(IDC_DC);
    o->deemph = shell_checked(IDC_DEEMP);
    shell_text_get(IDC_RTL_GAIN, o->rtl_gain, sizeof(o->rtl_gain));
    shell_text_get(IDC_RTL_PPM, o->rtl_ppm, sizeof(o->rtl_ppm));
    o->rtl_biastee = shell_checked(IDC_BIASTEE);
    o->rtl_direct = shell_combo_sel(IDC_DIRECT);
    shell_text_get(IDC_RSP_GRDB, o->rsp_grdb, sizeof(o->rsp_grdb));
    shell_text_get(IDC_RSP_LNA, o->rsp_lna, sizeof(o->rsp_lna));
    shell_text_get(IDC_FILE_PATH, o->file_path, sizeof(o->file_path));
    shell_text_get(IDC_FILE_RATE, o->file_rate, sizeof(o->file_rate));
    shell_text_get(IDC_FILE_CENTER, o->file_center, sizeof(o->file_center));
    shell_text_get(IDC_RECORD_PATH, o->record_path, sizeof(o->record_path));
    memcpy(o->channels, g_channels, sizeof(g_channels[0]) * (size_t)g_chan_count);
    o->channel_count = g_chan_count;
}

/** @brief Re-map all state to the preview pane; 0 on valid, -1 on invalid. */
static int refresh_preview(void)
{
    gui_launch_options_t o;
    char err[256], cmd[8192];
    collect_options(&o);
    int ok = gui_options_build_command_line(&o, cmd, sizeof(cmd), err, sizeof(err)) == 0;
    shell_text_set(IDC_PREVIEW, ok ? cmd : "");
    set_status(ok ? "Ready" : err);
    if (!g_running)
        EnableWindow(shell_hwnd(IDC_LAUNCH), ok);
    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Layout: control table, visibility rules, section flow               */
/* ------------------------------------------------------------------ */

#define EDIT_ST (WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP)

/** @brief Every control in the window; geometry is layout input, not position. */
static const shell_ctl_t k_ctls[] = {
    /* chrome */
    {IDC_TAB, -1, WC_TABCONTROL, NULL, WS_CLIPSIBLINGS, 668, 400},
    {IDC_STATUSLINE, -1, "STATIC", "Ready", 0, 470, 16},
    {IDC_LAUNCH, -1, "BUTTON", "Launch", BS_DEFPUSHBUTTON | WS_TABSTOP, 80, 28},
    {IDC_STOP, -1, "BUTTON", "Stop", BS_PUSHBUTTON | WS_TABSTOP, 80, 28},

    /* channels page */
    {IDC_CH_LIST,
     0,
     "LISTBOX",
     NULL,
     WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
     330,
     314},
    {IDC_PREVIEW_L, 0, "STATIC", "Command line", 0, 100, 16},
    {IDC_PREVIEW,
     0,
     "EDIT",
     NULL,
     WS_BORDER | WS_VSCROLL | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL,
     330,
     60},
    {IDC_CH_FREQ_L, 0, "STATIC", "Frequency", 0, 60, 16},
    {IDC_CH_FREQ, 0, "EDIT", NULL, EDIT_ST, 150, 22},
    {IDC_CH_MODE_L, 0, "STATIC", "Mode", 0, 60, 16},
    {IDC_CH_MODE, 0, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, 150, 20},
    {IDC_CH_OUT_L, 0, "STATIC", "Output", 0, 60, 16},
    {IDC_CH_OUT, 0, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, 150, 20},
    {IDC_CH_TCP_L, 0, "STATIC", "TCP port", 0, 60, 16},
    {IDC_CH_TCP_PORT, 0, "EDIT", NULL, EDIT_ST, 150, 22},
    {IDC_CH_UDP_HOST_L, 0, "STATIC", "UDP host", 0, 60, 16},
    {IDC_CH_UDP_HOST, 0, "EDIT", NULL, EDIT_ST, 150, 22},
    {IDC_CH_UDP_PORT_L, 0, "STATIC", "UDP port", 0, 60, 16},
    {IDC_CH_UDP_PORT, 0, "EDIT", NULL, EDIT_ST, 150, 22},
    {IDC_CH_FILE_L, 0, "STATIC", "File", 0, 30, 16},
    {IDC_CH_FILE, 0, "EDIT", NULL, EDIT_ST, 160, 22},
    {IDC_CH_FILE_BROWSE, 0, "BUTTON", "...", BS_PUSHBUTTON | WS_TABSTOP, 40, 28},
    {IDC_CH_ADD, 0, "BUTTON", "Add", BS_PUSHBUTTON | WS_TABSTOP, 70, 28},
    {IDC_CH_UPDATE, 0, "BUTTON", "Update", BS_PUSHBUTTON | WS_TABSTOP, 70, 28},
    {IDC_CH_REMOVE, 0, "BUTTON", "Remove", BS_PUSHBUTTON | WS_TABSTOP, 70, 28},

    /* source page */
    {IDC_GRP_SOURCE, 1, "BUTTON", "Source", BS_GROUPBOX, 632, 0},
    {IDC_BACKEND, 1, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, 240, 20},
    {IDC_DEVICE_L, 1, "STATIC", "Device (-d)", 0, 68, 16},
    {IDC_DEVICE, 1, "EDIT", NULL, EDIT_ST, 110, 22},
    {IDC_DEVICE_HINT, 1, "STATIC", "(blank = first device)", 0, 150, 16},
    {IDC_FILE_PATH_L, 1, "STATIC", "File", 0, 30, 16},
    {IDC_FILE_PATH, 1, "EDIT", NULL, EDIT_ST, 260, 22},
    {IDC_FILE_BROWSE, 1, "BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, 62, 28},
    {IDC_FILE_RATE_L, 1, "STATIC", "Rate:", 0, 34, 16},
    {IDC_FILE_RATE, 1, "EDIT", NULL, EDIT_ST, 60, 22},
    {IDC_FILE_CENTER_L, 1, "STATIC", "Center:", 0, 44, 16},
    {IDC_FILE_CENTER, 1, "EDIT", NULL, EDIT_ST, 80, 22},

    {IDC_GRP_RATES, 1, "BUTTON", "Rates and filters", BS_GROUPBOX, 632, 0},
    {IDC_RATE_IN_L, 1, "STATIC", "Capture rate (-s)", 0, 100, 16},
    {IDC_RATE_IN, 1, "EDIT", NULL, EDIT_ST, 80, 22},
    {IDC_RATE_OUT_L, 1, "STATIC", "Audio rate (-r)", 0, 90, 16},
    {IDC_RATE_OUT, 1, "EDIT", NULL, EDIT_ST, 80, 22},
    {IDC_TIMEOUT_L, 1, "STATIC", "Timeout s (-x)", 0, 86, 16},
    {IDC_TIMEOUT, 1, "EDIT", NULL, EDIT_ST, 50, 22},
    {IDC_DC, 1, "BUTTON", "DC block", BS_AUTOCHECKBOX | WS_TABSTOP, 80, 20},
    {IDC_DEEMP, 1, "BUTTON", "De-emphasis", BS_AUTOCHECKBOX | WS_TABSTOP, 95, 20},

    {IDC_GRP_RX, 1, "BUTTON", "Receiver settings", BS_GROUPBOX, 632, 0},
    {IDC_RTL_GAIN_L, 1, "STATIC", "Gain dB (-g)", 0, 70, 16},
    {IDC_RTL_GAIN, 1, "EDIT", NULL, EDIT_ST | ES_NUMBER, 70, 22},
    {IDC_RTL_PPM_L, 1, "STATIC", "PPM (-p)", 0, 52, 16},
    {IDC_RTL_PPM, 1, "EDIT", NULL, EDIT_ST | ES_NUMBER, 50, 22},
    {IDC_BIASTEE, 1, "BUTTON", "Bias-T (-T)", BS_AUTOCHECKBOX | WS_TABSTOP, 80, 20},
    {IDC_DIRECT_L, 1, "STATIC", "Direct sampling", 0, 90, 16},
    {IDC_DIRECT, 1, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, 140, 20},
    {IDC_RSP_GRDB_L, 1, "STATIC", "gRdB 0-59 (-g)", 0, 90, 16},
    {IDC_RSP_GRDB, 1, "EDIT", NULL, EDIT_ST | ES_NUMBER, 60, 22},
    {IDC_RSP_LNA_L, 1, "STATIC", "LNA state 0-2 (-L)", 0, 106, 16},
    {IDC_RSP_LNA, 1, "EDIT", NULL, EDIT_ST | ES_NUMBER, 50, 22},
    {IDC_RSP_NOTE, 1, "STATIC", "(blank gain = API AGC)", 0, 150, 16},

    {IDC_GRP_RECORD, 1, "BUTTON", "Record raw IQ (-R)", BS_GROUPBOX, 632, 0},
    {IDC_RECORD_L, 1, "STATIC", "CF32 file (blank = off)", 0, 130, 16},
    {IDC_RECORD_PATH, 1, "EDIT", NULL, EDIT_ST, 380, 22},
    {IDC_RECORD_BROWSE, 1, "BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, 62, 28},
};

/* Row/section builders for the layout table; parenthesized compound
 * literals so nested commas survive macro argument splitting */
#define IT(id, w) {id, w}
#define ROW(...)                                                                                   \
    {                                                                                              \
        .items = (const shell_item_t[]) { __VA_ARGS__, {0, 0} }                                    \
    }
#define ROWS(...)                                                                                  \
    (const shell_row_t[])                                                                          \
    {                                                                                              \
        __VA_ARGS__, { .items = NULL }                                                             \
    }
#define GROUP(id, title, x, w, rows_) {id, title, 0, x, w, rows_}
#define PLAIN(col, x, w, rows_) {0, NULL, col, x, w, rows_}

/** @brief Section flow: which sections exist, in what column, in what order. */
static const shell_group_t k_groups[] = {
    /* channels page, left: list + preview */
    PLAIN(
        0,
        24,
        330,
        ROWS(ROW(IT(IDC_CH_LIST, 0)), ROW(IT(IDC_PREVIEW_L, 0)), ROW(IT(IDC_PREVIEW, 0)))),
    /* channels page, right: editor */
    PLAIN(
        1,
        380,
        252,
        ROWS(
            ROW(IT(IDC_CH_FREQ_L, 0), IT(IDC_CH_FREQ, -1)),
            ROW(IT(IDC_CH_MODE_L, 0), IT(IDC_CH_MODE, -1)),
            ROW(IT(IDC_CH_OUT_L, 0), IT(IDC_CH_OUT, -1)),
            ROW(IT(IDC_CH_TCP_L, 0), IT(IDC_CH_TCP_PORT, -1)),
            ROW(IT(IDC_CH_UDP_HOST_L, 0), IT(IDC_CH_UDP_HOST, -1)),
            ROW(IT(IDC_CH_UDP_PORT_L, 0), IT(IDC_CH_UDP_PORT, -1)),
            ROW(IT(IDC_CH_FILE_L, 0), IT(IDC_CH_FILE, -1), IT(IDC_CH_FILE_BROWSE, 0)),
            ROW(IT(IDC_CH_ADD, 0), IT(IDC_CH_UPDATE, 0), IT(IDC_CH_REMOVE, 0)))),
    /* source page */
    GROUP(
        IDC_GRP_SOURCE,
        "Source",
        24,
        632,
        ROWS(
            ROW(IT(IDC_BACKEND, 240)),
            ROW(IT(IDC_DEVICE_L, 0), IT(IDC_DEVICE, 0), IT(IDC_DEVICE_HINT, -1)),
            ROW(IT(IDC_FILE_PATH_L, 0), IT(IDC_FILE_PATH, -1), IT(IDC_FILE_BROWSE, 0)),
            ROW(IT(IDC_FILE_RATE_L, 0),
                IT(IDC_FILE_RATE, 0),
                IT(IDC_FILE_CENTER_L, 0),
                IT(IDC_FILE_CENTER, 0)))),
    GROUP(
        IDC_GRP_RATES,
        "Rates and filters",
        24,
        632,
        ROWS(
            ROW(IT(IDC_RATE_IN_L, 0),
                IT(IDC_RATE_IN, 0),
                IT(IDC_RATE_OUT_L, 0),
                IT(IDC_RATE_OUT, 0),
                IT(IDC_TIMEOUT_L, 0),
                IT(IDC_TIMEOUT, 0)),
            ROW(IT(IDC_DC, 0), IT(IDC_DEEMP, 0)))),
    GROUP(
        IDC_GRP_RX,
        "Receiver settings",
        24,
        632,
        ROWS(
            ROW(IT(IDC_RTL_GAIN_L, 0),
                IT(IDC_RTL_GAIN, 0),
                IT(IDC_RTL_PPM_L, 0),
                IT(IDC_RTL_PPM, 0),
                IT(IDC_BIASTEE, 0),
                IT(IDC_DIRECT_L, 0),
                IT(IDC_DIRECT, -1)),
            ROW(IT(IDC_RSP_GRDB_L, 0),
                IT(IDC_RSP_GRDB, 0),
                IT(IDC_RSP_LNA_L, 0),
                IT(IDC_RSP_LNA, 0),
                IT(IDC_RSP_NOTE, -1)))),
    GROUP(
        IDC_GRP_RECORD,
        "Record raw IQ (-R)",
        24,
        632,
        ROWS(ROW(IT(IDC_RECORD_L, 0), IT(IDC_RECORD_PATH, -1), IT(IDC_RECORD_BROWSE, 0)))),
};

#define K_GROUPS_N (sizeof(k_groups) / sizeof(k_groups[0]))

/**
 * @brief The single source of truth for control visibility; the layout
 * engine reflows (never overlaps) whatever this returns.
 */
static int is_visible(int id)
{
    int src = shell_combo_sel(IDC_BACKEND);
    if (src < 0)
        src = GUI_SRC_RTL;
    int file = src == GUI_SRC_FILE, rtl = src == GUI_SRC_RTL;
    switch (id)
    {
    /* file replay has no hardware: hide device select, gains, capture rate */
    case IDC_DEVICE_L:
    case IDC_DEVICE:
    case IDC_DEVICE_HINT:
    case IDC_GRP_RX:
    case IDC_RATE_IN_L:
    case IDC_RATE_IN:
        return !file;
    case IDC_RTL_GAIN_L:
    case IDC_RTL_GAIN:
    case IDC_RTL_PPM_L:
    case IDC_RTL_PPM:
    case IDC_BIASTEE:
    case IDC_DIRECT_L:
    case IDC_DIRECT:
        return !file && rtl;
    case IDC_RSP_GRDB_L:
    case IDC_RSP_GRDB:
    case IDC_RSP_LNA_L:
    case IDC_RSP_LNA:
    case IDC_RSP_NOTE:
        return !file && !rtl;
    case IDC_FILE_PATH_L:
    case IDC_FILE_PATH:
    case IDC_FILE_BROWSE:
    case IDC_FILE_RATE_L:
    case IDC_FILE_RATE:
    case IDC_FILE_CENTER_L:
    case IDC_FILE_CENTER:
        return file;

    /* channel editor: fields of the selected output type */
    case IDC_CH_TCP_L:
    case IDC_CH_TCP_PORT:
        return g_edit.output == GUI_OUT_TCP;
    case IDC_CH_UDP_HOST_L:
    case IDC_CH_UDP_HOST:
    case IDC_CH_UDP_PORT_L:
    case IDC_CH_UDP_PORT:
        return g_edit.output == GUI_OUT_UDP;
    case IDC_CH_FILE_L:
    case IDC_CH_FILE:
    case IDC_CH_FILE_BROWSE:
        return g_edit.output == GUI_OUT_FILE;
    default:
        return 1;
    }
}

/** @brief Re-run the layout and size the tab, bottom bar, and window to it. */
static void relayout(void)
{
    int page = TabCtrl_GetCurSel(shell_hwnd(IDC_TAB));
    if (page < 0)
        page = PAGE_CHANNELS;
    int bottom = shell_layout(g_main, page, k_groups, (int)K_GROUPS_N, is_visible);

    SetWindowPos(
        shell_hwnd(IDC_TAB),
        NULL,
        0,
        0,
        668,
        bottom + 14,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    int bar_y = 6 + bottom + 14 + 12;
    MoveWindow(shell_hwnd(IDC_STATUSLINE), 14, bar_y + 6, 470, 16, TRUE);
    MoveWindow(shell_hwnd(IDC_LAUNCH), 500, bar_y, 80, 28, TRUE);
    MoveWindow(shell_hwnd(IDC_STOP), 590, bar_y, 80, 28, TRUE);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, 680, bar_y + 28 + 12};
    AdjustWindowRect(&r, style, FALSE);
    SetWindowPos(
        g_main,
        NULL,
        0,
        0,
        r.right - r.left,
        r.bottom - r.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ------------------------------------------------------------------ */
/* Channel list                                                        */
/* ------------------------------------------------------------------ */

/** @brief Human-readable list entry for channel i. */
static void channel_label(int i, char *buf, size_t n)
{
    static const char *modes[] = {"fm", "wbfm", "am", "usb", "lsb", "raw"};
    const gui_channel_t *c = &g_channels[i];
    char out[300];
    if (c->output == GUI_OUT_TCP)
        snprintf(out, sizeof(out), "tcp:%s", c->tcp_port);
    else if (c->output == GUI_OUT_UDP)
        snprintf(out, sizeof(out), "udp:%s:%s", c->udp_host, c->udp_port);
    else
        snprintf(out, sizeof(out), "%s", c->file_name);
    snprintf(buf, n, "%.31s   %.5s   ->   %.299s", c->freq, modes[c->mode], out);
}

/** @brief Rebuild the list box from g_channels, keeping a selection. */
static void reload_channel_list(int select)
{
    HWND list = shell_hwnd(IDC_CH_LIST);
    int keep = select < 0 ? ListBox_GetCurSel(list) : select;
    ListBox_ResetContent(list);
    char buf[512];
    for (int i = 0; i < g_chan_count; i++)
    {
        channel_label(i, buf, sizeof(buf));
        ListBox_AddString(list, buf);
    }
    if (keep >= 0 && keep < g_chan_count)
        ListBox_SetCurSel(list, keep);
}

/** @brief Load channel i into the editor pane. */
static void select_channel(int i)
{
    if (i < 0 || i >= g_chan_count)
        return;
    g_edit = g_channels[i];
    load_editor();
    relayout();
}

/* ------------------------------------------------------------------ */
/* Child process management                                            */
/* ------------------------------------------------------------------ */

/** @brief Watcher thread: waits for the child and notifies the UI. */
static DWORD WINAPI child_watch(LPVOID param)
{
    HANDLE h = (HANDLE)param;
    WaitForSingleObject(h, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    PostMessage(g_main, WM_CHILD_EXIT, 0, (LPARAM)code);
    return 0;
}

/** @brief Set the running/stopped UI state (button enables, g_running). */
static void set_running_ui(int running)
{
    g_running = running;
    EnableWindow(shell_hwnd(IDC_LAUNCH), !running);
    EnableWindow(shell_hwnd(IDC_STOP), running);
}

/** @brief Validate, then spawn the configured backend binary. */
static void on_launch(void)
{
    if (g_running || !refresh_preview())
        return;
    gui_launch_options_t o;
    char cmd[8192], full[8192 + 300], dir[MAX_PATH], err[256];
    collect_options(&o);
    if (gui_options_build_command_line(&o, cmd, sizeof(cmd), err, sizeof(err)) != 0)
    {
        set_status(err);
        return;
    }
    GetModuleFileNameA(NULL, dir, sizeof(dir));
    char *slash = strrchr(dir, '\\');
    if (slash)
        *slash = '\0';
    else
        strcpy(dir, ".");

    /* mapper emits "exe args..."; swap the bare exe name for the full path */
    const char *bin = o.backend == GUI_BACKEND_RSP ? "rsp_multi.exe" : "rtl_multi.exe";
    const char *rest = strchr(cmd, ' ');
    snprintf(full, sizeof(full), "\"%s\\%s\" %s", dir, bin, rest ? rest + 1 : "");

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, full, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi))
    {
        snprintf(err, sizeof(err), "Failed to launch %s (error %lu)", bin, GetLastError());
        set_status(err);
        return;
    }
    g_pi = pi;
    CloseHandle(pi.hThread);
    set_running_ui(1);
    char status[128];
    snprintf(
        status,
        sizeof(status),
        "Running: %s (PID %lu) - click Stop to terminate",
        bin,
        (unsigned long)pi.dwProcessId);
    set_status(status);
    CreateThread(NULL, 0, child_watch, pi.hProcess, 0, NULL);
}

/** @brief Terminate the running child (no graceful-stop IPC exists). */
static void on_stop(void)
{
    if (!g_running)
        return;
    TerminateProcess(g_pi.hProcess, (UINT)-1);
    set_status("Stopping...");
}

/* ------------------------------------------------------------------ */
/* Window procedure: event routing                                     */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CREATE:
        g_main = hwnd;
        shell_create(hwnd, k_ctls, (int)(sizeof(k_ctls) / sizeof(k_ctls[0])));
        shell_combo_add(IDC_BACKEND, "RTL-SDR (rtl_multi)");
        shell_combo_add(IDC_BACKEND, "SDRplay RSP (rsp_multi)");
        shell_combo_add(IDC_BACKEND, "IQ file replay (-I)");
        ComboBox_SetCurSel(shell_hwnd(IDC_BACKEND), 0);
        shell_combo_add(IDC_CH_MODE, "FM (narrow)");
        shell_combo_add(IDC_CH_MODE, "WBFM");
        shell_combo_add(IDC_CH_MODE, "AM");
        shell_combo_add(IDC_CH_MODE, "USB");
        shell_combo_add(IDC_CH_MODE, "LSB");
        shell_combo_add(IDC_CH_MODE, "RAW");
        shell_combo_add(IDC_CH_OUT, "TCP");
        shell_combo_add(IDC_CH_OUT, "UDP");
        shell_combo_add(IDC_CH_OUT, "File");
        SendMessage(shell_hwnd(IDC_PREVIEW), WM_SETFONT, (WPARAM)shell_apply_fonts(hwnd), FALSE);
        /* one ready-made starting channel */
        strcpy(g_channels[0].freq, "100M");
        g_channels[0].mode = GUI_MODE_FM;
        g_channels[0].output = GUI_OUT_TCP;
        strcpy(g_channels[0].tcp_port, "8100");
        g_chan_count = 1;
        g_edit.output = GUI_OUT_TCP;
        reload_channel_list(0);
        load_editor();
        relayout();
        refresh_preview();
        EnableWindow(shell_hwnd(IDC_STOP), FALSE);
        return 0;

    case WM_NOTIFY:
    {
        NMHDR *nm = (NMHDR *)lparam;
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE)
            relayout();
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wparam), code = HIWORD(wparam);
        switch (id)
        {
        case IDC_BACKEND:
            if (code == CBN_SELCHANGE)
            {
                relayout();
                refresh_preview();
            }
            return 0;
        case IDC_CH_OUT:
            if (code == CBN_SELCHANGE)
            {
                g_edit.output = (gui_output_type_t)shell_combo_sel(IDC_CH_OUT);
                relayout();
                refresh_preview();
            }
            return 0;
        case IDC_CH_LIST:
            if (code == LBN_SELCHANGE)
            {
                select_channel(ListBox_GetCurSel(shell_hwnd(IDC_CH_LIST)));
                refresh_preview();
            }
            return 0;
        case IDC_FILE_BROWSE:
            shell_browse(g_main, IDC_FILE_PATH, 0);
            refresh_preview();
            return 0;
        case IDC_CH_FILE_BROWSE:
            shell_browse(g_main, IDC_CH_FILE, 0);
            refresh_preview();
            return 0;
        case IDC_RECORD_BROWSE:
            shell_browse(g_main, IDC_RECORD_PATH, 1);
            refresh_preview();
            return 0;
        case IDC_CH_ADD:
            if (g_chan_count >= GUI_MAX_CHANNELS)
            {
                set_status("Channel limit reached (128)");
                return 0;
            }
            editor_to_edit();
            g_channels[g_chan_count++] = g_edit;
            reload_channel_list(g_chan_count - 1);
            refresh_preview();
            return 0;
        case IDC_CH_UPDATE:
        {
            int sel = ListBox_GetCurSel(shell_hwnd(IDC_CH_LIST));
            if (sel < 0)
            {
                set_status("Select a channel to update");
                return 0;
            }
            editor_to_edit();
            g_channels[sel] = g_edit;
            reload_channel_list(sel);
            refresh_preview();
            return 0;
        }
        case IDC_CH_REMOVE:
        {
            int sel = ListBox_GetCurSel(shell_hwnd(IDC_CH_LIST));
            if (sel < 0)
            {
                set_status("Select a channel to remove");
                return 0;
            }
            memmove(
                &g_channels[sel],
                &g_channels[sel + 1],
                sizeof(g_channels[0]) * (size_t)(g_chan_count - sel - 1));
            g_chan_count--;
            if (g_chan_count == 0)
                memset(&g_edit, 0, sizeof(g_edit));
            else
                select_channel(sel < g_chan_count ? sel : g_chan_count - 1);
            reload_channel_list(sel < g_chan_count ? sel : g_chan_count - 1);
            relayout();
            refresh_preview();
            return 0;
        }
        case IDC_LAUNCH:
            on_launch();
            return 0;
        case IDC_STOP:
            on_stop();
            return 0;
        case IDC_PREVIEW:
            return 0; /* readonly; ignore its EN_CHANGE to avoid refresh loops */
        default:
            refresh_preview();
            return 0;
        }
    }

    case WM_CHILD_EXIT:
    {
        char status[128];
        snprintf(status, sizeof(status), "Backend exited (code %lu)", (unsigned long)lparam);
        set_running_ui(0);
        refresh_preview();
        set_status(status); /* after refresh_preview, which resets the line */
        return 0;
    }

    case WM_CLOSE:
        fprintf(stderr, "wnd_proc: WM_CLOSE (g_running=%d)\n", g_running);
        if (g_running && MessageBoxA(
                             hwnd,
                             "A backend process is running.\n"
                             "Stop it and exit rxx_gui?",
                             "rxx_multi GUI",
                             MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        fprintf(stderr, "wnd_proc: WM_DESTROY\n");
        if (g_running)
        {
            TerminateProcess(g_pi.hProcess, (UINT)-1);
            WaitForSingleObject(g_pi.hProcess, 2000);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmdline, int show)
{
    (void)prev;
    (void)cmdline;
    return shell_run(instance, "rxx_gui", "rxx_multi GUI", 680, wnd_proc, show);
}
