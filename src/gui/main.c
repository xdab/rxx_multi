/**
 * @file main.c
 * @brief rxx_gui — Windows launcher UI for rxx_multi.
 *
 * Tabbed layout: the "Channels" tab holds the working set (channel list,
 * editor, live command-line preview); the "Device and input" tab holds the
 * set-once settings. A persistent bottom bar carries status + Launch/Stop.
 * All state maps to a backend command line through args_mapper.h; Launch/
 * Stop manage the backend child process. Generic Win32 plumbing lives in
 * shell.c.
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
    IDC_TAB = 100,
    IDC_BACKEND,
    IDC_DEVICE,
    IDC_RATE_IN,
    IDC_RATE_OUT,
    IDC_TIMEOUT,
    IDC_DC,
    IDC_DEEMP,
    IDC_RTL_GAIN,
    IDC_RTL_GAIN_L,
    IDC_RTL_PPM,
    IDC_RTL_PPM_L,
    IDC_BIASTEE,
    IDC_DIRECT,
    IDC_DIRECT_L,
    IDC_RSP_GRDB,
    IDC_RSP_GRDB_L,
    IDC_RSP_LNA,
    IDC_RSP_LNA_L,
    IDC_RSP_NOTE,
    IDC_SRC_DEVICE,
    IDC_SRC_FILE,
    IDC_FILE_PATH,
    IDC_FILE_BROWSE,
    IDC_FILE_RATE,
    IDC_FILE_CENTER,
    IDC_RECORD_PATH,
    IDC_RECORD_BROWSE,
    IDC_CH_LIST,
    IDC_CH_FREQ,
    IDC_CH_MODE,
    IDC_CH_OUT,
    IDC_CH_LBL1,
    IDC_CH_FLD1,
    IDC_CH_LBL2,
    IDC_CH_FLD2,
    IDC_CH_ADD,
    IDC_CH_UPDATE,
    IDC_CH_REMOVE,
    IDC_LAUNCH,
    IDC_STOP,
    IDC_PREVIEW,
    IDC_STATUSLINE,
    IDC_STATUS, /* dummy ID for labels that need no handle */
    IDC_COUNT
};

/** @brief Tab pages. */
enum
{
    PAGE_CHANNELS = 0,
    PAGE_DEVICE = 1
};

static void sync_backend(void);
static void sync_input_source(void);
static void load_edit_fields(void);

/** @brief Main window handle (owner for dialogs, target for notifications). */
static HWND g_main;
/** @brief Channel list master state (the -f/-M/-O lists, positional). */
static gui_channel_t g_channels[GUI_MAX_CHANNELS];
static int g_chan_count;
/** @brief Working copy of the channel currently in the editor pane. */
static gui_channel_t g_edit;
/** @brief Running child process state. */
static PROCESS_INFORMATION g_pi;
static int g_running;

#define WM_CHILD_EXIT (WM_APP + 1)

/* ------------------------------------------------------------------ */
/* Live preview + status                                               */
/* ------------------------------------------------------------------ */

/** @brief Set the one-line status text. */
static void set_status(const char *s) { shell_text_set(IDC_STATUSLINE, s); }

/** @brief Copy every control's value into a launch-options struct. */
static void collect_options(gui_launch_options_t *o)
{
    memset(o, 0, sizeof(*o));
    o->backend = (gui_backend_t)shell_combo_sel(IDC_BACKEND);
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
    o->use_file_input = shell_checked(IDC_SRC_FILE);
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
/* Channel editor state                                                */
/* ------------------------------------------------------------------ */

/** @brief Show the flag set matching the selected backend. */
static void sync_backend(void)
{
    int rtl = shell_combo_sel(IDC_BACKEND) == GUI_BACKEND_RTL;
    shell_show_ids(
        rtl,
        7,
        IDC_RTL_GAIN,
        IDC_RTL_GAIN_L,
        IDC_RTL_PPM,
        IDC_RTL_PPM_L,
        IDC_BIASTEE,
        IDC_DIRECT,
        IDC_DIRECT_L);
    shell_show_ids(!rtl, 5, IDC_RSP_GRDB, IDC_RSP_GRDB_L, IDC_RSP_LNA, IDC_RSP_LNA_L, IDC_RSP_NOTE);
}

/** @brief Enable file-input fields only when the IQ-file radio is picked. */
static void sync_input_source(void)
{
    int file = shell_checked(IDC_SRC_FILE);
    shell_enable_ids(file, 4, IDC_FILE_PATH, IDC_FILE_BROWSE, IDC_FILE_RATE, IDC_FILE_CENTER);
}

/** @brief Save the visible dynamic editor fields into g_edit. */
static void stash_edit_fields(void)
{
    switch (g_edit.output)
    {
    case GUI_OUT_TCP:
        shell_text_get(IDC_CH_FLD1, g_edit.tcp_port, sizeof(g_edit.tcp_port));
        break;
    case GUI_OUT_UDP:
        shell_text_get(IDC_CH_FLD1, g_edit.udp_host, sizeof(g_edit.udp_host));
        shell_text_get(IDC_CH_FLD2, g_edit.udp_port, sizeof(g_edit.udp_port));
        break;
    case GUI_OUT_FILE:
        shell_text_get(IDC_CH_FLD1, g_edit.file_name, sizeof(g_edit.file_name));
        break;
    }
}

/** @brief Show the dynamic editor fields for g_edit's output type. */
static void load_edit_fields(void)
{
    switch (g_edit.output)
    {
    case GUI_OUT_TCP:
        shell_text_set(IDC_CH_LBL1, "TCP port");
        shell_text_set(IDC_CH_FLD1, g_edit.tcp_port);
        shell_show_ids(0, 2, IDC_CH_LBL2, IDC_CH_FLD2);
        break;
    case GUI_OUT_UDP:
        shell_text_set(IDC_CH_LBL1, "UDP host");
        shell_text_set(IDC_CH_FLD1, g_edit.udp_host);
        shell_text_set(IDC_CH_LBL2, "UDP port");
        shell_text_set(IDC_CH_FLD2, g_edit.udp_port);
        shell_show_ids(1, 2, IDC_CH_LBL2, IDC_CH_FLD2);
        break;
    case GUI_OUT_FILE:
        shell_text_set(IDC_CH_LBL1, "File");
        shell_text_set(IDC_CH_FLD1, g_edit.file_name);
        shell_show_ids(0, 2, IDC_CH_LBL2, IDC_CH_FLD2);
        break;
    }
}

/** @brief Read freq/mode/output + dynamic fields from the editor into g_edit. */
static void editor_to_edit(void)
{
    shell_text_get(IDC_CH_FREQ, g_edit.freq, sizeof(g_edit.freq));
    g_edit.mode = (gui_mode_t)shell_combo_sel(IDC_CH_MODE);
    g_edit.output = (gui_output_type_t)shell_combo_sel(IDC_CH_OUT);
    stash_edit_fields();
}

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
    stash_edit_fields();
    if (i < 0 || i >= g_chan_count)
        return;
    g_edit = g_channels[i];
    shell_text_set(IDC_CH_FREQ, g_edit.freq);
    ComboBox_SetCurSel(shell_hwnd(IDC_CH_MODE), g_edit.mode);
    ComboBox_SetCurSel(shell_hwnd(IDC_CH_OUT), g_edit.output);
    load_edit_fields();
}

/** @brief Switch tab page, then re-apply that page's visibility rules. */
static void show_page(int page)
{
    shell_show_page(page);
    if (page == PAGE_DEVICE)
    {
        sync_backend();
        sync_input_source();
    }
    else
    {
        load_edit_fields();
    }
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
/* UI construction (fixed coordinates; P = tab page)                   */
/* ------------------------------------------------------------------ */

#define ADDP(p, cls, text, style, x, y, w, h, id)                                                  \
    shell_add(hwnd, p, id, cls, text, style, x, y, w, h)
#define EDITP(p, id, x, y, w, extra)                                                               \
    ADDP(p, "EDIT", NULL, WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP | (extra), x, y, w, 22, id)
#define COMBOP(p, id, x, y, w)                                                                     \
    ADDP(p, "COMBOBOX", NULL, CBS_DROPDOWNLIST | WS_TABSTOP, x, y, w, 120, id)
#define LABELP(p, txt, x, y, w) ADDP(p, "STATIC", txt, 0, x, y, w, 16, IDC_STATUS)
#define LABELID(p, txt, x, y, w, id) ADDP(p, "STATIC", txt, 0, x, y, w, 16, id)
#define CHECKP(p, txt, x, y, w, id)                                                                \
    ADDP(p, "BUTTON", txt, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 20, id)
#define PUSHP(p, txt, x, y, w, id)                                                                 \
    ADDP(p, "BUTTON", txt, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, 28, id)
#define GROUPP(p, txt, x, y, w, h) ADDP(p, "BUTTON", txt, BS_GROUPBOX, x, y, w, h, IDC_STATUS)

/** @brief Create the tab control, both pages, and the bottom bar. */
static void build_ui(HWND hwnd)
{
    ADDP(-1, WC_TABCONTROL, NULL, WS_CLIPSIBLINGS, 6, 6, 668, 486, IDC_TAB);
    TCITEMA ti = {0};
    ti.mask = TCIF_TEXT;
    ti.pszText = (LPSTR) "Channels";
    TabCtrl_InsertItem(shell_hwnd(IDC_TAB), 0, &ti);
    ti.pszText = (LPSTR) "Device and input";
    TabCtrl_InsertItem(shell_hwnd(IDC_TAB), 1, &ti);

    /* ---- Page 0: Channels ---------------------------------------- */
    ADDP(
        0,
        "LISTBOX",
        NULL,
        WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
        24,
        48,
        330,
        314,
        IDC_CH_LIST);
    LABELP(0, "Frequency", 380, 50, 150);
    EDITP(0, IDC_CH_FREQ, 380, 66, 150, 0);
    LABELP(0, "Mode", 380, 96, 150);
    COMBOP(0, IDC_CH_MODE, 380, 112, 150);
    shell_combo_add(IDC_CH_MODE, "FM (narrow)");
    shell_combo_add(IDC_CH_MODE, "WBFM");
    shell_combo_add(IDC_CH_MODE, "AM");
    shell_combo_add(IDC_CH_MODE, "USB");
    shell_combo_add(IDC_CH_MODE, "LSB");
    shell_combo_add(IDC_CH_MODE, "RAW");
    ComboBox_SetCurSel(shell_hwnd(IDC_CH_MODE), 0);
    LABELP(0, "Output", 380, 142, 150);
    COMBOP(0, IDC_CH_OUT, 380, 158, 150);
    shell_combo_add(IDC_CH_OUT, "TCP");
    shell_combo_add(IDC_CH_OUT, "UDP");
    shell_combo_add(IDC_CH_OUT, "File");
    ComboBox_SetCurSel(shell_hwnd(IDC_CH_OUT), 0);
    LABELID(0, "TCP port", 380, 192, 150, IDC_CH_LBL1);
    EDITP(0, IDC_CH_FLD1, 380, 208, 220, 0);
    LABELID(0, "UDP port", 380, 238, 150, IDC_CH_LBL2);
    EDITP(0, IDC_CH_FLD2, 380, 254, 220, 0);
    PUSHP(0, "Add", 380, 292, 70, IDC_CH_ADD);
    PUSHP(0, "Update", 458, 292, 70, IDC_CH_UPDATE);
    PUSHP(0, "Remove", 536, 292, 78, IDC_CH_REMOVE);
    LABELP(0, "Command line", 24, 372, 150);
    ADDP(
        0,
        "EDIT",
        NULL,
        WS_BORDER | WS_VSCROLL | ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL,
        24,
        390,
        632,
        60,
        IDC_PREVIEW);

    /* ---- Page 1: Device and input -------------------------------- */
    GROUPP(1, "Device", 24, 42, 632, 70);
    LABELP(1, "Backend", 40, 62, 52);
    COMBOP(1, IDC_BACKEND, 100, 58, 220);
    shell_combo_add(IDC_BACKEND, "RTL-SDR (rtl_multi)");
    shell_combo_add(IDC_BACKEND, "SDRplay RSP (rsp_multi)");
    ComboBox_SetCurSel(shell_hwnd(IDC_BACKEND), 0);
    LABELP(1, "Device (-d)", 348, 62, 68);
    EDITP(1, IDC_DEVICE, 420, 58, 120, 0);
    LABELP(1, "(blank = first device)", 424, 84, 140);

    GROUPP(1, "Rates and filters", 24, 120, 632, 70);
    LABELP(1, "Capture rate (-s)", 40, 138, 100);
    EDITP(1, IDC_RATE_IN, 40, 154, 80, 0);
    LABELP(1, "Audio rate (-r)", 160, 138, 90);
    EDITP(1, IDC_RATE_OUT, 160, 154, 80, 0);
    LABELP(1, "Timeout s (-x)", 280, 138, 86);
    EDITP(1, IDC_TIMEOUT, 280, 154, 50, 0);
    CHECKP(1, "DC block", 380, 152, 80, IDC_DC);
    CHECKP(1, "De-emphasis", 468, 152, 95, IDC_DEEMP);

    GROUPP(1, "Receiver settings", 24, 198, 632, 70);
    LABELP(1, "Gain dB (-g)", 40, 216, 76);
    EDITP(1, IDC_RTL_GAIN, 40, 232, 70, ES_NUMBER);
    LABELP(1, "PPM (-p)", 160, 216, 56);
    EDITP(1, IDC_RTL_PPM, 160, 232, 50, ES_NUMBER);
    CHECKP(1, "Bias-T (-T)", 240, 230, 82, IDC_BIASTEE);
    LABELP(1, "Direct sampling", 344, 216, 92);
    COMBOP(1, IDC_DIRECT, 344, 232, 140);
    shell_combo_add(IDC_DIRECT, "Off");
    shell_combo_add(IDC_DIRECT, "Direct (I)");
    shell_combo_add(IDC_DIRECT, "Direct (Q)");
    ComboBox_SetCurSel(shell_hwnd(IDC_DIRECT), 0);
    LABELP(1, "gRdB 0-59 (-g)", 40, 216, 92);
    EDITP(1, IDC_RSP_GRDB, 40, 232, 60, ES_NUMBER);
    LABELP(1, "LNA state 0-2 (-L)", 160, 216, 106);
    EDITP(1, IDC_RSP_LNA, 160, 232, 50, ES_NUMBER);
    LABELP(1, "(blank gain = API AGC)", 250, 232, 150);

    GROUPP(1, "Input source", 24, 276, 632, 96);
    ADDP(
        1,
        "BUTTON",
        "SDR device",
        BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
        40,
        292,
        100,
        20,
        IDC_SRC_DEVICE);
    ADDP(
        1,
        "BUTTON",
        "IQ file (-I)",
        BS_AUTORADIOBUTTON | WS_TABSTOP,
        160,
        292,
        95,
        20,
        IDC_SRC_FILE);
    Button_SetCheck(shell_hwnd(IDC_SRC_DEVICE), BST_CHECKED);
    LABELP(1, "Path:", 40, 318, 34);
    EDITP(1, IDC_FILE_PATH, 78, 314, 360, 0);
    PUSHP(1, "Browse...", 448, 312, 62, IDC_FILE_BROWSE);
    LABELP(1, "Rate:", 40, 344, 34);
    EDITP(1, IDC_FILE_RATE, 78, 340, 90, 0);
    LABELP(1, "Center:", 190, 344, 44);
    EDITP(1, IDC_FILE_CENTER, 238, 340, 90, 0);
    LABELP(1, "(e.g. 2M and 100M)", 344, 344, 120);

    GROUPP(1, "Record raw IQ (-R)", 24, 380, 632, 66);
    LABELP(1, "CF32 file (blank = off)", 40, 398, 130);
    EDITP(1, IDC_RECORD_PATH, 40, 414, 380, 0);
    PUSHP(1, "Browse...", 430, 412, 62, IDC_RECORD_BROWSE);

    /* ---- Bottom bar: status + launch ------------------------------ */
    LABELP(-1, "Ready", 14, 506, 470);
    ADDP(-1, "BUTTON", "Launch", BS_DEFPUSHBUTTON | WS_TABSTOP, 500, 498, 80, 28, IDC_LAUNCH);
    ADDP(-1, "BUTTON", "Stop", BS_PUSHBUTTON | WS_TABSTOP, 590, 498, 80, 28, IDC_STOP);
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
        build_ui(hwnd);
        SendMessage(shell_hwnd(IDC_PREVIEW), WM_SETFONT, (WPARAM)shell_apply_fonts(hwnd), FALSE);
        sync_backend();
        sync_input_source();
        /* one ready-made starting channel */
        strcpy(g_channels[0].freq, "100M");
        g_channels[0].mode = GUI_MODE_FM;
        g_channels[0].output = GUI_OUT_TCP;
        strcpy(g_channels[0].tcp_port, "8100");
        g_chan_count = 1;
        g_edit.output = GUI_OUT_TCP;
        reload_channel_list(0);
        select_channel(0);
        show_page(PAGE_CHANNELS);
        refresh_preview();
        EnableWindow(shell_hwnd(IDC_STOP), FALSE);
        return 0;

    case WM_NOTIFY:
    {
        NMHDR *nm = (NMHDR *)lparam;
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE)
            show_page(TabCtrl_GetCurSel(shell_hwnd(IDC_TAB)));
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
                sync_backend();
                refresh_preview();
            }
            return 0;
        case IDC_SRC_DEVICE:
        case IDC_SRC_FILE:
            if (code == BN_CLICKED)
            {
                sync_input_source();
                refresh_preview();
            }
            return 0;
        case IDC_CH_OUT:
            if (code == CBN_SELCHANGE)
            {
                stash_edit_fields();
                g_edit.output = (gui_output_type_t)shell_combo_sel(IDC_CH_OUT);
                load_edit_fields();
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
            {
                memset(&g_edit, 0, sizeof(g_edit));
                shell_text_set(IDC_CH_FREQ, "");
            }
            reload_channel_list(sel < g_chan_count ? sel : g_chan_count - 1);
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
    return shell_run(instance, "rxx_gui", "rxx_multi GUI", 680, 540, wnd_proc, show);
}
